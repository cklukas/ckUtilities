// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_operation_copy_job.hpp"

#include "directory_cursor.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "opened_entry.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

Observation<bool> unavailable_bool(const UnavailableObservation& detail) {
    return Observation<bool>::unavailable(detail.state, detail.error);
}

bool same(const ObjectIdentity& left, const ObjectIdentity& right) noexcept {
    return compare_identity(left, right) == IdentityComparison::Same;
}

std::optional<NativeName> generated_name(NameEncoding encoding, const char* prefix,
                                         const OperationJournalId& id, std::size_t item) {
    std::string text = std::string(prefix);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : id.bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        text.push_back(hex[value >> 4U]);
        text.push_back(hex[value & 0x0FU]);
    }
    text.push_back('-');
    text += std::to_string(item);
    if (encoding == NameEncoding::PosixBytes)
        return NativeName::from_posix(text);
    if (encoding == NameEncoding::WindowsUtf16) {
        std::u16string units;
        units.reserve(text.size());
        for (const auto character : text)
            units.push_back(static_cast<char16_t>(static_cast<unsigned char>(character)));
        return NativeName::from_windows(std::move(units));
    }
    return std::nullopt;
}

OperationJournalId item_journal_id(OperationJournalId base, std::size_t item) noexcept {
    const auto value = static_cast<std::uint64_t>(item);
    for (unsigned byte = 0; byte != sizeof(value); ++byte)
        base.bytes[8 + byte] = std::byte{static_cast<unsigned char>(value >> (byte * 8U))};
    return base;
}

OperationJournalId tree_child_journal_id(OperationJournalId base, std::size_t item) noexcept {
    // The regular batch factory reserves bytes 8..15 for its local item
    // number. Keep the recursive entry number in the preceding bytes so each
    // child journal remains unique even though every child batch has one item.
    const auto value = static_cast<std::uint64_t>(item);
    for (unsigned byte = 0; byte != sizeof(value) - 1; ++byte)
        base.bytes[1 + byte] = std::byte{static_cast<unsigned char>(value >> (byte * 8U))};
    return base;
}

} // namespace

struct NativeOperationCopyJob::State final {
    explicit State(NativeOperationCopyRequest value) : request(std::move(value)) {}

    NativeOperationCopyRequest request;
    std::unique_ptr<OpenedEntry> source;
    std::unique_ptr<OpenedEntry> staging;
    bool source_exclusion = false;

    Observation<bool> cleanup_unpublished_staging() {
        const auto journal_state = request.journal->state(request.item);
        if (!journal_state)
            return rejected<bool>(AdapterError::InvalidNativeResult,
                                  "native_operation_copy.cleanup_journal_state");
        switch (*journal_state) {
        case OperationJournalState::Planned:
        case OperationJournalState::Prepared:
        case OperationJournalState::Copied:
        case OperationJournalState::Verified:
        case OperationJournalState::Failed:
        case OperationJournalState::Cancelled:
            break;
        default:
            return rejected<bool>(AdapterError::PlanDecisionStale,
                                  "native_operation_copy.cleanup_ambiguous", ObservationState::Unknown);
        }
        if (!staging)
            return Observation<bool>::available(true);
        const auto metadata = staging->inspect_basic();
        if (!metadata.value())
            return unavailable_bool(*metadata.unavailable_detail());
        if (!metadata.value()->identity.value() || !metadata.value()->kind.value() ||
            *metadata.value()->kind.value() != ObjectKind::RegularFile)
            return rejected<bool>(AdapterError::MetadataChanged, "native_operation_copy.cleanup_kind",
                                  ObservationState::Changed);
        auto discarded = request.destination->discard_child(
            request.staging_name, *metadata.value()->identity.value());
        if (!discarded.value())
            return unavailable_bool(*discarded.unavailable_detail());
        if (!*discarded.value())
            return rejected<bool>(AdapterError::InvalidNativeResult,
                                  "native_operation_copy.cleanup_result");
        staging.reset();
        return Observation<bool>::available(true);
    }
};

