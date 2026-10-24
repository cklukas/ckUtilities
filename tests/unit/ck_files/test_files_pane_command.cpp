// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_command.hpp"
#include "pane_page.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <stdexcept>

namespace {
using namespace ck::files;
PaneEntry entry(std::string name, bool hidden = false) {
    return {*NativeName::from_posix(std::move(name)), {}, Observation<bool>::available(hidden)};
}
void load(PaneModel& pane, std::vector<PaneEntry> entries) {
    if (pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), entries) != PanePublish::Applied)
        throw std::runtime_error("command fixture publication failed");
}
PaneRowId id(const PaneModel& pane, std::size_t position) {
    const auto* row = pane.visible_row(position);
    if (!row) throw std::runtime_error("missing command fixture row");
    return row->id;
}
void check_counts(const PaneModel& pane, const std::vector<PaneRowId>& ids) {
    std::size_t marked = 0, visible = 0;
    for (const auto& row : ids) if (pane.row(row) && pane.marked(row)) {
        ++marked; if (pane.visible_index(row)) ++visible;
    }
    const auto summary = pane.selection_summary();
    CK_CHECK(summary.marks.total == marked && summary.marks.visible == visible && summary.marks.hidden == marked - visible);
    const auto sources = pane.sources();
    CK_CHECK(summary.state == sources.state && summary.origin == sources.origin &&
        summary.visible_count == sources.visible_count && summary.hidden_count == sources.hidden_count);
    CK_CHECK(sources.candidates.size() == summary.visible_count + summary.hidden_count);
    if (summary.state == PaneSourcesState::ListingPending || summary.state == PaneSourcesState::SelectionInvalidated) {
        CK_CHECK(summary.visible_count == 0 && summary.hidden_count == 0); return;
    }
    if (marked) {
        CK_CHECK(summary.origin == PaneSourcesOrigin::Marks && summary.visible_count == visible && summary.hidden_count == marked - visible);
        for (const auto& candidate : sources.candidates) CK_CHECK(pane.marked(candidate.row));
    } else {
        CK_CHECK(summary.origin == PaneSourcesOrigin::Cursor && summary.hidden_count == 0);
        CK_CHECK(summary.visible_count == (pane.focused() && pane.visible_index(*pane.focused()) ? 1 : 0));
    }
}
}
CK_TEST(pane_commands_reject_every_stale_or_foreign_command_before_any_change) {
    PaneModel pane, other; load(pane, {entry("a"), entry("b"), entry("c")});
    const auto a = id(pane, 0), c = id(pane, 2); const auto old = pane.state_stamp();
    CK_CHECK(apply_pane_command(pane, old, PaneMarkCommand{a, true}) == PaneCommandOutcome::Applied);
    const auto current = pane.state_stamp(), foreign = other.state_stamp();
    auto view = pane.view(); view.descending = true;
    const std::vector<PaneCommand> commands{PaneFocusCommand{c}, PaneMarkCommand{c, true}, PaneMarkRangeCommand{a, c, true},
        PaneMarkVisibleCommand{true}, PaneInvertVisibleCommand{}, PaneClearMarksCommand{}, PaneSetViewCommand{view}};
    for (const auto& command : commands) for (const auto& rejected : {old, foreign, PaneStateStamp{}}) {
        CK_CHECK(apply_pane_command(pane, rejected, command) == PaneCommandOutcome::Stale);
        CK_CHECK(pane.accepts(current) && pane.focused() == a && pane.marked(a) && !pane.marked(c) && !pane.view().descending);
    }
}
CK_TEST(pane_commands_apply_current_row_ids_and_reject_queued_reuse_of_the_old_page) {
    PaneModel pane; load(pane, {entry("a"), entry("b"), entry("c")});
    const auto a = id(pane, 0), b = id(pane, 1), c = id(pane, 2);
    auto stamp = pane.state_stamp();
    CK_CHECK(apply_pane_command(pane, stamp, PaneFocusCommand{b}) == PaneCommandOutcome::Applied && pane.focused() == b);
    CK_CHECK(apply_pane_command(pane, stamp, PaneMarkCommand{c, true}) == PaneCommandOutcome::Stale && !pane.marked(c));
    stamp = pane.state_stamp();
    CK_CHECK(apply_pane_command(pane, stamp, PaneMarkRangeCommand{c, a, true}) == PaneCommandOutcome::Applied);
    CK_CHECK(pane.marked(a) && pane.marked(b) && pane.marked(c) && !stamp.current());
    CK_CHECK(apply_pane_command(pane, pane.state_stamp(), PaneInvertVisibleCommand{}) == PaneCommandOutcome::Applied);
    CK_CHECK(pane.selection_summary().marks.total == 0);
    CK_CHECK(apply_pane_command(pane, pane.state_stamp(), PaneMarkVisibleCommand{true}) == PaneCommandOutcome::Applied);
    CK_CHECK(pane.selection_summary().marks.total == 3);
    CK_CHECK(apply_pane_command(pane, pane.state_stamp(), PaneClearMarksCommand{}) == PaneCommandOutcome::Applied);
    CK_CHECK(pane.selection_summary().marks.total == 0);
}
CK_TEST(pane_command_ranges_never_retarget_after_reorder_or_filtering) {
    PaneModel pane; load(pane, {entry("a"), entry("b"), entry("c")});
    const auto a = id(pane, 0), b = id(pane, 1), c = id(pane, 2); const auto old = pane.state_stamp();
    auto view = pane.view(); view.descending = true;
    CK_CHECK(apply_pane_command(pane, old, PaneSetViewCommand{view}) == PaneCommandOutcome::Applied);
    CK_CHECK(apply_pane_command(pane, old, PaneMarkRangeCommand{a, b, true}) == PaneCommandOutcome::Stale);
    CK_CHECK(apply_pane_command(pane, pane.state_stamp(), PaneMarkRangeCommand{a, b, true}) == PaneCommandOutcome::Applied);
    CK_CHECK(pane.marked(a) && pane.marked(b) && !pane.marked(c));
    view.display_substring = "b"; pane.set_view(view);
    const auto stamp = pane.state_stamp();
    CK_CHECK(apply_pane_command(pane, stamp, PaneMarkRangeCommand{b, a, false}) == PaneCommandOutcome::InvalidTarget);
    CK_CHECK(pane.accepts(stamp) && pane.marked(a) && pane.marked(b));
    CK_CHECK(pane.selection_summary().state == PaneSourcesState::HiddenMarksNeedReview);
}
CK_TEST(pane_commands_reject_invalid_targets_and_views_without_revoking_the_current_page) {
    PaneModel pane, other; load(pane, {entry("a"), entry("hidden", true)}); load(other, {entry("a")});
    const auto visible = id(pane, 0); auto view = pane.view(); view.show_hidden = true; pane.set_view(view);
    const auto hidden = id(pane, 1); view.show_hidden = false; pane.set_view(view);
    const auto stamp = pane.state_stamp();
    for (const auto& bad : {hidden, id(other, 0), PaneRowId{}}) {
        CK_CHECK(apply_pane_command(pane, stamp, PaneFocusCommand{bad}) == PaneCommandOutcome::InvalidTarget);
        CK_CHECK(apply_pane_command(pane, stamp, PaneMarkCommand{bad, true}) == PaneCommandOutcome::InvalidTarget);
        CK_CHECK(apply_pane_command(pane, stamp, PaneMarkRangeCommand{visible, bad, true}) == PaneCommandOutcome::InvalidTarget);
    }
    for (int mode = 0; mode < 2; ++mode) {
        auto invalid = pane.view(); if (mode == 0) invalid.sort = static_cast<PaneSortKey>(999); else invalid.display_substring.resize(4097);
        CK_CHECK(apply_pane_command(pane, stamp, PaneSetViewCommand{std::move(invalid)}) == PaneCommandOutcome::InvalidCommand);
    }
    CK_CHECK(pane.accepts(stamp) && pane.focused() == visible && pane.selection_summary().marks.total == 0);
}
CK_TEST(pane_selection_counts_follow_mixed_mark_and_projection_changes_without_rescanning_for_summary) {
    PaneModel pane; std::vector<PaneEntry> entries;
    for (int i = 0; i < 32; ++i) entries.push_back(entry("row" + std::to_string(i), i % 4 == 0));
    load(pane, std::move(entries)); auto view = pane.view(); view.show_hidden = true; pane.set_view(view);
    std::vector<PaneRowId> ids; for (std::size_t i = 0; i < pane.size(); ++i) ids.push_back(id(pane, i));
    std::uint32_t seed = 1234567;
    for (int step = 0; step < 1200; ++step) {
        seed = seed * 1664525U + 1013904223U;
        const auto visible = pane.visible_size();
        switch (seed % 8) {
        case 0: if (visible) pane.set_mark(id(pane, seed % visible), (seed & 256U) != 0); break;
        case 1: pane.mark_visible((seed & 256U) != 0); break;
        case 2: pane.invert_visible_marks(); break;
        case 3: if (visible) pane.mark_range(seed % visible, (seed >> 8U) % visible, (seed & 512U) != 0); break;
        case 4: pane.clear_marks(); break;
        case 5: view = pane.view(); view.show_hidden = !view.show_hidden; pane.set_view(view); break;
        case 6: view = pane.view(); view.descending = !view.descending;
            view.display_substring = (seed & 256U) ? "1" : ""; pane.set_view(view); break;
        case 7: if (visible) pane.focus(id(pane, seed % visible)); break;
        }
        check_counts(pane, ids);
    }
}
CK_TEST(pane_selection_summary_exposes_hidden_marks_in_pages_and_during_pending_and_failed_scans) {
    PaneModel pane; load(pane, {entry("a"), entry("b")}); const auto a = id(pane, 0), b = id(pane, 1);
    pane.mark_visible(true); auto view = pane.view(); view.display_substring = "b"; pane.set_view(view);
    const auto old = capture_pane_page(pane, 0).page; CK_CHECK(old);
    CK_CHECK(old->selection().state == PaneSourcesState::HiddenMarksNeedReview && old->selection().marks.total == 2 &&
        old->selection().visible_count == 1 && old->selection().hidden_count == 1);
    const auto request = pane.begin_listing(PaneListingPurpose::Refresh);
    check_counts(pane, {a, b}); const auto pending = capture_pane_page(pane, 0).page;
    CK_CHECK(pending->selection().state == PaneSourcesState::ListingPending && pending->selection().marks.hidden == 1);
    CK_CHECK(pane.fail_listing(request, {ObservationState::Denied, {}})); check_counts(pane, {a, b});
    CK_CHECK(pane.selection_summary().state == PaneSourcesState::HiddenMarksNeedReview);
    pane.clear_marks(); check_counts(pane, {a, b});
    CK_CHECK(old->selection().marks.total == 2 && !old->stamp().current());
    view.display_substring = "absent"; pane.set_view(view); check_counts(pane, {a, b});
    CK_CHECK(pane.selection_summary().state == PaneSourcesState::Empty);
}
CK_TEST(pane_selection_counts_survive_only_qualified_refresh_and_reset_on_navigation) {
    ck::testing::SimulatedFileSystem fs;
    const auto root = fs.make_node(ObjectKind::Directory), file = fs.make_node(ObjectKind::RegularFile);
#ifdef _WIN32
    const std::filesystem::path path = L"V:/simulated-pane-command";
#else
    const std::filesystem::path path = "/simulated-pane-command";
#endif
    const auto name = *NativeName::from_posix("held"); fs.bind(path, root); fs.put(root, name, file);
    auto opened = fs.open_directory(path); CK_CHECK(opened.value());
    auto parent = std::shared_ptr<const DirectoryCursor>(std::move(opened).take_value());
    auto observed = HeldChild::observe(parent, name); CK_CHECK(observed.value());
    auto held = std::move(observed).take_value();
    PaneModel pane; load(pane, {PaneEntry::from_held(held, {}), entry("unheld")});
    const auto old = id(pane, 0), lost = id(pane, 1); pane.mark_visible(true);
    auto fresh = held->refresh(); CK_CHECK(fresh.value());
    const auto request = pane.begin_listing(PaneListingPurpose::Refresh);
    CK_CHECK(pane.publish(request, {PaneEntry::from_held(std::move(fresh).take_value(), {}), entry("unheld")}) == PanePublish::Applied);
    check_counts(pane, {old, lost});
    CK_CHECK(pane.selection_summary().state == PaneSourcesState::SelectionInvalidated && pane.selection_summary().marks.total == 1);
    CK_CHECK(pane.invalidated_selection().marks == 1 && pane.marked(old));
    load(pane, {entry("new")}); check_counts(pane, {id(pane, 0)});
    CK_CHECK(pane.selection_summary().marks.total == 0 && pane.selection_summary().visible_count == 1);
}
