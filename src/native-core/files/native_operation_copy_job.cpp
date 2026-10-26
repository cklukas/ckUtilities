// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_operation_copy_job.hpp"

#include "directory_cursor.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "opened_entry.hpp"

#include <cstdint>
#include <string>

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

} // namespace

struct NativeOperationCopyJob::State final {
    explicit State(NativeOperationCopyRequest value) : request(std::move(value)) {}

    NativeOperationCopyRequest request;
    std::unique_ptr<OpenedEntry> source;
    std::unique_ptr<OpenedEntry> staging;

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
        request.journal->operation() != FileOperation::Copy ||
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
        auto opened = state->request.source_parent->open_child(state->request.source_name, EntryAccess::ContentRead);
        if (!opened.value()) return unavailable_bool(*opened.unavailable_detail());
        auto metadata = (*opened.value())->inspect_basic();
        if (!metadata.value()) return unavailable_bool(*metadata.unavailable_detail());
        if (!metadata.value()->identity.value() || !metadata.value()->kind.value() ||
            !same(*metadata.value()->identity.value(), state->request.source_identity) ||
            *metadata.value()->kind.value() != ObjectKind::RegularFile)
            return Observation<bool>::available(false);
        state->source = std::move(opened).take_value();
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
        commit.operation = FileOperation::Copy;
        commit.item = state->request.item;
        commit.journal = state->request.journal;
        commit.revalidate_source = [state](std::stop_token) {
            if (!state->source) return rejected<bool>(AdapterError::InvalidNativeResult,
                                                       "native_operation_copy.commit_source_handle");
            auto current = state->request.source_parent->open_child(state->request.source_name,
                                                                     EntryAccess::MetadataOnly);
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
            {}};
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

} // namespace ck::files