Observation<NativeOperationCopyJob::Pointer> NativeOperationCopyJob::create(
    NativeOperationCopyRequest request) {
    if (!request.journal || !request.journal_file || request.item >= request.journal->limits().max_items ||
        (request.operation != FileOperation::Copy && request.operation != FileOperation::Move) ||
        request.journal->operation() != request.operation ||
        request.journal->state(request.item) != OperationJournalState::Planned ||
        !request.source_parent || !request.destination || !request.source_name.is_valid() ||
        !request.staging_name.is_valid() ||
        !request.source_parent_identity.scope.is_valid() ||
        !request.destination_name.is_valid() || !request.source_identity.scope.is_valid() ||
        request.source_identity.scheme == IdentityScheme::Unknown ||
        !request.destination_identity.scope.is_valid() ||
        request.destination_identity.scheme == IdentityScheme::Unknown ||
        !NativeDurableIdentity::from_live(request.source_parent_identity) ||
        !NativeDurableIdentity::from_live(request.source_identity) ||
        !NativeDurableIdentity::from_live(request.destination_identity) ||
        !request.prepare_metadata || !request.copy_limits.valid())
        return rejected<Pointer>(AdapterError::PlanInvalid, "native_operation_copy.request");
    const auto source_parent_identity = request.source_parent->identity();
    const auto destination_identity = request.destination->identity();
    if (!source_parent_identity.value() || !destination_identity.value() ||
        !same(*source_parent_identity.value(), request.source_parent_identity) ||
        !same(*destination_identity.value(), request.destination_identity))
        return rejected<Pointer>(AdapterError::PlanDecisionStale, "native_operation_copy.parent_identity",
                                ObservationState::Changed);

    auto state = std::make_shared<State>(std::move(request));
    const auto append = [state](OperationJournalRecord record) {
        const auto source_parent = NativeDurableIdentity::from_live(state->request.source_parent_identity);
        const auto source = NativeDurableIdentity::from_live(state->request.source_identity);
        const auto destination = NativeDurableIdentity::from_live(state->request.destination_identity);
        if (!source_parent || !source || !destination)
            return Observation<bool>::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity),
                 "native_operation_copy.journal_identity"});
        NativeOperationRecoveryEvidence recovery_evidence{
            state->request.staging_name, state->request.destination_name,
            *source_parent, *source, *destination, {}};
        if (state->staging) {
            auto metadata = state->staging->inspect_basic();
            if (!metadata.value() || !metadata.value()->identity.value() ||
                !metadata.value()->kind.value() || *metadata.value()->kind.value() != ObjectKind::RegularFile)
                return Observation<bool>::unavailable(
                    metadata.value() ? ObservationState::Changed : metadata.state(),
                    metadata.value() ? NativeError{ErrorDomain::Adapter,
                        static_cast<std::int64_t>(AdapterError::MetadataChanged),
                        "native_operation_copy.journal_staging"} : metadata.unavailable_detail()->error);
            recovery_evidence.staging_identity = NativeDurableIdentity::from_live(
                *metadata.value()->identity.value());
            if (!recovery_evidence.staging_identity)
                return Observation<bool>::unavailable(ObservationState::Unknown,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity),
                     "native_operation_copy.journal_staging_identity"});
        }
        auto evidence = encode_native_operation_recovery_evidence(recovery_evidence);
        if (!evidence.value())
            return Observation<bool>::unavailable(evidence.state(), evidence.unavailable_detail()->error);
        record.evidence = std::move(evidence).take_value();
        return append_operation_journal_file(*state->request.journal_file, *state->request.journal,
                                             std::move(record));
    };

    OperationCopyRequest copy;
    copy.item = state->request.item;
    copy.journal = state->request.journal;
    copy.revalidate_source = [state](std::stop_token) {
        const auto access = state->request.operation == FileOperation::Move
            ? EntryAccess::ContentRead | EntryAccess::MoveSource : EntryAccess::ContentRead;
        auto opened = state->request.source_parent->open_child(state->request.source_name, access);
        if (!opened.value()) return unavailable_bool(*opened.unavailable_detail());
        auto metadata = (*opened.value())->inspect_basic();
        if (!metadata.value()) return unavailable_bool(*metadata.unavailable_detail());
        if (!metadata.value()->identity.value() || !metadata.value()->kind.value() ||
            !same(*metadata.value()->identity.value(), state->request.source_identity) ||
            *metadata.value()->kind.value() != ObjectKind::RegularFile)
            return Observation<bool>::available(false);
        state->source = std::move(opened).take_value();
        state->source_exclusion = state->request.operation == FileOperation::Move;
        return Observation<bool>::available(true);
    };
    copy.revalidate_destination = [state](std::stop_token) {
        const auto identity = state->request.destination->identity();
        if (!identity.value()) return unavailable_bool(*identity.unavailable_detail());
        return Observation<bool>::available(same(*identity.value(), state->request.destination_identity));
    };
    copy.prepare_copy = [state](std::stop_token) {
        if (!state->source)
            return rejected<StreamCopyJob::Pointer>(AdapterError::InvalidNativeResult,
                                                    "native_operation_copy.source_handle");
        auto staging = state->request.destination->create_child_file(state->request.staging_name);
        if (!staging.value()) return Observation<StreamCopyJob::Pointer>::unavailable(
            staging.state(), staging.unavailable_detail()->error);
        state->staging = std::move(staging).take_value();
        return create_stream_copy_job(*state->source, *state->staging, state->request.copy_limits);
    };
    copy.prepare_metadata = [state](std::stop_token stop) {
        if (!state->staging)
            return rejected<OperationMetadataJob::Pointer>(AdapterError::InvalidNativeResult,
                                                           "native_operation_copy.staging_handle");
        return state->request.prepare_metadata(*state->staging, stop);
    };
    copy.verify_copy = [state](std::stop_token) {
        if (!state->source || !state->staging)
            return rejected<bool>(AdapterError::InvalidNativeResult,
                                  "native_operation_copy.verify_handles");
        const auto source = state->source->inspect_basic();
        if (!source.value()) return unavailable_bool(*source.unavailable_detail());
        const auto destination = state->staging->inspect_basic();
        if (!destination.value()) return unavailable_bool(*destination.unavailable_detail());
        if (!source.value()->identity.value() || !source.value()->kind.value() ||
            !source.value()->logical_size.value() || !destination.value()->identity.value() ||
            !destination.value()->kind.value() || !destination.value()->logical_size.value())
            return rejected<bool>(AdapterError::UnsupportedQuery, "native_operation_copy.verify_metadata",
                                  ObservationState::Unknown);
        return Observation<bool>::available(
            same(*source.value()->identity.value(), state->request.source_identity) &&
            *source.value()->kind.value() == ObjectKind::RegularFile &&
            *destination.value()->kind.value() == ObjectKind::RegularFile &&
            *source.value()->logical_size.value() == *destination.value()->logical_size.value());
    };
    copy.append_journal = append;

    auto copy_job = OperationCopyJob::create(std::move(copy));
    if (!copy_job.value())
        return Observation<Pointer>::unavailable(copy_job.unavailable_detail()->state,
                                                copy_job.unavailable_detail()->error);

    OperationExecutionRequest execution_request;
    execution_request.copy = std::move(copy_job).take_value();
    execution_request.cleanup_unpublished_staging = [state] {
        return state->cleanup_unpublished_staging();
    };
    execution_request.prepare_commit = [state, append](std::stop_token) {
        OperationCommitRequest commit;
        commit.operation = state->request.operation;
        commit.item = state->request.item;
        commit.journal = state->request.journal;
        commit.revalidate_source = [state](std::stop_token) {
            if (!state->source) return rejected<bool>(AdapterError::InvalidNativeResult,
                                                       "native_operation_copy.commit_source_handle");
            const auto access = state->request.operation == FileOperation::Move
                ? EntryAccess::MetadataOnly | EntryAccess::MoveSource : EntryAccess::MetadataOnly;
            auto current = state->request.source_parent->open_child(state->request.source_name, access);
            if (!current.value()) return unavailable_bool(*current.unavailable_detail());
            const auto current_metadata = (*current.value())->inspect_basic();
            if (!current_metadata.value()) return unavailable_bool(*current_metadata.unavailable_detail());
            if (!current_metadata.value()->identity.value() || !current_metadata.value()->kind.value() ||
                !same(*current_metadata.value()->identity.value(), state->request.source_identity) ||
                *current_metadata.value()->kind.value() != ObjectKind::RegularFile)
                return Observation<bool>::available(false);
            const auto metadata = state->source->inspect_basic();
            if (!metadata.value()) return unavailable_bool(*metadata.unavailable_detail());
            return Observation<bool>::available(metadata.value()->identity.value() &&
                same(*metadata.value()->identity.value(), state->request.source_identity));
        };
        commit.revalidate_destination = [state](std::stop_token) {
            const auto identity = state->request.destination->identity();
            if (!identity.value()) return unavailable_bool(*identity.unavailable_detail());
            return Observation<bool>::available(same(*identity.value(), state->request.destination_identity));
        };
        commit.commit_destination = [state](std::stop_token) {
            if (!state->staging) return rejected<bool>(AdapterError::InvalidNativeResult,
                                                        "native_operation_copy.commit_staging_handle");
            auto published = state->staging->publish_noreplace(*state->request.destination,
                                                                state->request.destination_name);
            if (!published.value()) return unavailable_bool(*published.unavailable_detail());
            return Observation<bool>::available(*published.value());
        };
        if (state->request.operation == FileOperation::Move) {
            commit.verify_source_stability = [state](std::stop_token) {
                if (!state->source_exclusion)
                    return Observation<SourceStabilityGuarantee>::unavailable(ObservationState::Unsupported,
                        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery),
                         "native_operation_copy.source_exclusion"});
                return Observation<SourceStabilityGuarantee>::available(SourceStabilityGuarantee::EnforcedExclusion);
            };
            commit.retire_source = [state](std::stop_token) {
                if (!state->source || !state->request.source_parent)
                    return rejected<bool>(AdapterError::InvalidNativeResult,
                                          "native_operation_copy.retire_source_handle");
                auto retired = state->request.source_parent->retire_child(
                    state->request.source_name, state->request.source_identity);
                if (!retired.value())
                    return unavailable_bool(*retired.unavailable_detail());
                return Observation<bool>::available(*retired.value());
            };
        }
        commit.append_journal = append;
        return OperationCommitJob::create(std::move(commit));
    };

    auto execution = OperationExecutionJob::create(std::move(execution_request));
    if (!execution.value()) return rejected<Pointer>(AdapterError::PlanInvalid,
                                                     "native_operation_copy.execution_job");
    return Observation<Pointer>::available(Pointer(new NativeOperationCopyJob(
        std::move(state), std::move(execution).take_value())));
}

