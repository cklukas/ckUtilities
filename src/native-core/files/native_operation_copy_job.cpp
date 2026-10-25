// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_operation_copy_job.hpp"

#include "directory_cursor.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "opened_entry.hpp"

#include <cstdint>

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
        auto evidence = encode_native_operation_recovery_evidence({
            state->request.staging_name, state->request.destination_name,
            *source_parent, *source, *destination});
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

} // namespace ck::files
