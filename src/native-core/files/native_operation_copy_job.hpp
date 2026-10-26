// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_execution_job.hpp"
#include "operation_batch_job.hpp"
#include "operation_journal_file.hpp"
#include "held_child.hpp"
#include "recursive_inventory_job.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <utility>
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

    // Removes only an unpublished, worker-created staging file after a
    // non-ambiguous Failed/Cancelled outcome. Platforms without a qualified
    // handle-bound disposition return Unsupported; ambiguous publication is
    // never cleaned by this API.
    Observation<bool> cleanup_unpublished_staging();

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

// Worker-only construction inputs for a regular-file copy batch. Sources are
// held objects from one pane and destination is a held directory from the
// other pane; neither is converted back into a path. This boundary is
// intentionally regular-file-only until recursive directory composition has
// its own qualified implementation.
struct NativeOperationCopyBatchPolicy final {
    StreamCopyLimits copy_limits{};
    OperationJournalLimits journal_limits{};
    bool allow_unhandled_metadata_loss = false;
};

struct NativeOperationCopyBatchRequest final {
    std::vector<HeldChild::Pointer> sources;
    std::shared_ptr<const DirectoryCursor> destination;
    std::vector<NativeName> destination_names;
    std::optional<NativeAbsolutePath> journal_root;
    OperationJournalId journal_id{};
    NativeOperationCopyBatchPolicy policy{};
    // Platform adapter injection keeps the portable core independent of the
    // POSIX/Windows journal opener while retaining exact native names.
    std::function<Observation<std::unique_ptr<OperationJournalFile>>(
        const NativeAbsolutePath&, const NativeName&, bool)> open_journal;
};

// Creates and durably marks one journal per item before returning the ordered
// batch. If a later item cannot be composed, earlier journals remain on disk
// for explicit recovery; the factory never silently removes evidence.
Observation<OperationBatchJob::Pointer> create_native_operation_copy_batch(
    NativeOperationCopyBatchRequest request);

// Composes already-qualified native copy jobs without changing their
// per-item journal or failure semantics. The batch remains strictly ordered.
Observation<OperationBatchJob::Pointer> create_native_operation_copy_batch(
    std::vector<NativeOperationCopyJob::Pointer> jobs);

// Worker-only recursive directory copy. The completed inventory and every
// HeldChild remain native worker state; relative_components are consumed as
// exact NativeName values and are never reconstructed from display paths.
// The root is built below a private directory name and published with the
// qualified no-replace directory rename primitive. Recursive cleanup is
// intentionally not guessed: an interrupted or failed tree remains retained
// for explicit reconciliation.
struct NativeOperationCopyTreeRequest final {
    explicit NativeOperationCopyTreeRequest(NativeName name) : destination_name(std::move(name)) {}

    std::shared_ptr<const CompletedRecursiveInventory> inventory;
    std::shared_ptr<const DirectoryCursor> destination;
    NativeName destination_name;
    std::optional<NativeAbsolutePath> journal_root;
    OperationJournalId journal_id{};
    NativeOperationCopyBatchPolicy policy{};
    std::function<Observation<std::unique_ptr<OperationJournalFile>>(
        const NativeAbsolutePath&, const NativeName&, bool)> open_journal;
};

class NativeOperationCopyTreeJob final {
public:
    using Pointer = std::unique_ptr<NativeOperationCopyTreeJob>;

    static Observation<Pointer> create(NativeOperationCopyTreeRequest request);

    OperationExecutionState state() const noexcept { return state_; }
    OperationProgress progress() const noexcept;
    const OperationExecutionResult& result() const noexcept { return result_; }
    OperationExecutionState advance(std::stop_token stop = {});

    // There is no safe recursive directory disposition in the current native
    // contract. This API therefore refuses cleanup and leaves the private
    // tree available for recovery inspection.
    Observation<bool> cleanup_unpublished_staging();

private:
    struct State;
    explicit NativeOperationCopyTreeJob(std::shared_ptr<State> state) noexcept
        : state_data_(std::move(state)) {}

    std::shared_ptr<State> state_data_;
    OperationExecutionState state_ = OperationExecutionState::Copying;
    OperationExecutionResult result_;
};

Observation<NativeOperationCopyTreeJob::Pointer> create_native_operation_copy_tree(
    NativeOperationCopyTreeRequest request);

} // namespace ck::files
