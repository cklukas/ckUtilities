// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_recovery.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "operation_journal_file.hpp"

#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

using namespace ck::files;

namespace {
class MemoryJournalFile final : public OperationJournalFile {
public:
    std::vector<std::byte> bytes;

    Observation<std::uint64_t> size() override {
        return Observation<std::uint64_t>::available(bytes.size());
    }
    Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) override {
        if (offset > bytes.size())
            return Observation<std::size_t>::unavailable(ObservationState::Gone,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "recovery.file_offset"});
        const auto count = std::min(output.size(), bytes.size() - static_cast<std::size_t>(offset));
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
        return Observation<std::size_t>::available(count);
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> input) override {
        if (offset > bytes.size()) bytes.resize(static_cast<std::size_t>(offset));
        if (offset + input.size() > bytes.size()) bytes.resize(static_cast<std::size_t>(offset + input.size()));
        std::copy(input.begin(), input.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return Observation<std::size_t>::available(input.size());
    }
    Observation<bool> flush() override { return Observation<bool>::available(true); }
};

OperationJournal::Pointer journal(FileOperation operation) {
    OperationJournalId id;
    id.bytes[0] = std::byte{0x71};
    auto result = OperationJournal::create(operation, id, {4, 32, 4096});
    if (!result.value()) throw std::runtime_error("recovery journal fixture failed");
    return std::move(result).take_value();
}
void append(const OperationJournal::Pointer& value, std::uint64_t sequence, std::size_t item,
            OperationJournalState state) {
    if (!value->append({sequence, item, state, {}}).value())
        throw std::runtime_error("recovery state fixture failed");
}

NativeOperationRecoveryEvidence evidence() {
    AdapterScope scope;
    const auto identity = *NativeDurableIdentity::from_live(
        {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(7),
         NativeIdentifier::from_u64(8), {}});
    return {*NativeName::from_posix(".ck-stage"), *NativeName::from_posix("destination"),
            identity, identity, identity, identity};
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

CK_TEST(native_operation_recovery_projection_decodes_evidence_without_authorizing_mutation) {
    AdapterScope scope;
    OperationJournalId id;
    id.bytes[0] = std::byte{0x73};
    auto created = OperationJournal::create(FileOperation::Copy, id, {4, 32, 4096});
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto value = std::move(created).take_value();
    CK_CHECK(value->append({1, 0, OperationJournalState::Planned, {}}).value());
    const auto identity = *NativeDurableIdentity::from_live(
        {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(7),
         NativeIdentifier::from_u64(8), {}});
    NativeOperationRecoveryEvidence evidence{
        *NativeName::from_posix(".ck-stage"), *NativeName::from_posix("destination"),
        identity, identity, identity, {}};
    const auto encoded = encode_native_operation_recovery_evidence(evidence);
    CK_CHECK(encoded.value());
    if (!encoded.value()) return;
    CK_CHECK(value->append({2, 0, OperationJournalState::Prepared, *encoded.value()}).value());
    auto inspection = inspect_native_operation_recovery_evidence(*value);
    CK_CHECK(inspection.value() && inspection.value()->artifacts.size() == 1 &&
             !inspection.value()->has_untrusted_evidence);
    if (inspection.value()) {
        CK_CHECK(inspection.value()->artifacts[0].evidence.staging_name == evidence.staging_name);
        CK_CHECK(inspection.value()->report.items[0].action == OperationRecoveryAction::InspectUnpublishedStaging);
    }
}

CK_TEST(native_operation_recovery_projection_requires_review_for_missing_or_corrupt_evidence) {
    OperationJournalId id;
    id.bytes[0] = std::byte{0x74};
    auto created = OperationJournal::create(FileOperation::Copy, id, {4, 32, 4096});
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto value = std::move(created).take_value();
    CK_CHECK(value->append({1, 0, OperationJournalState::Planned, {}}).value());
    CK_CHECK(value->append({2, 0, OperationJournalState::Prepared, {std::byte{0xFF}}}).value());
    auto inspection = inspect_native_operation_recovery_evidence(*value);
    CK_CHECK(inspection.value() && inspection.value()->has_untrusted_evidence &&
             inspection.value()->report.requires_reconciliation &&
             inspection.value()->artifacts.empty());
    if (inspection.value())
        CK_CHECK(inspection.value()->report.items[0].action == OperationRecoveryAction::RequireReview);
}

CK_TEST(native_operation_recovery_file_projection_loads_durable_evidence) {
    MemoryJournalFile file;
    auto value = journal(FileOperation::Copy);
    CK_CHECK(append_operation_journal_file(file, *value,
        {1, 0, OperationJournalState::Planned, {}}).value());
    auto encoded = encode_native_operation_recovery_evidence(evidence());
    CK_CHECK(encoded.value());
    if (!encoded.value()) return;
    CK_CHECK(append_operation_journal_file(file, *value,
        {2, 0, OperationJournalState::Prepared, *encoded.value()}).value());

    auto inspection = inspect_native_operation_recovery_evidence(file, {2, 8, 4096});
    CK_CHECK(inspection.value() && inspection.value()->artifacts.size() == 1 &&
             !inspection.value()->has_untrusted_evidence);
    if (inspection.value())
        CK_CHECK(inspection.value()->artifacts[0].evidence.staging_identity.has_value() &&
                 inspection.value()->report.items[0].action == OperationRecoveryAction::InspectUnpublishedStaging);
}

CK_TEST(native_operation_recovery_file_projection_keeps_truncated_tail_review_only) {
    MemoryJournalFile file;
    auto value = journal(FileOperation::Copy);
    CK_CHECK(append_operation_journal_file(file, *value,
        {1, 0, OperationJournalState::Planned, {}}).value());
    file.bytes.push_back(std::byte{0xff});

    auto inspection = inspect_native_operation_recovery_evidence(file, {2, 8, 4096});
    CK_CHECK(inspection.value() && inspection.value()->report.has_untrusted_tail &&
             inspection.value()->has_untrusted_evidence && inspection.value()->report.requires_reconciliation);
    if (inspection.value())
        CK_CHECK(inspection.value()->report.items[0].action == OperationRecoveryAction::RequireReview &&
                 inspection.value()->artifacts.empty());
}
