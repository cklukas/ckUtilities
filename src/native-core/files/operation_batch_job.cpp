// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_batch_job.hpp"

#include <cstdint>

namespace ck::files {
namespace {
template <class T>
Observation<T> rejected(AdapterError code, const char* operation) {
    return Observation<T>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}

OperationBatchJob::OperationBatchJob(OperationBatchRequest request)
    : request_(std::move(request)), started_(request_.jobs.size(), false) {
    result_.items.reserve(request_.jobs.size());
    for (std::size_t index = 0; index != request_.jobs.size(); ++index)
        result_.items.push_back({index, OperationItemOutcome::Pending, std::nullopt});
}

Observation<OperationBatchJob::Pointer> OperationBatchJob::create(OperationBatchRequest request) {
    if (request.jobs.size() > OperationJournalLimits::hard_max_items)
        return rejected<Pointer>(AdapterError::InventoryLimit, "operation_batch.items");
    try {
        return Observation<Pointer>::available(Pointer(new OperationBatchJob(std::move(request))));
    } catch (...) {
        return rejected<Pointer>(AdapterError::InventoryLimit, "operation_batch.create");
    }
}

OperationItemOutcome OperationBatchJob::item_outcome(OperationExecutionState state) noexcept {
    switch (state) {
    case OperationExecutionState::Complete: return OperationItemOutcome::Complete;
    case OperationExecutionState::Failed: return OperationItemOutcome::Failed;
    case OperationExecutionState::Cancelled: return OperationItemOutcome::Cancelled;
    case OperationExecutionState::RetainedSource: return OperationItemOutcome::RetainedSource;
    case OperationExecutionState::NeedsReconciliation: return OperationItemOutcome::NeedsReconciliation;
    case OperationExecutionState::Copying:
    case OperationExecutionState::PreparingCommit:
    case OperationExecutionState::Committing:
        return OperationItemOutcome::Pending;
    }
    return OperationItemOutcome::Pending;
}

bool OperationBatchJob::terminal(OperationExecutionState state) noexcept {
    return state == OperationExecutionState::Complete || state == OperationExecutionState::Failed ||
        state == OperationExecutionState::Cancelled || state == OperationExecutionState::RetainedSource ||
        state == OperationExecutionState::NeedsReconciliation;
}

void OperationBatchJob::record_current() {
    if (current_ >= request_.jobs.size()) return;
    const auto& job = request_.jobs[current_];
    auto& item = result_.items[current_];
    item.outcome = item_outcome(job->state());
    if (job->failure()) item.failure = job->failure();
}

void OperationBatchJob::finish_if_done() {
    if (current_ < request_.jobs.size()) return;
    bool failed = false;
    bool cancelled = false;
    bool retained = false;
    bool reconciliation = false;
    for (const auto& item : result_.items) {
        failed = failed || item.outcome == OperationItemOutcome::Failed;
        cancelled = cancelled || item.outcome == OperationItemOutcome::Cancelled;
        retained = retained || item.outcome == OperationItemOutcome::RetainedSource;
        reconciliation = reconciliation || item.outcome == OperationItemOutcome::NeedsReconciliation;
    }
    result_.state = reconciliation ? OperationBatchState::NeedsReconciliation :
        retained ? OperationBatchState::RetainedSource :
        failed ? OperationBatchState::Failed :
        cancelled ? OperationBatchState::Cancelled : OperationBatchState::Complete;
}

OperationBatchState OperationBatchJob::advance(std::stop_token stop) {
    if (result_.state == OperationBatchState::Complete || result_.state == OperationBatchState::Failed ||
        result_.state == OperationBatchState::Cancelled || result_.state == OperationBatchState::RetainedSource ||
        result_.state == OperationBatchState::NeedsReconciliation)
        return result_.state;
    if (result_.state == OperationBatchState::Ready)
        result_.state = OperationBatchState::Running;

    while (current_ < request_.jobs.size() && terminal(request_.jobs[current_]->state())) {
        record_current();
        ++current_;
    }
    finish_if_done();
    if (result_.state != OperationBatchState::Running)
        return result_.state;

    if (stop.stop_requested()) {
        // A never-started item remains Pending. If the current item already
        // owns an in-flight job, give that job its safe cancellation point.
        if (started_[current_]) {
            request_.jobs[current_]->advance(stop);
            record_current();
            if (terminal(request_.jobs[current_]->state())) ++current_;
        }
        result_.state = OperationBatchState::Cancelled;
        return result_.state;
    }

    started_[current_] = true;
    request_.jobs[current_]->advance(stop);
    record_current();
    if (terminal(request_.jobs[current_]->state())) ++current_;
    finish_if_done();
    return result_.state;
}

} // namespace ck::files
