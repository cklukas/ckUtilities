// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_journal.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace ck::files {

enum class OperationCommitState {
    Ready,
    CommittingDestination,
    RetiringSource,
    Complete,
    Failed,
    Cancelled,
    RetainedSource,
    NeedsReconciliation
};

struct OperationCommitRequest final {
    FileOperation operation = FileOperation::Copy;
    std::size_t item = 0;
    OperationJournal::Pointer journal;

    // These callbacks own all native handles and are invoked only on the
    // worker. Revalidation callbacks must not mutate. A successful commit or
    // retirement callback means the corresponding action is complete; an
    // unavailable result after invocation is therefore ambiguous and forces
    // reconciliation rather than guessing what the filesystem did.
    std::function<Observation<bool>(std::stop_token)> revalidate_source;
    std::function<Observation<bool>(std::stop_token)> revalidate_destination;
    // Move source retirement requires a positive content-stability proof after
    // destination commit. An identity check or before/after observation alone
    // is not such a proof; platform adapters must report false/unknown when
    // their supported snapshot or enforced-exclusion guarantee is absent.
    std::function<Observation<bool>(std::stop_token)> verify_source_stability;
    std::function<Observation<bool>(std::stop_token)> commit_destination;
    std::function<Observation<bool>(std::stop_token)> retire_source;

    // The sink must durably append the supplied record before returning
    // available. The job verifies that exactly that record became visible in
    // the in-memory journal before crossing the next boundary.
    std::function<Observation<bool>(OperationJournalRecord)> append_journal;
};

struct OperationCommitResult final {
    OperationCommitState state = OperationCommitState::Ready;
    std::optional<UnavailableObservation> failure;
};

class OperationCommitJob final {
public:
    using Pointer = std::unique_ptr<OperationCommitJob>;

    static Observation<Pointer> create(OperationCommitRequest request);

    OperationCommitState state() const noexcept { return result_.state; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return result_.failure; }
    const OperationCommitResult& result() const noexcept { return result_; }
    // Each advance performs at most one injected native action or one durable
    // journal transition. No action is started after cancellation or failure.
    OperationCommitState advance(std::stop_token stop = {});

private:
    explicit OperationCommitJob(OperationCommitRequest request) noexcept : request_(std::move(request)) {}

    bool append(OperationJournalState state);
    void fail(UnavailableObservation detail, bool reconciliation) noexcept;
    void fail(AdapterError code, const char* operation, bool reconciliation) noexcept;
    bool invoke_revalidation(const std::function<Observation<bool>(std::stop_token)>& callback,
                             std::stop_token stop, const char* operation,
                             OperationJournalState terminal_state);

    OperationCommitRequest request_;
    OperationCommitResult result_;
};

} // namespace ck::files