OperationExecutionState NativeOperationCopyJob::advance(std::stop_token stop) {
    return execution_->advance(stop);
}

Observation<bool> NativeOperationCopyJob::cleanup_unpublished_staging() {
    if (!state_ || !execution_)
        return rejected<bool>(AdapterError::InvalidNativeResult, "native_operation_copy.cleanup_state");
    return execution_->cleanup_unpublished_staging();
}

Observation<OperationBatchJob::Pointer> create_native_operation_copy_batch(
    std::vector<NativeOperationCopyJob::Pointer> jobs) {
    OperationBatchRequest request;
    request.jobs.reserve(jobs.size());
    for (auto& job : jobs) {
        if (!job)
            return rejected<OperationBatchJob::Pointer>(AdapterError::PlanInvalid,
                                                        "native_operation_batch.null_job");
        request.jobs.push_back(std::move(*job).release_execution_job());
    }
    return OperationBatchJob::create(std::move(request));
}

Observation<OperationBatchJob::Pointer> create_native_operation_copy_batch(
    NativeOperationCopyBatchRequest request) {
    if (request.sources.empty() || request.sources.size() != request.destination_names.size() ||
        request.sources.size() > request.policy.journal_limits.max_items || !request.destination ||
        !request.open_journal || !request.journal_root ||
        !request.journal_root->is_valid() || !request.policy.copy_limits.valid())
        return rejected<OperationBatchJob::Pointer>(AdapterError::PlanInvalid,
                                                     "native_operation_copy_batch.request");

    const auto destination_identity = request.destination->identity();
    if (!destination_identity.value() || !NativeDurableIdentity::from_live(*destination_identity.value()))
        return rejected<OperationBatchJob::Pointer>(AdapterError::UnqualifiedIdentity,
                                                     "native_operation_copy_batch.destination");

    std::vector<NativeOperationCopyJob::Pointer> jobs;
    jobs.reserve(request.sources.size());
    for (std::size_t item = 0; item != request.sources.size(); ++item) {
        const auto& source = request.sources[item];
        const auto& destination_name = request.destination_names[item];
        if (!source || !destination_name.is_valid() || !source->parent())
            return rejected<OperationBatchJob::Pointer>(AdapterError::PlanInvalid,
                                                         "native_operation_copy_batch.source");
        const auto& metadata = source->metadata();
        if (!metadata.identity.value() || !metadata.kind.value())
            return rejected<OperationBatchJob::Pointer>(AdapterError::UnqualifiedIdentity,
                                                         "native_operation_copy_batch.source_metadata",
                                                         ObservationState::Unknown);
        if (*metadata.kind.value() != ObjectKind::RegularFile)
            return rejected<OperationBatchJob::Pointer>(AdapterError::UnsupportedQuery,
                                                         "native_operation_copy_batch.non_regular_source",
                                                         ObservationState::Unsupported);
        if (!NativeDurableIdentity::from_live(*metadata.identity.value()) ||
            !NativeDurableIdentity::from_live(*source->parent_identity().value()))
            return rejected<OperationBatchJob::Pointer>(AdapterError::UnqualifiedIdentity,
                                                         "native_operation_copy_batch.source_identity");

        const auto item_id = item_journal_id(request.journal_id, item);
        const auto staging_name = generated_name(destination_name.encoding(), ".ck-copy-staging-", item_id, item);
        const auto journal_name = generated_name(request.journal_root->encoding(), ".ck-copy-journal-", item_id, item);
        if (!staging_name || !journal_name)
            return rejected<OperationBatchJob::Pointer>(AdapterError::UnrepresentableName,
                                                         "native_operation_copy_batch.generated_name");

        auto journal_file = request.open_journal(*request.journal_root, *journal_name, true);
        if (!journal_file.value())
            return Observation<OperationBatchJob::Pointer>::unavailable(
                journal_file.unavailable_detail()->state, journal_file.unavailable_detail()->error);
        auto journal = OperationJournal::create(FileOperation::Copy, item_id,
                                                 request.policy.journal_limits);
        if (!journal.value())
            return Observation<OperationBatchJob::Pointer>::unavailable(
                journal.unavailable_detail()->state, journal.unavailable_detail()->error);
        auto journal_pointer = std::move(journal).take_value();
        auto appended = append_operation_journal_file(**journal_file.value(), *journal_pointer,
            {1, 0, OperationJournalState::Planned, {}});
        if (!appended.value())
            return Observation<OperationBatchJob::Pointer>::unavailable(
                appended.unavailable_detail()->state, appended.unavailable_detail()->error);

        auto journal_handle = std::shared_ptr<OperationJournalFile>(std::move(journal_file).take_value());
        NativeOperationCopyRequest copy_request{
            journal_pointer,
            journal_handle,
            0,
            source->parent(),
            *source->parent_identity().value(),
            source->name(),
            *metadata.identity.value(),
            request.destination,
            *destination_identity.value(),
            *staging_name,
            destination_name,
            request.policy.copy_limits,
            {},
            FileOperation::Copy};
        const auto held_source = source;
        const auto allow_metadata_loss = request.policy.allow_unhandled_metadata_loss;
        copy_request.prepare_metadata = [held_source, allow_metadata_loss](OpenedEntry& staging,
                                                                            std::stop_token) {
            OperationMetadataRequest metadata_request;
            metadata_request.apply_traversal_metadata = [&staging, held_source](std::stop_token) {
                return staging.apply_portable_basic_metadata(held_source->metadata());
            };
            metadata_request.apply_security_metadata = [allow_metadata_loss](std::stop_token) {
                if (allow_metadata_loss)
                    return Observation<bool>::available(true);
                return Observation<bool>::unavailable(ObservationState::Unsupported,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::ProviderReviewRequired),
                     "native_operation_copy.metadata_loss_consent"});
            };
            // Portable basic metadata is the only preservation subset this
            // factory binds today. The explicit no-op records that final-time
            // handling beyond that subset was not claimed as preserved.
            metadata_request.apply_final_times = [](std::stop_token) {
                return Observation<bool>::available(true);
            };
            metadata_request.flush_destination = [&staging](std::stop_token) {
                return staging.flush_content();
            };
            return OperationMetadataJob::create(std::move(metadata_request));
        };
        auto created = NativeOperationCopyJob::create(std::move(copy_request));
        if (!created.value())
            return Observation<OperationBatchJob::Pointer>::unavailable(
                created.unavailable_detail()->state, created.unavailable_detail()->error);
        jobs.push_back(std::move(created).take_value());
    }
    return create_native_operation_copy_batch(std::move(jobs));
}

