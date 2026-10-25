// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal.hpp"

#include <cvision/testing/cktest.hpp>

#include <stdexcept>

using namespace ck::files;

namespace {
OperationJournal::Pointer journal(FileOperation operation = FileOperation::Copy) {
    auto result = OperationJournal::create(operation, {4, 32, 64});
    if (!result.value())
        throw std::runtime_error("journal fixture creation failed");
    return std::move(result).take_value();
}
void append(const OperationJournal::Pointer &journal, std::uint64_t sequence, std::size_t item,
            OperationJournalState state) {
    auto result = journal->append({sequence, item, state, {}});
    if (!result.value())
        throw std::runtime_error("journal fixture append failed");
}
} // namespace

CK_TEST(operation_journal_copy_requires_verified_commit_before_completion) {
    auto value = journal();
    append(value, 1, 0, OperationJournalState::Planned);
    append(value, 2, 0, OperationJournalState::Prepared);
    append(value, 3, 0, OperationJournalState::Copied);
    append(value, 4, 0, OperationJournalState::Verified);
    append(value, 5, 0, OperationJournalState::CommitIntended);
    append(value, 6, 0, OperationJournalState::DestinationCommitted);
    append(value, 7, 0, OperationJournalState::Complete);
    CK_CHECK(value->state(0) == OperationJournalState::Complete && value->terminal(0));
    CK_CHECK(value->records().size() == 7);
}

CK_TEST(operation_journal_move_requires_source_intent_and_retirement_after_destination_commit) {
    auto value = journal(FileOperation::Move);
    append(value, 1, 3, OperationJournalState::Planned);
    append(value, 2, 3, OperationJournalState::Prepared);
    append(value, 3, 3, OperationJournalState::Copied);
    append(value, 4, 3, OperationJournalState::Verified);
    append(value, 5, 3, OperationJournalState::CommitIntended);
    append(value, 6, 3, OperationJournalState::DestinationCommitted);
    append(value, 7, 3, OperationJournalState::SourceRemovalIntended);
    append(value, 8, 3, OperationJournalState::SourceRetired);
    append(value, 9, 3, OperationJournalState::Complete);
    CK_CHECK(value->state(3) == OperationJournalState::Complete && value->terminal(3));
}

CK_TEST(operation_journal_rejects_gaps_invalid_edges_and_terminal_rewrites) {
    auto value = journal();
    CK_CHECK(!value->append({2, 0, OperationJournalState::Planned, {}}).value());
    append(value, 1, 0, OperationJournalState::Planned);
    CK_CHECK(!value->append({2, 0, OperationJournalState::Copied, {}}).value());
    append(value, 2, 0, OperationJournalState::Prepared);
    append(value, 3, 0, OperationJournalState::Cancelled);
    CK_CHECK(!value->append({4, 0, OperationJournalState::Failed, {}}).value());
    CK_CHECK(!value->append({4, 1, OperationJournalState::Prepared, {}}).value());
}

CK_TEST(operation_journal_failure_reconciliation_and_record_limits_are_explicit) {
    auto value = journal();
    auto writable = value;
    std::vector<std::byte> evidence(64, std::byte{0x7f});
    CK_CHECK(writable->append({1, 0, OperationJournalState::Planned, evidence}).value());
    CK_CHECK(writable->append({2, 0, OperationJournalState::Prepared, {}}).value());
    CK_CHECK(writable->append({3, 0, OperationJournalState::NeedsReconciliation, {}}).value());
    CK_CHECK(writable->terminal(0));
    CK_CHECK(!writable->append({4, 1, OperationJournalState::Planned, std::vector<std::byte>(65)}).value());
    auto invalid = OperationJournal::create(FileOperation::Copy, {0, 1, 1});
    CK_CHECK(!invalid.value() && invalid.unavailable_detail()->error.code ==
        static_cast<std::int64_t>(AdapterError::InvalidLimit));
}
