// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_execution_job.hpp"
#include "operation_journal_file.hpp"
#include "native_delete_recovery_evidence.hpp"
#include "recursive_inventory_job.hpp"

namespace ck::files {

struct NativeOperationDeleteTreeRequest final {
    std::shared_ptr<const CompletedRecursiveInventory> inventory;
    std::shared_ptr<OperationJournalFile> journal_file;
    std::optional<NativeAbsolutePath> journal_root;
    OperationJournalId journal_id{};
    OperationJournalLimits journal_limits{};
    std::function<Observation<std::unique_ptr<OperationJournalFile>>(
        const NativeAbsolutePath&, const NativeName&, bool)> open_journal;
};

// Worker-owned recursive permanent deletion. The inventory is frozen before
// mutation, entries are retired in reverse order, and every native action is
// identity-bound to the held parent/name pair. No links are followed and no
// newly appearing child can enter the deletion set.
class NativeOperationDeleteTreeJob final {
public:
    using Pointer = std::unique_ptr<NativeOperationDeleteTreeJob>;

    static Observation<Pointer> create(NativeOperationDeleteTreeRequest request);

    OperationExecutionState state() const noexcept { return state_; }
    OperationProgress progress() const noexcept;
    const OperationExecutionResult& result() const noexcept { return result_; }
    OperationExecutionState advance(std::stop_token stop = {});

private:
    struct State;
    explicit NativeOperationDeleteTreeJob(std::shared_ptr<State> state) noexcept
        : state_data_(std::move(state)) {}

    std::shared_ptr<State> state_data_;
    OperationExecutionState state_ = OperationExecutionState::PreparingCommit;
    OperationExecutionResult result_;
};

} // namespace ck::files
