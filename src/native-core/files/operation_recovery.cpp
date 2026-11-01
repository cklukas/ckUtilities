// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_recovery.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ck::files {
namespace {
template <class T>
Observation<T> rejected(AdapterError code, const char* operation) {
    return Observation<T>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

OperationRecoveryAction action(OperationJournalState state, FileOperation operation) noexcept {
    if (operation == FileOperation::Delete) {
        switch (state) {
        case OperationJournalState::Complete:
        case OperationJournalState::Failed:
        case OperationJournalState::Cancelled:
        case OperationJournalState::RetainedSource:
        case OperationJournalState::NeedsReconciliation:
            return OperationRecoveryAction::IgnoreTerminal;
        case OperationJournalState::SourceRemovalIntended:
        case OperationJournalState::SourceRetired:
            return OperationRecoveryAction::InspectAmbiguousDeletion;
        default:
            return OperationRecoveryAction::RequireReview;
        }
    }
    switch (state) {
    case OperationJournalState::Planned:
        return operation == FileOperation::Move ? OperationRecoveryAction::RequireReview
                                                : OperationRecoveryAction::InspectUnpublishedStaging;
    case OperationJournalState::Prepared:
        return operation == FileOperation::Move ? OperationRecoveryAction::RequireReview
                                                : OperationRecoveryAction::InspectUnpublishedStaging;
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

Observation<NativeOperationRecoveryInspection> inspect_native_operation_recovery_evidence(
    const OperationJournal& journal, std::optional<UnavailableObservation> tail,
    NativeOperationRecoveryEvidenceLimits evidence_limits) {
    auto generic = inspect_operation_recovery(journal, tail);
    if (!generic.value())
        return Observation<NativeOperationRecoveryInspection>::unavailable(
            generic.state(), generic.unavailable_detail()->error);

    std::vector<const OperationJournalRecord*> latest(journal.limits().max_items);
    for (const auto& record : journal.records()) {
        if (record.item >= latest.size())
            return rejected<NativeOperationRecoveryInspection>(AdapterError::InvalidNativeResult,
                                                               "native_recovery_evidence.item");
        latest[record.item] = &record;
    }

    NativeOperationRecoveryInspection result;
    result.report = std::move(generic).take_value();
    result.has_untrusted_evidence = result.report.has_untrusted_tail;
    for (auto& item : result.report.items) {
        if (item.action == OperationRecoveryAction::IgnoreTerminal) continue;
        const auto* record = latest[item.item];
        if (!record || record->evidence.empty()) {
            item.action = OperationRecoveryAction::RequireReview;
            item.requires_reconciliation = true;
            result.report.requires_reconciliation = true;
            result.has_untrusted_evidence = true;
            continue;
        }
        if (journal.operation() == FileOperation::Delete) {
            auto evidence = decode_native_delete_recovery_evidence(record->evidence, evidence_limits);
            if (!evidence.value()) {
                item.action = OperationRecoveryAction::RequireReview;
                item.requires_reconciliation = true;
                result.report.requires_reconciliation = true;
                result.has_untrusted_evidence = true;
                continue;
            }
            NativeOperationRecoveryArtifact artifact;
            artifact.item = item.item;
            artifact.state = item.state;
            artifact.deletion_evidence = std::move(evidence).take_value();
            result.artifacts.push_back(std::move(artifact));
            continue;
        }
        auto evidence = decode_native_operation_recovery_evidence(record->evidence, evidence_limits);
        if (!evidence.value()) {
            item.action = OperationRecoveryAction::RequireReview;
            item.requires_reconciliation = true;
            result.report.requires_reconciliation = true;
            result.has_untrusted_evidence = true;
            continue;
        }
        result.artifacts.push_back({item.item, item.state, std::move(evidence).take_value(), std::nullopt});
    }
    return Observation<NativeOperationRecoveryInspection>::available(std::move(result));
}

Observation<OperationRecoveryReport> inspect_native_operation_recovery(
    OperationJournalFile& file, OperationJournalLimits journal_limits,
    OperationJournalCodecLimits codec_limits) {
    auto loaded = load_operation_journal(file, journal_limits, codec_limits);
    if (!loaded.value()) {
        const auto& error = *loaded.unavailable_detail();
        return Observation<OperationRecoveryReport>::unavailable(error.state, error.error);
    }
    return inspect_operation_recovery(*loaded.value()->journal, loaded.value()->tail);
}

Observation<NativeOperationRecoveryInspection> inspect_native_operation_recovery_evidence(
    OperationJournalFile& file, OperationJournalLimits journal_limits,
    OperationJournalCodecLimits codec_limits, NativeOperationRecoveryEvidenceLimits evidence_limits) {
    auto loaded = load_operation_journal(file, journal_limits, codec_limits);
    if (!loaded.value()) {
        const auto& error = *loaded.unavailable_detail();
        return Observation<NativeOperationRecoveryInspection>::unavailable(error.state, error.error);
    }
    return inspect_native_operation_recovery_evidence(
        *loaded.value()->journal, loaded.value()->tail, evidence_limits);
}

} // namespace ck::files
