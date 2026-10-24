// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_command.hpp"
#include <stdexcept>
#include <type_traits>

namespace ck::files {
PaneCommandOutcome apply_pane_command(PaneModel& model, const PaneStateStamp& stamp, const PaneCommand& command) {
    if (!model.accepts(stamp)) return PaneCommandOutcome::Stale;
    if (command.valueless_by_exception()) return PaneCommandOutcome::InvalidCommand;
    return std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, PaneFocusCommand>)
            return model.focus(value.row) ? PaneCommandOutcome::Applied : PaneCommandOutcome::InvalidTarget;
        else if constexpr (std::is_same_v<T, PaneMarkCommand>)
            return model.set_mark(value.row, value.marked) ? PaneCommandOutcome::Applied : PaneCommandOutcome::InvalidTarget;
        else if constexpr (std::is_same_v<T, PaneMarkRangeCommand>) {
            const auto first = model.visible_index(value.first), last = model.visible_index(value.last);
            if (!first || !last) return PaneCommandOutcome::InvalidTarget;
            return model.mark_range(*first, *last, value.marked) ? PaneCommandOutcome::Applied : PaneCommandOutcome::InvalidTarget;
        } else if constexpr (std::is_same_v<T, PaneMarkVisibleCommand>) {
            model.mark_visible(value.marked); return PaneCommandOutcome::Applied;
        } else if constexpr (std::is_same_v<T, PaneInvertVisibleCommand>) {
            model.invert_visible_marks(); return PaneCommandOutcome::Applied;
        } else if constexpr (std::is_same_v<T, PaneClearMarksCommand>) {
            model.clear_marks(); return PaneCommandOutcome::Applied;
        } else if constexpr (std::is_same_v<T, PaneSetViewCommand>) {
            try { model.set_view(value.view); }
            catch (const std::invalid_argument&) { return PaneCommandOutcome::InvalidCommand; }
            return PaneCommandOutcome::Applied;
        } else {
            static_assert(std::is_same_v<T, void>, "unhandled pane command alternative");
        }
    }, command);
}
} // namespace ck::files
