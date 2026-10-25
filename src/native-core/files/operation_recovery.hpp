// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

// Pure startup inspection. It never opens, deletes, replaces, resumes or
// retires a native object. A caller must obtain fresh held-object identity and
// digest evidence before turning any non-terminal action into a mutation.
Observation<OperationRecoveryReport> inspect_operation_recovery(
    const OperationJournal& journal,
    std::optional<UnavailableObservation> tail = std::nullopt);

// Read-only startup composition. The file remains worker-owned; a successful
// result contains the decoded journal and any untrusted tail projected into
// explicit review actions. No native object is opened or mutated here.
Observation<OperationRecoveryReport> inspect_native_operation_recovery(
    OperationJournalFile& file, OperationJournalLimits journal_limits = {},
    OperationJournalCodecLimits codec_limits = {});

} // namespace ck::files
