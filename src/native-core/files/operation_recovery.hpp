// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_operation_recovery_evidence.hpp"
#include "native_delete_recovery_evidence.hpp"
#include "operation_journal.hpp"
#include "operation_journal_file.hpp"

#include <optional>
#include <vector>

namespace ck::files {

enum class OperationRecoveryAction {
    IgnoreTerminal,
    InspectUnpublishedStaging,
    VerifyDestination,
    VerifyDestinationBeforeSourceRetirement,
    InspectAmbiguousPublication,
    InspectAmbiguousSourceRetirement,
    InspectAmbiguousDeletion,
    RequireReview
};

struct OperationRecoveryItem final {
    std::size_t item = 0;
    OperationJournalState state = OperationJournalState::Planned;
    OperationRecoveryAction action = OperationRecoveryAction::RequireReview;
    bool requires_reconciliation = true;
};

struct OperationRecoveryReport final {
    std::vector<OperationRecoveryItem> items;
    bool requires_reconciliation = false;
    bool has_untrusted_tail = false;
};

struct NativeOperationRecoveryArtifact final {
    std::size_t item = 0;
    OperationJournalState state = OperationJournalState::Planned;
    std::optional<NativeOperationRecoveryEvidence> evidence;
    std::optional<NativeDeleteRecoveryEvidence> deletion_evidence;
};

struct NativeOperationRecoveryInspection final {
    OperationJournalId journal_id{};
    OperationRecoveryReport report;
    std::vector<NativeOperationRecoveryArtifact> artifacts;
    bool has_untrusted_evidence = false;
};

// Pure startup inspection. It never opens, deletes, replaces, resumes or
// retires a native object. A caller must obtain fresh held-object identity and
// digest evidence before turning any non-terminal action into a mutation.
Observation<OperationRecoveryReport> inspect_operation_recovery(
    const OperationJournal& journal,
    std::optional<UnavailableObservation> tail = std::nullopt);

// Read-only native projection. It decodes evidence only for items whose
// generic recovery action needs an artifact. Missing or malformed evidence
// changes that item to RequireReview; no path, handle or mutation is produced.
Observation<NativeOperationRecoveryInspection> inspect_native_operation_recovery_evidence(
    const OperationJournal& journal,
    std::optional<UnavailableObservation> tail = std::nullopt,
    NativeOperationRecoveryEvidenceLimits evidence_limits = {});

// Read-only startup composition. The file remains worker-owned; a successful
// result contains the decoded journal and any untrusted tail projected into
// explicit review actions. No native object is opened or mutated here.
Observation<OperationRecoveryReport> inspect_native_operation_recovery(
    OperationJournalFile& file, OperationJournalLimits journal_limits = {},
    OperationJournalCodecLimits codec_limits = {});

// Read-only native startup composition. This retains the durable loader's
// explicit tail condition while also projecting the latest native evidence;
// an untrusted tail or evidence record remains review-only.
Observation<NativeOperationRecoveryInspection> inspect_native_operation_recovery_evidence(
    OperationJournalFile& file, OperationJournalLimits journal_limits = {},
    OperationJournalCodecLimits codec_limits = {},
    NativeOperationRecoveryEvidenceLimits evidence_limits = {});

} // namespace ck::files
