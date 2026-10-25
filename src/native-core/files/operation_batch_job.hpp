// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_execution_job.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

namespace ck::files {

enum class OperationBatchState { Ready, Running, Complete, Failed, Cancelled,
                                 RetainedSource, NeedsReconciliation };

enum class OperationItemOutcome { Pending, Complete, Failed, Cancelled,
                                  RetainedSource, NeedsReconciliation };

struct OperationBatchItemResult final {
    std::size_t item = 0;
    OperationItemOutcome outcome = OperationItemOutcome::Pending;
    std::optional<UnavailableObservation> failure;
};

struct OperationBatchResult final {
    OperationBatchState state = OperationBatchState::Ready;
    std::vector<OperationBatchItemResult> items;
};

struct OperationBatchRequest final {
    // Jobs are executed strictly in vector order. A terminal item does not
    // authorize any later item to start until its result has been recorded.
    std::vector<OperationExecutionJob::Pointer> jobs;
};

// Worker-only serialized mutation coordinator. It never collapses partial
// item outcomes into a boolean and never starts a new item after cancellation.
class OperationBatchJob final {
public:
    using Pointer = std::unique_ptr<OperationBatchJob>;

    static Observation<Pointer> create(OperationBatchRequest request);

    OperationBatchState state() const noexcept { return result_.state; }
    const OperationBatchResult& result() const noexcept { return result_; }
    std::size_t current_item() const noexcept { return current_; }
    OperationBatchState advance(std::stop_token stop = {});
    Observation<bool> cleanup_item(std::size_t item);

private:
    explicit OperationBatchJob(OperationBatchRequest request);

    static OperationItemOutcome item_outcome(OperationExecutionState state) noexcept;
    static bool terminal(OperationExecutionState state) noexcept;
    void record_current();
    void finish_if_done();

    OperationBatchRequest request_;
    OperationBatchResult result_;
    std::vector<bool> started_;
    std::size_t current_ = 0;
};

} // namespace ck::files
