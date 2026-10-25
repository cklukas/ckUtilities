// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal.hpp"

#include <cvision/testing/cktest.hpp>

#include <stdexcept>

using namespace ck::files;

namespace {
OperationJournalId id() {
    OperationJournalId result;
    result.bytes[0] = std::byte{1};
    return result;
}
OperationJournal::Pointer journal(FileOperation operation = FileOperation::Copy) {
    auto result = OperationJournal::create(operation, id(), {4, 32, 64});
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
    auto invalid = OperationJournal::create(FileOperation::Copy, id(), {0, 1, 1});
    CK_CHECK(!invalid.value() && invalid.unavailable_detail()->error.code ==
        static_cast<std::int64_t>(AdapterError::InvalidLimit));
}

CK_TEST(operation_journal_codec_roundtrips_records_and_preserves_opaque_evidence) {
    auto value = journal();
    value->append({1, 0, OperationJournalState::Planned, {std::byte{1}, std::byte{2}}});
    value->append({2, 0, OperationJournalState::Prepared, {std::byte{3}}});
    auto encoded = encode_operation_journal(*value);
    CK_CHECK(encoded.value());
    auto decoded = decode_operation_journal(*encoded.value(), {4, 32, 64});
    CK_CHECK(decoded.value() && decoded.value()->journal && !decoded.value()->tail);
    if (decoded.value() && decoded.value()->journal) {
        CK_CHECK(decoded.value()->journal->operation() == FileOperation::Copy);
        CK_CHECK(decoded.value()->journal->id() == id());
        CK_CHECK(decoded.value()->journal->records().size() == 2);
        CK_CHECK(decoded.value()->journal->records()[0].evidence.size() == 2);
        CK_CHECK(decoded.value()->journal->records()[1].evidence.size() == 1);
    }
}

CK_TEST(operation_journal_codec_reports_truncated_or_corrupt_tail_without_erasing_older_records) {
    auto value = journal();
    value->append({1, 0, OperationJournalState::Planned, {}});
    value->append({2, 0, OperationJournalState::Prepared, {}});
    auto encoded = encode_operation_journal(*value);
    CK_CHECK(encoded.value() && encoded.value()->size() > 4);
    if (!encoded.value())
        return;
    auto truncated = *encoded.value();
    truncated.pop_back();
    auto truncated_result = decode_operation_journal(truncated, {4, 32, 64});
    CK_CHECK(truncated_result.value() && truncated_result.value()->journal && truncated_result.value()->tail);
    if (truncated_result.value() && truncated_result.value()->journal)
        CK_CHECK(truncated_result.value()->journal->records().size() == 1);
    auto corrupt = *encoded.value();
    corrupt[corrupt.size() - 1] ^= std::byte{1};
    auto corrupt_result = decode_operation_journal(corrupt, {4, 32, 64});
    CK_CHECK(corrupt_result.value() && corrupt_result.value()->journal && corrupt_result.value()->tail);
    if (corrupt_result.value() && corrupt_result.value()->journal)
        CK_CHECK(corrupt_result.value()->journal->records().size() == 1);
}
