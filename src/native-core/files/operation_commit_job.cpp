// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_commit_job.hpp"

#include <cstdint>

namespace ck::files {
namespace {
template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

UnavailableObservation detail(AdapterError code, const char* operation,
                              ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
}

Observation<OperationCommitJob::Pointer> OperationCommitJob::create(OperationCommitRequest request) {
    if (!request.journal || request.item >= request.journal->limits().max_items ||
        request.journal->operation() != request.operation ||
        !request.revalidate_source || !request.revalidate_destination || !request.commit_destination ||
        !request.append_journal ||
        (request.operation == FileOperation::Move &&
         (!request.retire_source || !request.verify_source_stability)))
        return rejected<OperationCommitJob::Pointer>(AdapterError::PlanInvalid, "operation_commit.request");
    if (request.journal->state(request.item) != OperationJournalState::Verified)
        return rejected<OperationCommitJob::Pointer>(AdapterError::PlanInvalid, "operation_commit.journal_state");
    return Observation<OperationCommitJob::Pointer>::available(
        OperationCommitJob::Pointer(new OperationCommitJob(std::move(request))));
}

void OperationCommitJob::fail(UnavailableObservation failure, bool reconciliation) noexcept {
    result_.failure = std::move(failure);
    result_.state = reconciliation ? OperationCommitState::NeedsReconciliation : OperationCommitState::Failed;
}

void OperationCommitJob::fail(AdapterError code, const char* operation, bool reconciliation) noexcept {
    fail(detail(code, operation), reconciliation);
}

bool OperationCommitJob::append(OperationJournalState state) {
    const auto before = request_.journal->records().size();
    const auto sequence = static_cast<std::uint64_t>(before) + 1;
    auto result = request_.append_journal({sequence, request_.item, state, {}});
    if (!result.value()) {
        const auto& error = *result.unavailable_detail();
        fail(error, true);
        return false;
    }
    if (request_.journal->records().size() != before + 1 ||
        request_.journal->records().back().sequence != sequence ||
        request_.journal->state(request_.item) != state) {
        fail(AdapterError::InvalidNativeResult, "operation_commit.journal_publish", true);
        return false;
    }
    return true;
}

bool OperationCommitJob::invoke_revalidation(
    const std::function<Observation<bool>(std::stop_token)>& callback,
    std::stop_token stop, const char* operation, OperationJournalState terminal_state) {
    const auto finish_without_action = [this, terminal_state](UnavailableObservation error) {
        result_.failure = std::move(error);
        if (append(terminal_state)) {
            result_.state = terminal_state == OperationJournalState::Failed
                ? OperationCommitState::Failed : OperationCommitState::RetainedSource;
        }
        return false;
    };
    if (stop.stop_requested()) {
        return finish_without_action(detail(AdapterError::InterruptedRead, "operation_commit.cancelled"));
    }
    try {
        auto result = callback(stop);
        if (!result.value() || !*result.value()) {
            if (!result.value()) {
                const auto& error = *result.unavailable_detail();
                return finish_without_action(error);
            } else {
                return finish_without_action(detail(AdapterError::PlanDecisionStale, operation));
            }
        }
    } catch (...) {
        return finish_without_action(detail(AdapterError::InvalidNativeResult, operation));
    }
    return true;
}

bool OperationCommitJob::invoke_source_stability(std::stop_token stop) {
    const auto retain = [this](UnavailableObservation error) {
        result_.failure = std::move(error);
        if (append(OperationJournalState::RetainedSource))
            result_.state = OperationCommitState::RetainedSource;
        return false;
    };
    if (stop.stop_requested())
        return retain(detail(AdapterError::InterruptedRead, "operation_commit.source_stability_cancelled"));
    try {
        const auto result = request_.verify_source_stability(stop);
        if (!result.value()) {
            const auto& error = *result.unavailable_detail();
            return retain(error);
        }
        if (*result.value() == SourceStabilityGuarantee::BeforeAfterObservation)
            return retain(detail(AdapterError::PlanDecisionStale,
                                 "operation_commit.source_stability_observation_only"));
    } catch (...) {
        return retain(detail(AdapterError::InvalidNativeResult,
                             "operation_commit.source_stability_exception"));
    }
    return true;
}

OperationCommitState OperationCommitJob::advance(std::stop_token stop) {
    if (result_.state == OperationCommitState::Complete || result_.state == OperationCommitState::Failed ||
        result_.state == OperationCommitState::Cancelled || result_.state == OperationCommitState::RetainedSource ||
        result_.state == OperationCommitState::NeedsReconciliation)
        return result_.state;

    if (result_.state == OperationCommitState::Ready) {
        if (stop.stop_requested()) {
            if (append(OperationJournalState::Cancelled)) result_.state = OperationCommitState::Cancelled;
            return result_.state;
        }
        if (!invoke_revalidation(request_.revalidate_source, stop, "operation_commit.source_changed",
                                  OperationJournalState::Failed) ||
            !invoke_revalidation(request_.revalidate_destination, stop, "operation_commit.destination_changed",
                                  OperationJournalState::Failed))
            return result_.state;
        if (append(OperationJournalState::CommitIntended)) result_.state = OperationCommitState::CommittingDestination;
        return result_.state;
    }

    if (result_.state == OperationCommitState::CommittingDestination) {
        if (stop.stop_requested()) {
            if (append(OperationJournalState::Cancelled)) result_.state = OperationCommitState::Cancelled;
            return result_.state;
        }
        try {
            auto committed = request_.commit_destination(stop);
            if (!committed.value()) {
                const auto& error = *committed.unavailable_detail();
                fail(error, true);
                return result_.state;
            }
            if (!*committed.value()) {
                fail(AdapterError::InvalidNativeResult, "operation_commit.publish_result", true);
                return result_.state;
            }
        } catch (...) {
            fail(AdapterError::InvalidNativeResult, "operation_commit.publish_exception", true);
            return result_.state;
        }
        if (!append(OperationJournalState::DestinationCommitted)) return result_.state;
        if (request_.operation == FileOperation::Copy) {
            if (append(OperationJournalState::Complete)) result_.state = OperationCommitState::Complete;
        } else {
            if (append(OperationJournalState::SourceRemovalIntended)) result_.state = OperationCommitState::RetiringSource;
        }
        return result_.state;
    }

    if (stop.stop_requested()) {
        if (append(OperationJournalState::RetainedSource)) result_.state = OperationCommitState::RetainedSource;
        return result_.state;
    }
    if (!invoke_revalidation(request_.revalidate_source, stop, "operation_commit.source_changed_before_retire",
                             OperationJournalState::RetainedSource))
        return result_.state;
    // A held identity and a pair of observations do not prove that a writer
    // could not have changed content. Require the adapter's explicit snapshot
    // or enforced-exclusion guarantee before entering source retirement.
    if (!invoke_source_stability(stop))
        return result_.state;
    try {
        auto retired = request_.retire_source(stop);
        if (!retired.value()) {
            const auto& error = *retired.unavailable_detail();
            fail(error, true);
            return result_.state;
        }
        if (!*retired.value()) {
            fail(AdapterError::InvalidNativeResult, "operation_commit.retire_result", true);
            return result_.state;
        }
    } catch (...) {
        fail(AdapterError::InvalidNativeResult, "operation_commit.retire_exception", true);
        return result_.state;
    }
    if (append(OperationJournalState::SourceRetired) && append(OperationJournalState::Complete))
        result_.state = OperationCommitState::Complete;
    return result_.state;
}

} // namespace ck::files
