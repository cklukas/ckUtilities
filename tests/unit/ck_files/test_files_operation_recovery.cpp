// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_recovery.hpp"

#include <cvision/testing/cktest.hpp>

#include <stdexcept>

using namespace ck::files;

namespace {
OperationJournal::Pointer journal(FileOperation operation) {
    OperationJournalId id;
    id.bytes[0] = std::byte{0x71};
    auto result = OperationJournal::create(operation, id, {4, 32, 64});
    if (!result.value()) throw std::runtime_error("recovery journal fixture failed");
    return std::move(result).take_value();
}
void append(const OperationJournal::Pointer& value, std::uint64_t sequence, std::size_t item,
            OperationJournalState state) {
    if (!value->append({sequence, item, state, {}}).value())
        throw std::runtime_error("recovery state fixture failed");
}
}

CK_TEST(operation_recovery_ignores_completed_items_without_reconciliation) {
    auto value = journal(FileOperation::Copy);
    append(value, 1, 0, OperationJournalState::Planned);
    append(value, 2, 0, OperationJournalState::Prepared);
    append(value, 3, 0, OperationJournalState::Copied);
    append(value, 4, 0, OperationJournalState::Verified);
    append(value, 5, 0, OperationJournalState::CommitIntended);
    append(value, 6, 0, OperationJournalState::DestinationCommitted);
    append(value, 7, 0, OperationJournalState::Complete);
    auto report = inspect_operation_recovery(*value);
    CK_CHECK(report.value() && report.value()->items.size() == 1 && !report.value()->requires_reconciliation);
    if (report.value())
        CK_CHECK(report.value()->items[0].action == OperationRecoveryAction::IgnoreTerminal &&
                 !report.value()->items[0].requires_reconciliation);
}

CK_TEST(operation_recovery_maps_commit_and_retirement_boundaries_to_review_actions) {
    auto copy = journal(FileOperation::Copy);
    append(copy, 1, 0, OperationJournalState::Planned);
    append(copy, 2, 0, OperationJournalState::Prepared);
    append(copy, 3, 0, OperationJournalState::Copied);
    append(copy, 4, 0, OperationJournalState::Verified);
    append(copy, 5, 0, OperationJournalState::CommitIntended);
    auto copy_report = inspect_operation_recovery(*copy);
    CK_CHECK(copy_report.value() && copy_report.value()->requires_reconciliation);
    if (copy_report.value())
        CK_CHECK(copy_report.value()->items[0].action == OperationRecoveryAction::InspectAmbiguousPublication);

    auto move = journal(FileOperation::Move);
    append(move, 1, 0, OperationJournalState::Planned);
    append(move, 2, 0, OperationJournalState::Prepared);
    append(move, 3, 0, OperationJournalState::Copied);
    append(move, 4, 0, OperationJournalState::Verified);
    append(move, 5, 0, OperationJournalState::CommitIntended);
    append(move, 6, 0, OperationJournalState::DestinationCommitted);
    append(move, 7, 0, OperationJournalState::SourceRemovalIntended);
    auto move_report = inspect_operation_recovery(*move);
    CK_CHECK(move_report.value() && move_report.value()->items[0].action ==
             OperationRecoveryAction::InspectAmbiguousSourceRetirement);
}

CK_TEST(operation_recovery_requires_review_for_untrusted_tail_and_reconciliation_state) {
    auto value = journal(FileOperation::Copy);
    append(value, 1, 0, OperationJournalState::Planned);
    append(value, 2, 0, OperationJournalState::NeedsReconciliation);
    const UnavailableObservation tail{ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "test.tail"}};
    auto report = inspect_operation_recovery(*value, tail);
    CK_CHECK(report.value() && report.value()->has_untrusted_tail && report.value()->requires_reconciliation);
    if (report.value())
        CK_CHECK(report.value()->items[0].action == OperationRecoveryAction::IgnoreTerminal &&
                 report.value()->items[0].requires_reconciliation);
}
