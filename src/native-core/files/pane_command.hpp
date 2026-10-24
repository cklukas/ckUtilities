// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pane_model.hpp"
#include <variant>

namespace ck::files {
struct PaneFocusCommand { PaneRowId row; };
struct PaneMarkCommand { PaneRowId row; bool marked; };
// Endpoints are row IDs from one reviewed page/state, never mutable positions.
struct PaneMarkRangeCommand { PaneRowId first, last; bool marked; };
struct PaneMarkVisibleCommand { bool marked; };
struct PaneInvertVisibleCommand {};
struct PaneClearMarksCommand {};
struct PaneSetViewCommand { PaneView view; };
using PaneCommand = std::variant<PaneFocusCommand, PaneMarkCommand, PaneMarkRangeCommand,
    PaneMarkVisibleCommand, PaneInvertVisibleCommand, PaneClearMarksCommand, PaneSetViewCommand>;
enum class PaneCommandOutcome { Applied, Stale, InvalidTarget, InvalidCommand };
// Execute on the model's serialized owning worker. The same stamp gates the
// entire command before any change. Bulk/range/view work may traverse the model
// and allocate; it never belongs in UI dispatch. Invalid view/target commands
// leave state/stamps unchanged; allocation failures propagate unchanged.
// Commands only affect pane state. They neither open files nor plan/authorize
// filesystem operations. Listing-pending/invalidated selection stays explicit.
PaneCommandOutcome apply_pane_command(PaneModel& model, const PaneStateStamp& stamp, const PaneCommand& command);
} // namespace ck::files
