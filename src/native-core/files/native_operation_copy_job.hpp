// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_execution_job.hpp"
#include "operation_batch_job.hpp"
#include "operation_journal_file.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace ck::files {

// The native composition root for one regular-file copy. All directory and
// entry owners remain inside the worker-owned job. The destination name is
// never reopened from display text and publication is no-replace only.
struct NativeOperationCopyRequest final {
    OperationJournal::Pointer journal;
    std::shared_ptr<OperationJournalFile> journal_file;
    std::size_t item = 0;
    std::shared_ptr<const DirectoryCursor> source_parent;
    ObjectIdentity source_parent_identity;
    NativeName source_name;
    ObjectIdentity source_identity;
    std::shared_ptr<const DirectoryCursor> destination;
    ObjectIdentity destination_identity;
    NativeName staging_name;
    NativeName destination_name;
    StreamCopyLimits copy_limits{};

    // This callback must construct every metadata phase, including explicit
    // no-op decisions. It receives the private staging entry and may only use
    // worker-owned native state. An absent callback is rejected: the native
    // copy path must never imply that unhandled metadata was preserved.
    std::function<Observation<OperationMetadataJob::Pointer>(OpenedEntry&, std::stop_token)>
        prepare_metadata;
};

// Worker-only native binding of OperationCopyJob and OperationCommitJob. This
// class deliberately supports Copy only until native metadata adapters and
// move retirement are independently qualified.
class NativeOperationCopyJob final {
public:
    using Pointer = std::unique_ptr<NativeOperationCopyJob>;

    static Observation<Pointer> create(NativeOperationCopyRequest request);

    OperationExecutionState state() const noexcept { return execution_->state(); }
    OperationProgress progress() const noexcept { return execution_->progress(); }
    const OperationExecutionResult& result() const noexcept { return execution_->result(); }
    OperationExecutionState advance(std::stop_token stop = {});

    // Transfers the worker-bound execution job into the serialized batch
    // coordinator. The callbacks retain State, so native owners remain alive
    // after this wrapper is moved and never cross into UI state.
    OperationExecutionJob::Pointer release_execution_job() && noexcept {
        state_.reset();
        return std::move(execution_);
    }

private:
    struct State;
    NativeOperationCopyJob(std::shared_ptr<State> state, OperationExecutionJob::Pointer execution) noexcept
        : state_(std::move(state)), execution_(std::move(execution)) {}

    std::shared_ptr<State> state_;
    OperationExecutionJob::Pointer execution_;
};

// Composes already-qualified native copy jobs without changing their
// per-item journal or failure semantics. The batch remains strictly ordered.
Observation<OperationBatchJob::Pointer> create_native_operation_copy_batch(
    std::vector<NativeOperationCopyJob::Pointer> jobs);

} // namespace ck::files
