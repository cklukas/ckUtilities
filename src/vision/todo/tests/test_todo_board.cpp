// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_board_view.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>

#include "cvision/core/text.hpp"
#include "cvision/testing/cktest.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/ui/application.hpp"
#include "cvision/ui/standard_roles.hpp"

namespace {

using namespace ckv;
using namespace ck::todo;

AuditStamp board_stamp() { return {IsoTimestamp{"2026-08-25T12:00:00Z"}, "board-test"}; }

TaskDraft board_draft(std::string title, std::string details = {}) {
    TaskDraft draft;
    draft.title = std::move(title);
    draft.details = std::move(details);
    return draft;
}

TodoWorkspace board_workspace(int tasks = 4, int extra_lanes = 0) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    for (int index = 0; index < tasks; ++index) {
        TaskDraft draft = board_draft("Task " + std::to_string(index + 1), "Details " + std::to_string(index + 1));
        if (index == 0) {
            draft.priority = Priority::High;
            draft.due_date = IsoDate{"2026-09-01"};
            draft.color = TodoColor::Red;
        }
        if (!workspace.add_task(LaneId{1}, std::move(draft), board_stamp())) return TodoWorkspace::empty();
    }
    for (int index = 0; index < extra_lanes; ++index) {
        if (!workspace.insert_lane(BoardId{1}, "Lane " + std::to_string(index + 4)))
            return TodoWorkspace::empty();
    }
    return workspace;
}

KeyEvent key(Key value, std::string text = {}) {
    return KeyEvent{KeyChord{value, Modifier::None, std::move(text)}};
}

std::string surface_row(const ckv::scene::Surface& surface, int row) {
    std::string result;
    for (int x = 0; x < surface.size().width; ++x) result += surface.at(Point{x, row}).grapheme();
    return result;
}

struct ThemeFixture {
    ckv::ui::RoleRegistry registry;
    ckv::ui::StandardRoles roles = ckv::ui::intern_standard_roles(registry);
    ckv::ui::Theme theme = ckv::ui::make_classic_theme(registry, roles);
};

}  // namespace

CK_TEST(todo_lane_keeps_stable_selection_across_model_refresh) {
    TodoWorkspace workspace = board_workspace();
    TodoLaneView lane;
    lane.set_lane(workspace, LaneId{1});
    lane.select_task(TaskId{3});
    CK_CHECK(lane.selected_task() == TaskId{3});
    CK_CHECK(workspace.edit_task(TaskId{1}, board_draft("Renamed"), board_stamp()));
    lane.set_lane(workspace, LaneId{1});
    CK_CHECK(lane.selected_task() == TaskId{3});
}

CK_TEST(todo_lane_keyboard_navigation_reports_stable_task_ids) {
    TodoLaneView lane;
    lane.set_bounds(Rect{0, 0, 26, 8});
    lane.set_lane(board_workspace(), LaneId{1});
    TaskId selected;
    lane.on_selection_changed = [&](TaskId task_id) { selected = task_id; };
    CK_CHECK(lane.on_key(key(Key::Down)));
    CK_CHECK(lane.selected_task() == TaskId{2});
    CK_CHECK(selected == TaskId{2});
    CK_CHECK(lane.on_key(key(Key::End)));
    CK_CHECK(lane.selected_task() == TaskId{4});
    CK_CHECK(lane.on_key(key(Key::Home)));
    CK_CHECK(lane.selected_task() == TaskId{1});
}

CK_TEST(todo_lane_enter_and_space_submit_the_same_move_intent) {
    TodoLaneView lane;
    lane.set_lane(board_workspace(), LaneId{1});
    int toggles = 0;
    lane.on_move_toggle = [&](TaskId task_id) {
        CK_CHECK(task_id == TaskId{1});
        ++toggles;
    };
    CK_CHECK(lane.on_key(key(Key::Enter)));
    CK_CHECK(lane.on_key(key(Key::Char, " ")));
    CK_CHECK(toggles == 2);
}

