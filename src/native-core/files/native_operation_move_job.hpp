// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "held_child.hpp"
#include "native_operation_copy_job.hpp"
#include "operation_batch_job.hpp"
#include "operation_journal_file.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ck::files {

// Worker-owned move. Same-filesystem moves use an identity-checked,
// no-replace native rename. Cross-filesystem regular-file moves use verified
// copy, durable publication, and source retirement only where the adapter can
// enforce source-writer exclusion; unsupported cases retain the source.
struct NativeOperationMoveRequest final {
    OperationJournal::Pointer journal;
    std::shared_ptr<OperationJournalFile> journal_file;
    std::size_t item = 0;
    HeldChild::Pointer source;
    std::shared_ptr<const DirectoryCursor> destination;
    ObjectIdentity destination_identity;
    NativeName destination_name;
    StreamCopyLimits copy_limits{};
    bool allow_unhandled_metadata_loss = false;
};

class NativeOperationMoveJob final {
public:
    using Pointer = std::unique_ptr<NativeOperationMoveJob>;

    static Observation<Pointer> create(NativeOperationMoveRequest request);

    OperationExecutionState state() const noexcept { return phase_; }
    const OperationExecutionResult& result() const noexcept { return result_; }
    OperationExecutionState advance(std::stop_token stop = {});

private:
    struct State;
    explicit NativeOperationMoveJob(std::shared_ptr<State> state,
                                   NativeOperationCopyJob::Pointer cross_filesystem_copy = {}) noexcept
        : state_(std::move(state)), cross_filesystem_copy_(std::move(cross_filesystem_copy)) {}

    std::shared_ptr<State> state_;
    NativeOperationCopyJob::Pointer cross_filesystem_copy_;
    OperationExecutionState phase_ = OperationExecutionState::Copying;
    OperationExecutionResult result_;
};

struct NativeOperationMoveBatchRequest final {
    std::vector<HeldChild::Pointer> sources;
    std::shared_ptr<const DirectoryCursor> destination;
    std::vector<NativeName> destination_names;
    std::optional<NativeAbsolutePath> journal_root;
    OperationJournalId journal_id{};
    OperationJournalLimits journal_limits{};
    std::function<Observation<std::unique_ptr<OperationJournalFile>>(
        const NativeAbsolutePath&, const NativeName&, bool)> open_journal;
    StreamCopyLimits copy_limits{};
    bool allow_unhandled_metadata_loss = false;
};

class NativeOperationMoveBatchJob final {
public:
    using Pointer = std::unique_ptr<NativeOperationMoveBatchJob>;

    static Observation<Pointer> create(std::vector<NativeOperationMoveJob::Pointer> jobs);
    OperationBatchState state() const noexcept { return result_.state; }
    const OperationBatchResult& result() const noexcept { return result_; }
    OperationBatchState advance(std::stop_token stop = {});

private:
    explicit NativeOperationMoveBatchJob(std::vector<NativeOperationMoveJob::Pointer> jobs);
    std::vector<NativeOperationMoveJob::Pointer> jobs_;
    OperationBatchResult result_;
    std::vector<bool> started_;
    std::size_t current_ = 0;
};

Observation<NativeOperationMoveBatchJob::Pointer> create_native_operation_move_batch(
    NativeOperationMoveBatchRequest request);

} // namespace ck::files