struct NativeOperationCopyTreeJob::State final {
    explicit State(NativeOperationCopyTreeRequest value,
                   OperationJournal::Pointer root_journal,
                   std::shared_ptr<OperationJournalFile> root_journal_file,
                   NativeName root_staging_name,
                   ObjectIdentity root_destination_identity)
        : request(std::move(value)), journal(std::move(root_journal)),
          journal_file(std::move(root_journal_file)), staging_name(std::move(root_staging_name)),
          destination_identity(std::move(root_destination_identity)) {}

    NativeOperationCopyTreeRequest request;
    OperationJournal::Pointer journal;
    std::shared_ptr<OperationJournalFile> journal_file;
    NativeName staging_name;
    ObjectIdentity destination_identity;
    std::shared_ptr<const DirectoryCursor> staging_root;
    std::optional<ObjectIdentity> staging_identity;
    std::vector<std::shared_ptr<const DirectoryCursor>> directories;
    OperationBatchJob::Pointer child_batch;
    std::size_t next_entry = 1;
    std::uint64_t observed_bytes = 0;

    const FrozenInventory& inventory() const noexcept { return *request.inventory->inventory(); }
    const std::vector<HeldChild::Pointer>& sources() const noexcept {
        return request.inventory->sources();
    }

    std::optional<std::size_t> parent_index(std::size_t entry) const noexcept {
        const auto& components = inventory().entries()[entry].relative_components;
        if (components.empty()) return std::nullopt;
        const auto prefix_size = components.size() - 1;
        for (std::size_t candidate = 0; candidate != entry; ++candidate) {
            const auto& candidate_components = inventory().entries()[candidate].relative_components;
            if (inventory().entries()[candidate].kind != ObjectKind::Directory ||
                candidate_components.size() != prefix_size)
                continue;
            if (std::equal(candidate_components.begin(), candidate_components.end(), components.begin()))
                return candidate;
        }
        return std::nullopt;
    }