CK_TEST(todo_lane_double_click_activates_the_pointed_card) {
    TodoLaneView lane;
    lane.set_bounds(Rect{5, 4, 26, 8});
    lane.set_lane(board_workspace(), LaneId{1});
    std::optional<TaskId> activated;
    lane.on_activate = [&](TaskId task_id) { activated = task_id; };
    // The second press of a double click (MouseEvent::click_count, which
    // Application counts on its clock) opens the card.
    MouseEvent event{MouseAction::Down, MouseButton::Left, Point{8, 7}, std::nullopt, Modifier::None};
    CK_CHECK(lane.on_mouse(event));
    CK_CHECK(lane.on_mouse(MouseEvent{MouseAction::Up, MouseButton::Left, Point{8, 7}, std::nullopt, Modifier::None}));
    CK_CHECK(!activated.has_value());
    event.click_count = 2;
    CK_CHECK(lane.on_mouse(event));
    CK_CHECK(activated == TaskId{2});
    CK_CHECK(lane.selected_task() == TaskId{2});
}

CK_TEST(todo_lane_draws_counted_frame_and_two_row_cards) {
    ThemeFixture fixture;
    TodoLaneView lane;
    lane.set_context(ckv::ui::Context{&fixture.theme, &fixture.registry, nullptr});
    lane.set_bounds(Rect{0, 0, 30, 8});
    lane.set_lane(board_workspace(), LaneId{1});
    ckv::scene::Surface surface(Size{30, 8}, Cell::from_grapheme(" ", Style{}));
    ckv::scene::Painter painter(surface, Rect{0, 0, 30, 8});
    lane.draw(painter);
    CK_CHECK(surface_row(surface, 0).find("To Do (4)") != std::string::npos);
    CK_CHECK(surface_row(surface, 1).find("↑") != std::string::npos);
    CK_CHECK(surface_row(surface, 1).find("Task 1") != std::string::npos);
    CK_CHECK(surface_row(surface, 1).find("2026-09-01") != std::string::npos);
    CK_CHECK(surface_row(surface, 2).find("Details 1") != std::string::npos);
}

CK_TEST(todo_lane_derives_overdue_today_tomorrow_and_future_badges_from_injected_date) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    const std::array<std::pair<std::string, std::string>, 4> dates{{
        {"Overdue", "2026-08-24"}, {"Today", "2026-08-25"},
        {"Tomorrow", "2026-08-26"}, {"Future", "2026-09-01"},
    }};
    for (std::size_t index = 0; index < dates.size(); ++index) {
        TaskDraft draft = board_draft(dates[index].first);
        draft.priority = static_cast<Priority>(index + 1);
        draft.due_date = IsoDate{dates[index].second};
        if (index == 1) draft.due_time = IsoTime{"09:15"};
        CK_CHECK(workspace.add_task(LaneId{1}, std::move(draft), board_stamp()));
    }

    ThemeFixture fixture;
    TodoLaneView lane;
    lane.set_context(ckv::ui::Context{&fixture.theme, &fixture.registry, nullptr});
    lane.set_bounds(Rect{0, 0, 40, 10});
    lane.set_lane(workspace, LaneId{1});
    lane.set_today(IsoDate{"2026-08-25"});
    ckv::scene::Surface surface(Size{40, 10}, Cell::from_grapheme(" ", Style{}));
    ckv::scene::Painter painter(surface, Rect{0, 0, 40, 10});
    lane.draw(painter);

    CK_CHECK(surface_row(surface, 1).find("↑ Overdue") != std::string::npos);
    CK_CHECK(surface_row(surface, 1).find("! overdue") != std::string::npos);
    CK_CHECK(surface_row(surface, 3).find("• Today") != std::string::npos);
    CK_CHECK(surface_row(surface, 3).find("! today 09:15") != std::string::npos);
    CK_CHECK(surface_row(surface, 5).find("↓ Tomorrow") != std::string::npos);
    CK_CHECK(surface_row(surface, 5).find("tomorrow") != std::string::npos);
    CK_CHECK(surface_row(surface, 7).find("⌛ Future") != std::string::npos);
    CK_CHECK(surface_row(surface, 7).find("2026-09-01") != std::string::npos);
}

