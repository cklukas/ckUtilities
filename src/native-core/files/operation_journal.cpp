// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal.hpp"

#include <algorithm>
#include <limits>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char *operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

bool valid_limits(OperationJournalLimits limits) noexcept {
    return limits.max_items && limits.max_items <= OperationJournalLimits::hard_max_items &&
        limits.max_records && limits.max_records <= OperationJournalLimits::hard_max_records &&
        limits.max_evidence_bytes <= OperationJournalLimits::hard_max_evidence_bytes;
}

bool valid_state(OperationJournalState state) noexcept {
    return static_cast<unsigned>(state) <=
        static_cast<unsigned>(OperationJournalState::NeedsReconciliation);
}

} // namespace

bool operation_journal_terminal(OperationJournalState state) noexcept {
    return state == OperationJournalState::Complete || state == OperationJournalState::Failed ||
        state == OperationJournalState::Cancelled || state == OperationJournalState::RetainedSource ||
        state == OperationJournalState::NeedsReconciliation;
}

bool operation_journal_transition(FileOperation operation, OperationJournalState from,
                                  OperationJournalState to) noexcept {
    if (!valid_state(from) || !valid_state(to) || operation_journal_terminal(from))
        return false;
    if (to == OperationJournalState::Failed || to == OperationJournalState::Cancelled ||
        to == OperationJournalState::RetainedSource || to == OperationJournalState::NeedsReconciliation)
        return true;
    switch (from) {
    case OperationJournalState::Planned:
        return to == OperationJournalState::Prepared;
    case OperationJournalState::Prepared:
        return to == OperationJournalState::Copied;
    case OperationJournalState::Copied:
        return to == OperationJournalState::Verified;
    case OperationJournalState::Verified:
        return to == OperationJournalState::CommitIntended;
    case OperationJournalState::CommitIntended:
        return to == OperationJournalState::DestinationCommitted;
    case OperationJournalState::DestinationCommitted:
        return operation == FileOperation::Copy ? to == OperationJournalState::Complete
                                                 : to == OperationJournalState::SourceRemovalIntended;
    case OperationJournalState::SourceRemovalIntended:
        return operation == FileOperation::Move && to == OperationJournalState::SourceRetired;
    case OperationJournalState::SourceRetired:
        return operation == FileOperation::Move && to == OperationJournalState::Complete;
    case OperationJournalState::Complete:
    case OperationJournalState::Failed:
    case OperationJournalState::Cancelled:
    case OperationJournalState::RetainedSource:
    case OperationJournalState::NeedsReconciliation:
        return false;
    }
    return false;
}

Observation<OperationJournal::Pointer> OperationJournal::create(FileOperation operation,
                                                                OperationJournalLimits limits) {
    if (operation != FileOperation::Copy && operation != FileOperation::Move)
        return rejected<Pointer>(AdapterError::PlanInvalid, "operation_journal.operation");
    if (!valid_limits(limits))
        return rejected<Pointer>(AdapterError::InvalidLimit, "operation_journal.limits");
    return Observation<Pointer>::available(Pointer(new OperationJournal(operation, limits)));
}

Observation<bool> OperationJournal::append(OperationJournalRecord record) {
    if (!valid_state(record.state))
        return rejected<bool>(AdapterError::PlanInvalid, "operation_journal.state");
    if (record.item >= limits_.max_items || record.evidence.size() > limits_.max_evidence_bytes)
        return rejected<bool>(AdapterError::InventoryLimit, "operation_journal.record_limit",
                              ObservationState::Unknown);
    if (records_.size() >= limits_.max_records)
        return rejected<bool>(AdapterError::InventoryLimit, "operation_journal.records",
                              ObservationState::Unknown);
    const auto expected_sequence = static_cast<std::uint64_t>(records_.size()) + 1;
    if (record.sequence != expected_sequence)
        return rejected<bool>(AdapterError::PlanInvalid, "operation_journal.sequence");
    const auto previous = state(record.item);
    if (previous) {
        if (!operation_journal_transition(operation_, *previous, record.state))
            return rejected<bool>(AdapterError::PlanInvalid, "operation_journal.transition");
    } else if (record.state != OperationJournalState::Planned) {
        return rejected<bool>(AdapterError::PlanInvalid, "operation_journal.first_state");
    }
    try {
        records_.push_back(std::move(record));
    } catch (...) {
        return rejected<bool>(AdapterError::InventoryLimit, "operation_journal.append",
                              ObservationState::Unknown);
    }
    return Observation<bool>::available(true);
}

std::optional<OperationJournalState> OperationJournal::state(std::size_t item) const noexcept {
    for (auto it = records_.rbegin(); it != records_.rend(); ++it)
        if (it->item == item)
            return it->state;
    return std::nullopt;
}

bool OperationJournal::terminal(std::size_t item) const noexcept {
    const auto current = state(item);
    return current && operation_journal_terminal(*current);
}

} // namespace ck::files