    Observation<bool> append(OperationJournalState state) {
        NativeOperationRecoveryEvidence evidence{
            staging_name,
            request.destination_name,
            {},
            {},
            {},
            {}};
        const auto& root_source = sources().front();
        const auto& root_metadata = inventory().entries().front();
        if (!root_source || !root_source->parent_identity().value() ||
            !root_metadata.identity.scope.is_valid() || !destination_identity.scope.is_valid())
            return rejected<bool>(AdapterError::UnqualifiedIdentity,
                                  "native_operation_copy_tree.journal_identity");
        const auto source_parent = NativeDurableIdentity::from_live(*root_source->parent_identity().value());
        const auto source = NativeDurableIdentity::from_live(root_metadata.identity);
        const auto destination = NativeDurableIdentity::from_live(destination_identity);
        if (!source_parent || !source || !destination)
            return rejected<bool>(AdapterError::UnqualifiedIdentity,
                                  "native_operation_copy_tree.journal_identity");
        evidence.source_parent_identity = *source_parent;
        evidence.source_identity = *source;
        evidence.destination_identity = *destination;
        if (staging_identity) {
            const auto durable_staging = NativeDurableIdentity::from_live(*staging_identity);
            if (!durable_staging)
                return rejected<bool>(AdapterError::UnqualifiedIdentity,
                                      "native_operation_copy_tree.journal_staging_identity",
                                      ObservationState::Unknown);
            evidence.staging_identity = *durable_staging;
        }
        auto encoded = encode_native_operation_recovery_evidence(evidence);
        if (!encoded.value())
            return Observation<bool>::unavailable(encoded.unavailable_detail()->state,
                                                  encoded.unavailable_detail()->error);
        const auto sequence = static_cast<std::uint64_t>(journal->records().size()) + 1;
        return append_operation_journal_file(*journal_file, *journal,
            {sequence, 0, state, std::move(encoded).take_value()});
    }

    Observation<bool> verify() const {
        const auto destination = request.destination->identity();
        if (!destination.value()) return unavailable_bool(*destination.unavailable_detail());
        if (!same(*destination.value(), destination_identity)) return Observation<bool>::available(false);
        if (!staging_root || !staging_identity) return rejected<bool>(
            AdapterError::InvalidNativeResult, "native_operation_copy_tree.verify_staging");
        const auto staged = staging_root->inspect_basic();
        if (!staged.value()) return unavailable_bool(*staged.unavailable_detail());
        if (!staged.value()->identity.value() || !staged.value()->kind.value() ||
            *staged.value()->kind.value() != ObjectKind::Directory ||
            !same(*staged.value()->identity.value(), *staging_identity))
            return Observation<bool>::available(false);
        const auto& entries = inventory().entries();
        for (std::size_t index = 0; index != entries.size(); ++index) {
            const auto current = sources()[index]->inspect_basic();
            if (!current.value()) return unavailable_bool(*current.unavailable_detail());
            if (!current.value()->identity.value() || !current.value()->kind.value() ||
                !same(*current.value()->identity.value(), entries[index].identity) ||
                *current.value()->kind.value() != entries[index].kind)
                return Observation<bool>::available(false);
            if (entries[index].kind == ObjectKind::RegularFile &&
                (!current.value()->logical_size.value() ||
                 *current.value()->logical_size.value() != entries[index].logical_size))
                return Observation<bool>::available(false);
        }
        for (std::size_t index = 0; index != directories.size(); ++index) {
            if (!directories[index]) continue;
            const auto current = directories[index]->inspect_basic();
            if (!current.value()) return unavailable_bool(*current.unavailable_detail());
            if (!current.value()->identity.value() || !current.value()->kind.value() ||
                *current.value()->kind.value() != ObjectKind::Directory)
                return Observation<bool>::available(false);
        }
        return Observation<bool>::available(true);
    }
};

