// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_recovery.hpp"

#include <cstdint>

namespace ck::files {
namespace {
template <class T>
Observation<T> rejected(AdapterError code, const char* operation) {
    return Observation<T>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

OperationRecoveryAction action(OperationJournalState state, FileOperation operation) noexcept {
    switch (state) {
    case OperationJournalState::Planned:
    case OperationJournalState::Prepared:
    case OperationJournalState::Copied:
    case OperationJournalState::Verified:
        return OperationRecoveryAction::InspectUnpublishedStaging;
    case OperationJournalState::CommitIntended:
        return OperationRecoveryAction::InspectAmbiguousPublication;
    case OperationJournalState::DestinationCommitted:
        return operation == FileOperation::Copy ? OperationRecoveryAction::VerifyDestination
                                                 : OperationRecoveryAction::VerifyDestinationBeforeSourceRetirement;
    case OperationJournalState::SourceRemovalIntended:
        return OperationRecoveryAction::InspectAmbiguousSourceRetirement;
    case OperationJournalState::SourceRetired:
        return OperationRecoveryAction::VerifyDestination;
    case OperationJournalState::Complete:
    case OperationJournalState::Failed:
    case OperationJournalState::Cancelled:
    case OperationJournalState::RetainedSource:
    case OperationJournalState::NeedsReconciliation:
        return OperationRecoveryAction::IgnoreTerminal;
    }
    return OperationRecoveryAction::RequireReview;
}

bool terminal(OperationJournalState state) noexcept {
    return operation_journal_terminal(state);
}
}

Observation<OperationRecoveryReport> inspect_operation_recovery(
    const OperationJournal& journal, std::optional<UnavailableObservation> tail) {
    if (journal.records().empty())
        return rejected<OperationRecoveryReport>(AdapterError::PlanInvalid, "operation_recovery.empty");

    std::vector<std::optional<OperationJournalState>> states(journal.limits().max_items);
    for (const auto& record : journal.records()) {
        if (record.item >= states.size())
            return rejected<OperationRecoveryReport>(AdapterError::InvalidNativeResult, "operation_recovery.item");
        states[record.item] = record.state;
    }

    OperationRecoveryReport report;
    report.has_untrusted_tail = tail.has_value();
    report.requires_reconciliation = tail.has_value();
    for (std::size_t item = 0; item != states.size(); ++item) {
        if (!states[item]) continue;
        const auto state = *states[item];
        const auto recovery = action(state, journal.operation());
        const bool needs = state == OperationJournalState::NeedsReconciliation ||
            !terminal(state) || recovery != OperationRecoveryAction::IgnoreTerminal;
        report.requires_reconciliation = report.requires_reconciliation || needs;
        report.items.push_back({item, state, recovery, needs});
    }
    if (report.items.empty())
        return rejected<OperationRecoveryReport>(AdapterError::PlanInvalid, "operation_recovery.no_items");
    return Observation<OperationRecoveryReport>::available(std::move(report));
}

} // namespace ck::files