CK_TEST(todo_lane_move_source_has_a_shape_not_only_a_color) {
    ThemeFixture fixture;
    TodoLaneView lane;
    lane.set_context(ckv::ui::Context{&fixture.theme, &fixture.registry, nullptr});
    lane.set_bounds(Rect{0, 0, 26, 8});
    lane.set_lane(board_workspace(), LaneId{1});
    lane.set_move_source(TaskId{1});
    ckv::scene::Surface surface(Size{26, 8}, Cell::from_grapheme(" ", Style{}));
    ckv::scene::Painter painter(surface, Rect{0, 0, 26, 8});
    lane.draw(painter);
    CK_CHECK(has_attr(surface.at(Point{3, 1}).style().attrs, Attr::Underline));
    CK_CHECK(has_attr(surface.at(Point{3, 1}).style().attrs, Attr::Bold));
}

CK_TEST(todo_board_materializes_every_lane_and_preserves_active_lane) {
    TodoWorkspace workspace = board_workspace(2, 2);
    TodoBoardView board;
    board.set_bounds(Rect{0, 0, 80, 16});
    CK_CHECK(board.set_board(workspace, BoardId{1}));
    CK_CHECK(board.children().size() == 1U);
    auto* viewport = dynamic_cast<ckv::widgets::ScrollViewport*>(board.children().front().get());
    CK_CHECK(viewport != nullptr);
    auto* row = viewport != nullptr ? dynamic_cast<ckv::ui::Row*>(viewport->content()) : nullptr;
    CK_CHECK(row != nullptr);
    CK_CHECK(board.lane_count() == 5);
    CK_CHECK(board.lane_view(LaneId{1})->parent() == row);
    CK_CHECK(board.active_lane() == LaneId{1});
    board.lane_view(LaneId{3})->on_focus(FocusEvent{true});
    CK_CHECK(board.active_lane() == LaneId{3});
    CK_CHECK(board.set_board(workspace, BoardId{1}));
    CK_CHECK(board.active_lane() == LaneId{3});
}

CK_TEST(todo_board_left_right_navigation_moves_focus_and_reveals_lanes) {
    ckv::term::HeadlessTerminal terminal(Size{50, 16});
    ManualClock clock;
    ckv::ui::Application app(terminal, clock);
    app.theme() = ckv::ui::make_classic_theme(app.roles(), ckv::ui::intern_standard_roles(app.roles()));
    auto board = std::make_unique<TodoBoardView>();
    board->set_fills_root(false);
    TodoBoardView* observer = app.root().add(std::move(board));
    observer->set_bounds(Rect{0, 0, 50, 16});
    CK_CHECK(observer->set_board(board_workspace(0, 4), BoardId{1}));
    app.set_focus(observer->lane_view(LaneId{1}));
    for (int step = 0; step < 6; ++step) app.focused()->on_key(key(Key::Right));
    CK_CHECK(observer->active_lane() == LaneId{7});
    CK_CHECK(app.focused() == observer->lane_view(LaneId{7}));
    const Rect revealed = observer->lane_view(LaneId{7})->absolute_bounds();
    CK_CHECK(revealed.x >= 0);
    CK_CHECK(revealed.right() <= 50);
    CK_CHECK(app.focused()->on_key(key(Key::Right)));
    CK_CHECK(observer->active_lane() == LaneId{1});
    CK_CHECK(app.focused()->on_key(key(Key::Left)));
    CK_CHECK(observer->active_lane() == LaneId{7});
    CK_CHECK(app.focused()->on_key(key(Key::Tab)));
    CK_CHECK(observer->active_lane() == LaneId{1});
    CK_CHECK(app.focused()->on_key(
        KeyEvent{KeyChord{Key::Tab, Modifier::Shift, {}}}));
    CK_CHECK(observer->active_lane() == LaneId{7});
}