namespace {

bool tree_terminal(OperationBatchState state) noexcept {
    return state == OperationBatchState::Complete || state == OperationBatchState::Failed ||
        state == OperationBatchState::Cancelled || state == OperationBatchState::RetainedSource ||
        state == OperationBatchState::NeedsReconciliation;
}

UnavailableObservation tree_error(AdapterError code, const char* operation,
                                  ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

} // namespace

Observation<NativeOperationCopyTreeJob::Pointer> NativeOperationCopyTreeJob::create(
    NativeOperationCopyTreeRequest request) {
    if (!request.inventory || !request.inventory->inventory() || !request.destination ||
        !request.destination_name.is_valid() || !request.journal_root ||
        !request.journal_root->is_valid() || !request.open_journal ||
        !request.policy.copy_limits.valid())
        return rejected<Pointer>(AdapterError::PlanInvalid, "native_operation_copy_tree.request");

    const auto& inventory = *request.inventory->inventory();
    const auto& entries = inventory.entries();
    const auto& sources = request.inventory->sources();
    if (entries.empty() || entries.size() != sources.size() ||
        entries.size() > request.policy.journal_limits.max_items ||
        entries.front().kind != ObjectKind::Directory || !entries.front().relative_components.empty() ||
        inventory.link_policy() != InventoryLinkPolicy::PreserveLinks)
        return rejected<Pointer>(AdapterError::PlanInvalid, "native_operation_copy_tree.inventory");
    const auto destination_identity = request.destination->identity();
    if (!destination_identity.value() || !NativeDurableIdentity::from_live(*destination_identity.value()))
        return rejected<Pointer>(AdapterError::UnqualifiedIdentity,
                                "native_operation_copy_tree.destination");

    std::unordered_set<std::size_t> hard_link_groups;
    for (std::size_t index = 0; index != entries.size(); ++index) {
        const auto& entry = entries[index];
        const auto& source = sources[index];
        if (!source || entry.identity.scheme == IdentityScheme::Unknown ||
            !NativeDurableIdentity::from_live(entry.identity) || !source->metadata().identity.value() ||
            !source->metadata().kind.value() ||
            !same(*source->metadata().identity.value(), entry.identity) ||
            *source->metadata().kind.value() != entry.kind || entry.link != LinkTraversalEvidence::NotLink)
            return rejected<Pointer>(AdapterError::PlanDecisionStale,
                                     "native_operation_copy_tree.source_identity", ObservationState::Changed);
        if (index != 0 && (entry.relative_components.empty() ||
                           entry.relative_components.back().encoding() != request.destination_name.encoding()))
            return rejected<Pointer>(AdapterError::UnsupportedQuery,
                                     "native_operation_copy_tree.name_encoding",
                                     ObservationState::Unsupported);
        if (entry.kind != ObjectKind::Directory && entry.kind != ObjectKind::RegularFile)
            return rejected<Pointer>(AdapterError::UnsupportedQuery,
                                     "native_operation_copy_tree.entry_kind",
                                     ObservationState::Unsupported);
        if (entry.kind == ObjectKind::RegularFile && index != 0) {
            const auto group = inventory.hard_link_groups()[index];
            if (!hard_link_groups.insert(group).second)
                return rejected<Pointer>(AdapterError::UnsupportedQuery,
                                         "native_operation_copy_tree.hard_link_alias",
                                         ObservationState::Unsupported);
        }
        if (!entry.relative_components.empty() && entry.relative_components.back().encoding() !=
                                                   source->name().encoding())
            return rejected<Pointer>(AdapterError::PlanInvalid,
                                     "native_operation_copy_tree.source_name_encoding");
    }
    if (!sources.front()->parent_identity().value() ||
        !NativeDurableIdentity::from_live(*sources.front()->parent_identity().value()))
        return rejected<Pointer>(AdapterError::UnqualifiedIdentity,
                                 "native_operation_copy_tree.source_parent");
    if (!request.policy.allow_unhandled_metadata_loss)
        return rejected<Pointer>(AdapterError::ProviderReviewRequired,
                                 "native_operation_copy_tree.metadata_loss_consent",
                                 ObservationState::Unsupported);

    const auto root_staging_name = generated_name(request.destination_name.encoding(), ".ck-tree-staging-",
                                                   request.journal_id, 0);
    const auto journal_name = generated_name(request.journal_root->encoding(), ".ck-tree-journal-",
                                             request.journal_id, 0);
    if (!root_staging_name || !journal_name)
        return rejected<Pointer>(AdapterError::UnrepresentableName,
                                 "native_operation_copy_tree.generated_name");
    auto journal_file = request.open_journal(*request.journal_root, *journal_name, true);
    if (!journal_file.value())
        return Observation<Pointer>::unavailable(journal_file.unavailable_detail()->state,
                                                 journal_file.unavailable_detail()->error);
    auto journal = OperationJournal::create(FileOperation::Copy, request.journal_id,
                                            request.policy.journal_limits);
    if (!journal.value())
        return Observation<Pointer>::unavailable(journal.unavailable_detail()->state,
                                                 journal.unavailable_detail()->error);
    auto journal_pointer = std::move(journal).take_value();
    auto journal_handle = std::shared_ptr<OperationJournalFile>(std::move(journal_file).take_value());
    auto planned = append_operation_journal_file(*journal_handle, *journal_pointer,
        {1, 0, OperationJournalState::Planned, {}});
    if (!planned.value())
        return Observation<Pointer>::unavailable(planned.unavailable_detail()->state,
                                                 planned.unavailable_detail()->error);
    auto state = std::make_shared<State>(std::move(request), std::move(journal_pointer),
                                         std::move(journal_handle), std::move(*root_staging_name),
                                         *destination_identity.value());
    return Observation<Pointer>::available(Pointer(new NativeOperationCopyTreeJob(std::move(state))));
}

OperationProgress NativeOperationCopyTreeJob::progress() const noexcept {
    const auto total = state_data_ && state_data_->request.inventory && state_data_->request.inventory->inventory()
        ? std::optional<std::uint64_t>(state_data_->request.inventory->inventory()->logical_bytes())
        : std::nullopt;
    return {state_, state_data_ ? state_data_->observed_bytes : 0, total};
}

OperationExecutionState NativeOperationCopyTreeJob::advance(std::stop_token stop) {
    if (state_ == OperationExecutionState::Complete || state_ == OperationExecutionState::Failed ||
        state_ == OperationExecutionState::Cancelled || state_ == OperationExecutionState::NeedsReconciliation)
        return state_;

    auto fail = [&](UnavailableObservation detail, bool force_reconciliation = false) {
        const auto desired = force_reconciliation || detail.state == ObservationState::Unknown
            ? OperationJournalState::NeedsReconciliation : OperationJournalState::Failed;
        const auto appended = state_data_->append(desired);
        if (!appended.value()) {
            result_.failure = *appended.unavailable_detail();
            state_ = result_.state = OperationExecutionState::NeedsReconciliation;
            return;
        }
        result_.failure = std::move(detail);
        state_ = result_.state = desired == OperationJournalState::NeedsReconciliation
            ? OperationExecutionState::NeedsReconciliation : OperationExecutionState::Failed;
    };
    auto cancel = [&] {
        const auto appended = state_data_->append(OperationJournalState::Cancelled);
        if (!appended.value()) {
            result_.failure = *appended.unavailable_detail();
            state_ = result_.state = OperationExecutionState::NeedsReconciliation;
        } else {
            state_ = result_.state = OperationExecutionState::Cancelled;
        }
    };
    auto journal_failure = [&](UnavailableObservation detail) {
        result_.failure = std::move(detail);
        state_ = result_.state = OperationExecutionState::NeedsReconciliation;
    };

    // The phase is encoded by the durable journal state and the retained
    // native owners: Planned means root creation, Prepared means entries,
    // Copied means verification, and Verified means publish preparation.
    const auto journal_state = state_data_->journal->state(0);
    if (!journal_state) {
        fail(tree_error(AdapterError::InvalidNativeResult, "native_operation_copy_tree.journal_state"), true);
        return state_;
    }
    if (stop.stop_requested()) {
        if (*journal_state == OperationJournalState::Complete ||
            *journal_state == OperationJournalState::DestinationCommitted) return state_;
        cancel();
        return state_;
    }

    if (*journal_state == OperationJournalState::Planned) {
        auto created = state_data_->request.destination->create_child_directory(state_data_->staging_name);
        if (!created.value()) {
            fail(*created.unavailable_detail());
            return state_;
        }
        state_data_->staging_root = std::shared_ptr<const DirectoryCursor>(std::move(created).take_value());
        const auto metadata = state_data_->staging_root->inspect_basic();
        if (!metadata.value() || !metadata.value()->identity.value() || !metadata.value()->kind.value() ||
            *metadata.value()->kind.value() != ObjectKind::Directory) {
            fail(metadata.value() ? tree_error(AdapterError::InvalidNativeResult,
                                               "native_operation_copy_tree.staging_kind", ObservationState::Unknown)
                                  : *metadata.unavailable_detail(), true);
            return state_;
        }
        state_data_->staging_identity = *metadata.value()->identity.value();
        state_data_->directories.resize(state_data_->inventory().entries().size());
        state_data_->directories[0] = state_data_->staging_root;
        const auto appended = state_data_->append(OperationJournalState::Prepared);
        if (!appended.value()) {
            journal_failure(*appended.unavailable_detail());
            return state_;
        }
        return state_;
    }

    if (*journal_state == OperationJournalState::Prepared) {
        if (state_data_->child_batch) {
            const auto child_state = state_data_->child_batch->advance(stop);
            if (!tree_terminal(child_state))
                return state_;
            const auto& items = state_data_->child_batch->result().items;
            if (child_state != OperationBatchState::Complete) {
                if (!items.empty() && items.front().failure)
                    fail(*items.front().failure, child_state == OperationBatchState::NeedsReconciliation);
                else
                    fail(tree_error(AdapterError::InvalidNativeResult,
                                    "native_operation_copy_tree.child_failure"),
                         child_state == OperationBatchState::NeedsReconciliation);
                return state_;
            }
            state_data_->child_batch.reset();
            ++state_data_->next_entry;
            return state_;
        }
        const auto& entries = state_data_->inventory().entries();
        if (state_data_->next_entry >= entries.size()) {
            const auto appended = state_data_->append(OperationJournalState::Copied);
            if (!appended.value()) {
                journal_failure(*appended.unavailable_detail());
                return state_;
            }
            return state_;
        }
        const auto index = state_data_->next_entry;
        const auto parent = state_data_->parent_index(index);
        if (!parent || *parent >= state_data_->directories.size() || !state_data_->directories[*parent]) {
            fail(tree_error(AdapterError::PlanTopologyUnsafe,
                            "native_operation_copy_tree.parent_mapping"));
            return state_;
        }
        const auto& component = entries[index].relative_components.back();
        if (entries[index].kind == ObjectKind::Directory) {
            auto created = state_data_->directories[*parent]->create_child_directory(component);
            if (!created.value()) {
                fail(*created.unavailable_detail());
                return state_;
            }
            state_data_->directories[index] = std::shared_ptr<const DirectoryCursor>(std::move(created).take_value());
            const auto metadata = state_data_->directories[index]->inspect_basic();
            if (!metadata.value() || !metadata.value()->identity.value() || !metadata.value()->kind.value() ||
                *metadata.value()->kind.value() != ObjectKind::Directory) {
                fail(metadata.value() ? tree_error(AdapterError::InvalidNativeResult,
                                                   "native_operation_copy_tree.directory_kind", ObservationState::Unknown)
                                      : *metadata.unavailable_detail(), true);
                return state_;
            }
            ++state_data_->next_entry;
            return state_;
        }
        NativeOperationCopyBatchRequest child_request;
        child_request.sources = {state_data_->sources()[index]};
        child_request.destination = state_data_->directories[*parent];
        child_request.destination_names = {component};
        child_request.journal_root = state_data_->request.journal_root;
        child_request.journal_id = tree_child_journal_id(state_data_->request.journal_id, index);
        child_request.policy = state_data_->request.policy;
        child_request.open_journal = state_data_->request.open_journal;
        auto child = create_native_operation_copy_batch(std::move(child_request));
        if (!child.value()) {
            fail(*child.unavailable_detail(), true);
            return state_;
        }
        state_data_->child_batch = std::move(child).take_value();
        return state_;
    }

    if (*journal_state == OperationJournalState::Copied) {
        const auto verified = state_data_->verify();
        if (!verified.value()) {
            fail(*verified.unavailable_detail(), verified.state() == ObservationState::Unknown);
            return state_;
        }
        if (!*verified.value()) {
            fail(tree_error(AdapterError::PlanDecisionStale,
                            "native_operation_copy_tree.verify", ObservationState::Changed));
            return state_;
        }
        const auto appended = state_data_->append(OperationJournalState::Verified);
        if (!appended.value()) {
            journal_failure(*appended.unavailable_detail());
            return state_;
        }
        return state_;
    }

    if (*journal_state == OperationJournalState::Verified) {
        const auto appended = state_data_->append(OperationJournalState::CommitIntended);
        if (!appended.value()) {
            journal_failure(*appended.unavailable_detail());
            return state_;
        }
        return state_;
    }

    if (*journal_state == OperationJournalState::CommitIntended) {
        if (!state_data_->staging_root || !state_data_->staging_identity) {
            fail(tree_error(AdapterError::InvalidNativeResult,
                            "native_operation_copy_tree.publish_handle"), true);
            return state_;
        }
        // Windows refuses to rename a directory tree while descendant
        // directory handles remain open. Verification has completed, and
        // rename_child_noreplace reopens the root by its qualified name and
        // identity, so release traversal handles before publication.
        state_data_->directories.clear();
        state_data_->staging_root.reset();
        auto published = state_data_->request.destination->rename_child_noreplace(
            state_data_->staging_name, *state_data_->staging_identity,
            *state_data_->request.destination, state_data_->request.destination_name);
        if (!published.value()) {
            fail(*published.unavailable_detail(), published.state() == ObservationState::Unknown);
            return state_;
        }
        if (!*published.value()) {
            fail(tree_error(AdapterError::InvalidNativeResult,
                            "native_operation_copy_tree.publish_result"), true);
            return state_;
        }
        const auto appended = state_data_->append(OperationJournalState::DestinationCommitted);
        if (!appended.value()) {
            journal_failure(*appended.unavailable_detail());
            return state_;
        }
        const auto complete = state_data_->append(OperationJournalState::Complete);
        if (!complete.value()) {
            journal_failure(*complete.unavailable_detail());
            return state_;
        }
        state_ = result_.state = OperationExecutionState::Complete;
        return state_;
    }

    fail(tree_error(AdapterError::InvalidNativeResult, "native_operation_copy_tree.state"), true);
    return state_;
}

Observation<bool> NativeOperationCopyTreeJob::cleanup_unpublished_staging() {
    return rejected<bool>(AdapterError::UnsupportedQuery,
                          "native_operation_copy_tree.cleanup_recursive", ObservationState::Unsupported);
}

Observation<NativeOperationCopyTreeJob::Pointer> create_native_operation_copy_tree(
    NativeOperationCopyTreeRequest request) {
    return NativeOperationCopyTreeJob::create(std::move(request));
}

} // namespace ck::files
