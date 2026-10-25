// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_execution_job.hpp"

#include <cstdint>

namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char* operation,
                              ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

template <class T>
Observation<T> rejected(AdapterError code, const char* operation) {
    return Observation<T>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}

Observation<OperationExecutionJob::Pointer> OperationExecutionJob::create(OperationExecutionRequest request) {
    if (!request.copy || !request.prepare_commit)
        return rejected<Pointer>(AdapterError::PlanInvalid, "operation_execution.request");
    return Observation<Pointer>::available(Pointer(new OperationExecutionJob(std::move(request))));
}

void OperationExecutionJob::fail(UnavailableObservation detail, OperationExecutionState state) noexcept {
    result_.failure = std::move(detail);
    result_.state = state;
}

OperationExecutionState OperationExecutionJob::advance(std::stop_token stop) {
    if (result_.state == OperationExecutionState::Complete || result_.state == OperationExecutionState::Failed ||
        result_.state == OperationExecutionState::Cancelled || result_.state == OperationExecutionState::RetainedSource ||
        result_.state == OperationExecutionState::NeedsReconciliation)
        return result_.state;

    if (result_.state == OperationExecutionState::Copying) {
        const auto copy_state = request_.copy->advance(stop);
        switch (copy_state) {
        case OperationCopyState::Verified:
            result_.state = OperationExecutionState::PreparingCommit;
            return result_.state;
        case OperationCopyState::Failed:
            if (request_.copy->failure()) result_.failure = request_.copy->failure();
            result_.state = OperationExecutionState::Failed;
            return result_.state;
        case OperationCopyState::Cancelled:
            if (request_.copy->failure()) result_.failure = request_.copy->failure();
            result_.state = OperationExecutionState::Cancelled;
            return result_.state;
        case OperationCopyState::NeedsReconciliation:
            if (request_.copy->failure()) result_.failure = request_.copy->failure();
            result_.state = OperationExecutionState::NeedsReconciliation;
            return result_.state;
        default:
            return result_.state;
        }
    }

    if (result_.state == OperationExecutionState::PreparingCommit) {
        try {
            auto prepared = request_.prepare_commit(stop);
            if (!prepared.value()) {
                fail(*prepared.unavailable_detail(), OperationExecutionState::NeedsReconciliation);
                return result_.state;
            }
            commit_ = std::move(prepared).take_value();
            if (!commit_ || commit_->state() != OperationCommitState::Ready) {
                fail(error(AdapterError::InvalidNativeResult, "operation_execution.commit_prepare"),
                     OperationExecutionState::NeedsReconciliation);
                return result_.state;
            }
        } catch (...) {
            fail(error(AdapterError::InvalidNativeResult, "operation_execution.commit_prepare_exception"),
                 OperationExecutionState::NeedsReconciliation);
            return result_.state;
        }
        result_.state = OperationExecutionState::Committing;
        return result_.state;
    }

    const auto commit_state = commit_->advance(stop);
    switch (commit_state) {
    case OperationCommitState::Complete:
        result_.state = OperationExecutionState::Complete;
        break;
    case OperationCommitState::Failed:
        if (commit_->failure()) result_.failure = commit_->failure();
        result_.state = OperationExecutionState::Failed;
        break;
    case OperationCommitState::Cancelled:
        if (commit_->failure()) result_.failure = commit_->failure();
        result_.state = OperationExecutionState::Cancelled;
        break;
    case OperationCommitState::RetainedSource:
        if (commit_->failure()) result_.failure = commit_->failure();
        result_.state = OperationExecutionState::RetainedSource;
        break;
    case OperationCommitState::NeedsReconciliation:
        if (commit_->failure()) result_.failure = commit_->failure();
        result_.state = OperationExecutionState::NeedsReconciliation;
        break;
    default:
        break;
    }
    return result_.state;
}

} // namespace ck::files