CK_TEST(todo_board_keyboard_move_tracks_explicit_insertion_slots) {
    ckv::term::HeadlessTerminal terminal(Size{80, 16});
    ManualClock clock;
    ckv::ui::Application app(terminal, clock);
    app.theme() = ckv::ui::make_classic_theme(app.roles(), ckv::ui::intern_standard_roles(app.roles()));
    auto board = std::make_unique<TodoBoardView>();
    TodoBoardView* observer = app.root().add(std::move(board));
    observer->set_bounds(Rect{0, 0, 80, 16});
    CK_CHECK(observer->set_board(board_workspace(), BoardId{1}));
    CK_CHECK(observer->select_task(TaskId{2}));
    observer->begin_keyboard_move(TaskId{2}, LaneId{1}, TaskId{3});
    CK_CHECK(observer->keyboard_move_before() == TaskId{3});
    CK_CHECK(app.focused()->on_key(key(Key::Down)));
    CK_CHECK(observer->keyboard_move_before() == TaskId{4});
    CK_CHECK(app.focused()->on_key(key(Key::Up)));
    CK_CHECK(observer->keyboard_move_before() == TaskId{3});
    CK_CHECK(app.focused()->on_key(key(Key::Home)));
    CK_CHECK(observer->keyboard_move_before() == TaskId{1});
    CK_CHECK(app.focused()->on_key(key(Key::End)));
    CK_CHECK(!observer->keyboard_move_before());
    observer->end_keyboard_move();
}

CK_TEST(todo_board_rejects_unknown_board_without_losing_current_content) {
    TodoBoardView board;
    TodoWorkspace workspace = board_workspace();
    CK_CHECK(board.set_board(workspace, BoardId{1}));
    CK_CHECK(!board.set_board(workspace, BoardId{99}));
    CK_CHECK(board.board_id() == BoardId{1});
    CK_CHECK(board.lane_count() == 3);
}

CK_TEST(todo_board_pointer_drag_reports_stable_drop_ids_and_clears_feedback) {
    ckv::term::HeadlessTerminal terminal(Size{80, 16});
    ManualClock clock;
    ckv::ui::Application app(terminal, clock);
    app.theme() = ckv::ui::make_classic_theme(app.roles(), ckv::ui::intern_standard_roles(app.roles()));
    auto board = std::make_unique<TodoBoardView>();
    TodoBoardView* observer = app.root().add(std::move(board));
    observer->set_bounds(Rect{0, 0, 80, 16});
    CK_CHECK(observer->set_board(board_workspace(1), BoardId{1}));
    std::optional<TaskId> dropped_task;
    std::optional<LaneId> dropped_lane;
    std::optional<TaskId> dropped_before;
    observer->on_task_drop = [&](TaskId task_id, LaneId lane_id, std::optional<TaskId> before) {
        dropped_task = task_id;
        dropped_lane = lane_id;
        dropped_before = before;
    };

    CK_CHECK(app.dispatch(
        MouseEvent{MouseAction::Down, MouseButton::Left, Point{3, 1}, std::nullopt, Modifier::None}));
    CK_CHECK(app.dispatch(
        MouseEvent{MouseAction::Move, MouseButton::None, Point{30, 1}, std::nullopt, Modifier::None}));
    CK_CHECK(observer->lane_view(LaneId{2})->move_target());
    CK_CHECK(app.dispatch(
        MouseEvent{MouseAction::Up, MouseButton::Left, Point{30, 1}, std::nullopt, Modifier::None}));
    CK_CHECK(dropped_task == TaskId{1});
    CK_CHECK(dropped_lane == LaneId{2});
    CK_CHECK(!dropped_before.has_value());
    CK_CHECK(!observer->lane_view(LaneId{2})->move_target());
}

CK_TEST(todo_lane_right_click_distinguishes_lane_header_and_task_card) {
    TodoLaneView lane;
    lane.set_bounds(Rect{5, 4, 26, 8});
    lane.set_lane(board_workspace(1), LaneId{1});
    std::optional<LaneId> lane_menu;
    std::optional<TaskId> task_menu;
    lane.on_lane_context_menu = [&](LaneId lane_id, Point) { lane_menu = lane_id; };
    lane.on_task_context_menu = [&](TaskId task_id, Point) { task_menu = task_id; };
    CK_CHECK(lane.on_mouse(
        MouseEvent{MouseAction::Down, MouseButton::Right, Point{8, 4}, std::nullopt, Modifier::None}));
    CK_CHECK(lane_menu == LaneId{1});
    CK_CHECK(lane.on_mouse(
        MouseEvent{MouseAction::Down, MouseButton::Right, Point{8, 5}, std::nullopt, Modifier::None}));
    CK_CHECK(task_menu == TaskId{1});
}
