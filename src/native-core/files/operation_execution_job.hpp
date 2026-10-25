// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_commit_job.hpp"
#include "operation_copy_job.hpp"

#include <functional>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>

namespace ck::files {

enum class OperationExecutionState { Copying, PreparingCommit, Committing, Complete, Failed, Cancelled,
                                     RetainedSource, NeedsReconciliation };

struct OperationProgress final {
    OperationExecutionState state = OperationExecutionState::Copying;
    std::uint64_t observed_bytes = 0;
    // A total is optional by design. A source size observed before copying is
    // not a snapshot guarantee, so callers must not turn its absence into a
    // guessed percentage or an exact completion estimate.
    std::optional<std::uint64_t> total_bytes;
};

struct OperationExecutionRequest final {
    OperationCopyJob::Pointer copy;
    // Constructs the commit job only after the copy job has reached Verified.
    // Construction must be side-effect free; the returned commit job owns all
    // native callbacks and remains worker-bound.
    std::function<Observation<OperationCommitJob::Pointer>(std::stop_token)> prepare_commit;
    std::function<Observation<bool>()> cleanup_unpublished_staging;
};

struct OperationExecutionResult final {
    OperationExecutionState state = OperationExecutionState::Copying;
    std::optional<UnavailableObservation> failure;
};

// Worker-only composition of journaled copy and commit stages. It does not
// collapse their receipts: publication and source retirement remain behind the
// separate commit job's durable intent/revalidation boundaries.
class OperationExecutionJob final {
public:
    using Pointer = std::unique_ptr<OperationExecutionJob>;
    static Observation<Pointer> create(OperationExecutionRequest request);

    OperationExecutionState state() const noexcept { return result_.state; }
    const OperationExecutionResult& result() const noexcept { return result_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return result_.failure; }
    OperationProgress progress() const noexcept;
    OperationExecutionState advance(std::stop_token stop = {});
    Observation<bool> cleanup_unpublished_staging();

private:
    explicit OperationExecutionJob(OperationExecutionRequest request) noexcept : request_(std::move(request)) {}
    void fail(UnavailableObservation detail, OperationExecutionState state) noexcept;

    OperationExecutionRequest request_;
    OperationExecutionResult result_;
    OperationCommitJob::Pointer commit_;
};

} // namespace ck::files
