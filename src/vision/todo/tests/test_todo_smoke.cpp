// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_app.hpp"
#include "memory_todo_repository.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "cvision/testing/cktest.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/widgets/common_components.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/status_line.hpp"
#include "cvision/widgets/text_editor.hpp"
#include "cvision/widgets/window.hpp"

namespace {

using namespace ckv;
using namespace ck::todo;

CalendarReading smoke_reading() {
    return {IsoTimestamp{"2026-08-25T12:00:00Z"}, IsoDate{"2026-08-25"}, IsoTime{"14:30"}};
}

TodoWorkspace smoke_workspace() {
    return *TodoWorkspace::guided({IsoTimestamp{"2026-08-25T11:00:00Z"}, "fixture"}).value;
}

TodoWorkspace movement_workspace(bool sorted_target = false, bool empty_target = false) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    for (const char* title : {"First", "Second", "Third"}) {
        TaskDraft draft;
        draft.title = title;
        if (!workspace.add_task(LaneId{1}, std::move(draft),
                                {IsoTimestamp{"2026-08-25T11:00:00Z"}, "fixture"}))
            return TodoWorkspace::empty();
    }
    if (!empty_target) {
        TaskDraft lower;
        lower.title = "Lower";
        lower.priority = Priority::Low;
        if (!workspace.add_task(LaneId{2}, std::move(lower),
                                {IsoTimestamp{"2026-08-25T11:00:00Z"}, "fixture"}))
            return TodoWorkspace::empty();
        TaskDraft higher;
        higher.title = "Higher";
        higher.priority = Priority::High;
        if (!workspace.add_task(LaneId{2}, std::move(higher),
                                {IsoTimestamp{"2026-08-25T11:00:00Z"}, "fixture"}))
            return TodoWorkspace::empty();
    }
    if (sorted_target && !workspace.set_lane_sort(LaneId{2}, SortMode::Priority))
        return TodoWorkspace::empty();
    return workspace;
}

TodoWorkspace one_lane_workspace() {
    TodoWorkspace workspace = smoke_workspace();
    if (!workspace.merge_lane(LaneId{2}, LaneId{1})) return TodoWorkspace::empty();
    if (!workspace.merge_lane(LaneId{3}, LaneId{1})) return TodoWorkspace::empty();
    return workspace;
}

TodoWorkspace two_board_workspace() {
    TodoWorkspace workspace = smoke_workspace();
    if (!workspace.add_board("Release")) return TodoWorkspace::empty();
    if (!workspace.switch_board(BoardId{1})) return TodoWorkspace::empty();
    return workspace;
}

TaskDraft draft_from(const Task& task) {
    return TaskDraft{
        task.title, task.details, task.note, task.priority, task.due_date, task.due_time, task.color};
}

void commit_external_task(MemoryTodoRepository& repository, std::string title) {
    auto external = repository.load();
    CK_CHECK(external);
    if (!external) return;
    TaskDraft draft;
    draft.title = std::move(title);
    CK_CHECK(external.value->workspace.add_task(
        LaneId{2}, std::move(draft), {IsoTimestamp{"2026-08-25T12:01:00Z"}, "external"}));
    CK_CHECK(repository.commit(external.value->revision, external.value->workspace, IsoDate{"2026-08-25"}));
}

void commit_external_task_edit(MemoryTodoRepository& repository, TaskId task_id, std::string title) {
    auto external = repository.load();
    CK_CHECK(external);
    if (!external) return;
    const Task* task = external.value->workspace.find_task(task_id);
    CK_CHECK(task != nullptr);
    if (task == nullptr) return;
    TaskDraft draft = draft_from(*task);
    draft.title = std::move(title);
    CK_CHECK(external.value->workspace.edit_task(
        task_id, std::move(draft), {IsoTimestamp{"2026-08-25T12:01:00Z"}, "external"}));
    CK_CHECK(repository.commit(external.value->revision, external.value->workspace, IsoDate{"2026-08-25"}));
}

void commit_external_note(MemoryTodoRepository& repository, TaskId task_id, std::string note,
                          std::string timestamp = "2026-08-25T12:01:00Z") {
    auto external = repository.load();
    CK_CHECK(external);
    if (!external) return;
    const Task* task = external.value->workspace.find_task(task_id);
    CK_CHECK(task != nullptr);
    if (task == nullptr) return;
    TaskDraft draft = draft_from(*task);
    draft.note = std::move(note);
    CK_CHECK(external.value->workspace.edit_task(
        task_id, std::move(draft), {IsoTimestamp{std::move(timestamp)}, "external"}));
    CK_CHECK(repository.commit(external.value->revision, external.value->workspace, IsoDate{"2026-08-25"}));
}

void commit_external_delete(MemoryTodoRepository& repository, TaskId task_id) {
    auto external = repository.load();
    CK_CHECK(external);
    if (!external) return;
    CK_CHECK(external.value->workspace.delete_task(task_id));
    CK_CHECK(repository.commit(external.value->revision, external.value->workspace, IsoDate{"2026-08-25"}));
}

KeyEvent key(Key value, std::string text = {}) {
    return KeyEvent{KeyChord{value, Modifier::None, std::move(text)}};
}

bool is_inside(Rect inner, Rect outer) {
    return inner.left() >= outer.left() && inner.top() >= outer.top() &&
           inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}

std::string frame_text(ckv::ui::Application& app) {
    app.step(0);
    const auto frame = app.current_frame();
    std::string text;
    for (int y = 0; y < frame.size().height; ++y)
        for (int x = 0; x < frame.size().width; ++x) text += frame.at(Point{x, y}).grapheme();
    return text;
}

struct SmokeFixture {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock monotonic;
    ckv::ui::Application app{terminal, monotonic};
    MemoryTodoRepository repository{smoke_workspace()};
    FixedCalendarClock calendar{smoke_reading()};
    TodoApp todo{app, repository, calendar, "smoke-test"};
};

const ckv::widgets::MenuBarItem* menu_named(const ckv::widgets::MenuBar& menu, const std::string& label) {
    for (const auto& item : menu.menus())
        if (item.label == label) return &item;
    return nullptr;
}

}  // namespace

CK_TEST(todo_app_builds_a_windowed_board_with_menu_status_and_focus) {
    SmokeFixture fixture;
    CK_CHECK(fixture.todo.controller().is_open());
    CK_CHECK(fixture.todo.board_window() != nullptr);
    CK_CHECK(fixture.todo.board_window()->movable());
    CK_CHECK(fixture.todo.board_window()->resizable());
    CK_CHECK(fixture.todo.board_view()->lane_count() == 3);
    CK_CHECK(fixture.todo.status_line() != nullptr);
    bool found_board = false;
    bool found_calendar = false;
    for (const ckv::widgets::StatusLineItem& item : fixture.todo.status_line()->items()) {
        if (item.label == "Board: main" &&
            item.command == fixture.app.commands().id_for(TodoApp::kBoardManagerKey).value_or(ckv::ui::kInvalidCommand))
            found_board = true;
        if (item.label == "2026-08-25 12:00Z") found_calendar = true;
    }
    CK_CHECK(found_board);
    CK_CHECK(found_calendar);
    CK_CHECK(fixture.app.focused() == fixture.todo.board_view()->lane_view(LaneId{1}));
    CK_CHECK(fixture.app.commands().id_for(TodoApp::kAddTaskKey).has_value());
    CK_CHECK(fixture.app.commands().command_for_key(KeyChord{Key::F2, Modifier::None, {}}) ==
             fixture.app.commands().id_for(TodoApp::kAddTaskKey));
}

CK_TEST(todo_board_only_aliases_do_not_run_while_a_task_dialog_has_focus) {
    SmokeFixture fixture;
    const auto edit = fixture.app.commands().id_for(TodoApp::kEditTaskKey);
    CK_CHECK(edit && fixture.app.execute_command(*edit));
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(fixture.app.dispatch(key(Key::Char, "e")));
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(!fixture.app.is_modal());
}

CK_TEST(todo_task_dialog_uses_typed_date_and_time_pickers_and_round_trips_both) {
    SmokeFixture fixture;
    const auto edit = fixture.app.commands().id_for(TodoApp::kEditTaskKey);
    CK_CHECK(edit && fixture.app.execute_command(*edit));
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(fixture.app.dispatch(key(Key::Tab)));
    CK_CHECK(fixture.app.dispatch(key(Key::Tab)));
    auto* due = dynamic_cast<ckv::widgets::DatePicker*>(fixture.app.focused());
    CK_CHECK(due != nullptr);
    if (due == nullptr) return;
    CK_CHECK(!due->value());
    CK_CHECK(fixture.app.dispatch(key(Key::Char, " ")));
    CK_CHECK(dynamic_cast<ckv::widgets::CalendarDropdown*>(fixture.app.input_capture()) != nullptr);
    CK_CHECK(fixture.app.dispatch(key(Key::Right)));
    CK_CHECK((due->value() ==
              std::optional<ckv::widgets::DateValue>{ckv::widgets::DateValue{2026, 8, 26}}));
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
    CK_CHECK(fixture.app.dispatch(key(Key::Tab)));
    CK_CHECK(fixture.app.dispatch(key(Key::Char, " ")));
    CK_CHECK(fixture.app.dispatch(key(Key::Tab)));
    auto* due_time = dynamic_cast<ckv::widgets::TimePicker*>(fixture.app.focused());
    CK_CHECK(due_time != nullptr);
    if (due_time == nullptr) return;
    CK_CHECK((due_time->value() == ckv::widgets::TimeValue{14, 30, 0}));
    CK_CHECK(fixture.app.dispatch(key(Key::Up)));
    CK_CHECK(fixture.app.dispatch(key(Key::Right)));
    CK_CHECK(fixture.app.dispatch(key(Key::Up)));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    const Task* task = fixture.todo.controller().workspace()->find_task(TaskId{1});
    CK_CHECK(task != nullptr);
    if (task != nullptr) {
        CK_CHECK(task->due_date == std::optional<IsoDate>{IsoDate{"2026-08-26"}});
        CK_CHECK(task->due_time == std::optional<IsoTime>{IsoTime{"15:31"}});
    }
}

CK_TEST(todo_move_mode_commits_across_lanes_and_escape_cancels) {
    SmokeFixture fixture;
    const auto move = fixture.app.commands().id_for(TodoApp::kMoveTaskKey);
    CK_CHECK(move && fixture.app.execute_command(*move));
    CK_CHECK(fixture.todo.move_active());
    CK_CHECK(fixture.app.dispatch(key(Key::Right)));
    CK_CHECK(fixture.app.execute_command(*move));
    CK_CHECK(!fixture.todo.move_active());
    CK_CHECK(fixture.todo.controller().workspace()->lane_of(TaskId{1}) == LaneId{2});

    fixture.todo.board_view()->select_task(TaskId{1});
    CK_CHECK(fixture.app.execute_command(*move));
    CK_CHECK(fixture.app.dispatch(key(Key::Right)));
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
    CK_CHECK(!fixture.todo.move_active());
    CK_CHECK(fixture.todo.controller().workspace()->lane_of(TaskId{1}) == LaneId{2});
}

CK_TEST(todo_move_mode_moves_one_slot_down_with_one_down_key) {
    ckv::term::HeadlessTerminal terminal(Size{100, 30});
    ManualClock monotonic;
    ckv::ui::Application app(terminal, monotonic);
    MemoryTodoRepository repository(movement_workspace());
    FixedCalendarClock calendar(smoke_reading());
    TodoApp todo(app, repository, calendar, "move-within-lane");
    const auto move = app.commands().id_for(TodoApp::kMoveTaskKey);
    CK_CHECK(todo.board_view()->select_task(TaskId{2}));
    CK_CHECK(move && app.execute_command(*move));
    CK_CHECK(app.dispatch(key(Key::Down)));
    CK_CHECK(app.execute_command(*move));
    const Lane* lane = todo.controller().workspace()->find_lane(LaneId{1});
    CK_CHECK(lane != nullptr);
    if (lane != nullptr)
        CK_CHECK(lane->task_ids == std::vector<TaskId>({TaskId{1}, TaskId{3}, TaskId{2}}));
}

CK_TEST(todo_move_mode_accepts_sorted_and_empty_target_lanes) {
    {
        ckv::term::HeadlessTerminal terminal(Size{100, 30});
        ManualClock monotonic;
        ckv::ui::Application app(terminal, monotonic);
        MemoryTodoRepository repository(movement_workspace(true));
        FixedCalendarClock calendar(smoke_reading());
        TodoApp todo(app, repository, calendar, "move-to-sorted");
        const auto move = app.commands().id_for(TodoApp::kMoveTaskKey);
        CK_CHECK(move && app.execute_command(*move));
        CK_CHECK(app.dispatch(key(Key::Right)));
        CK_CHECK(app.dispatch(key(Key::Down)));
        CK_CHECK(app.execute_command(*move));
        CK_CHECK(!todo.move_active());
        CK_CHECK(todo.controller().workspace()->lane_of(TaskId{1}) == LaneId{2});
    }
    {
        ckv::term::HeadlessTerminal terminal(Size{100, 30});
        ManualClock monotonic;
        ckv::ui::Application app(terminal, monotonic);
        MemoryTodoRepository repository(movement_workspace(false, true));
        FixedCalendarClock calendar(smoke_reading());
        TodoApp todo(app, repository, calendar, "move-to-empty");
        const auto move = app.commands().id_for(TodoApp::kMoveTaskKey);
        CK_CHECK(move && app.execute_command(*move));
        CK_CHECK(app.dispatch(key(Key::Right)));
        CK_CHECK(app.dispatch(key(Key::Enter)));
        CK_CHECK(!todo.move_active());
        CK_CHECK(todo.controller().workspace()->lane_of(TaskId{1}) == LaneId{2});
    }
}

CK_TEST(todo_note_window_is_modeless_and_autosaves_after_150ms) {
    SmokeFixture fixture;
    CK_CHECK(fixture.todo.desktop().windows().size() == 1U);
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    CK_CHECK(fixture.todo.note_window_count() == 1);
    CK_CHECK(fixture.todo.desktop().windows().size() == 2U);
    CK_CHECK(!fixture.app.is_modal());
    CK_CHECK(fixture.app.dispatch(TextEvent{"autosaved note", false, false}));
    fixture.monotonic.advance(150'000'000);
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1})->note == "autosaved note");
    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().close));
    CK_CHECK(fixture.todo.note_window_count() == 0);
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.desktop().windows().size() == 1U);
    CK_CHECK(fixture.app.focused() == fixture.todo.board_view()->lane_view(LaneId{1}));
    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().next_window));
}

CK_TEST(todo_note_editor_accepts_shifted_character_input_through_application_routing) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));

    CK_CHECK(fixture.app.dispatch(KeyEvent{KeyChord{Key::Char, Modifier::Shift, "U"}}));
    CK_CHECK(fixture.app.dispatch(KeyEvent{KeyChord{Key::Char, Modifier::None, "ppercase"}}));
    auto* editor = dynamic_cast<ckv::widgets::TextEditor*>(fixture.app.focused());
    CK_CHECK(editor != nullptr);
    if (editor == nullptr) return;
    CK_CHECK(editor->document()->text() == "Uppercase");

    fixture.monotonic.advance(150'000'000);
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1})->note == "Uppercase");
}

CK_TEST(todo_closing_notes_detaches_each_window_and_activates_the_remaining_note) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    CK_CHECK(fixture.todo.board_view()->select_task(TaskId{2}));
    CK_CHECK(note && fixture.app.execute_command(*note));
    CK_CHECK(fixture.todo.note_window_count() == 2U);
    CK_CHECK(fixture.todo.desktop().windows().size() == 3U);

    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().close));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.note_window_count() == 1U);
    CK_CHECK(fixture.todo.desktop().windows().size() == 2U);
    CK_CHECK(dynamic_cast<ckv::widgets::TextEditor*>(fixture.app.focused()) != nullptr);

    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().close));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.note_window_count() == 0U);
    CK_CHECK(fixture.todo.desktop().windows().size() == 1U);
}

CK_TEST(todo_quit_flushes_an_open_note_before_requesting_exit) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    CK_CHECK(fixture.app.dispatch(TextEvent{"saved by quit", false, false}));
    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().quit));
    CK_CHECK(fixture.app.quit_requested());
    const auto stored = fixture.repository.load();
    CK_CHECK(stored && stored.value->workspace.find_task(TaskId{1})->note == "saved by quit");
}

CK_TEST(todo_board_close_uses_the_same_safe_quit_path_and_keeps_the_view_alive) {
    SmokeFixture fixture;
    ckv::widgets::Window* const board = fixture.todo.board_window();
    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().close));
    CK_CHECK(fixture.app.quit_requested());
    CK_CHECK(fixture.todo.board_window() == board);
    CK_CHECK(fixture.todo.board_view()->lane_count() == 3);
}

CK_TEST(todo_note_window_uses_the_revisioned_editor_with_undo_redo_and_wrap) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    auto* editor = dynamic_cast<ckv::widgets::TextEditor*>(fixture.app.focused());
    CK_CHECK(editor != nullptr);
    if (editor == nullptr) return;
    CK_CHECK(editor->wrap_mode() == ckv::widgets::WrapMode::Word);
    CK_CHECK(fixture.app.dispatch(TextEvent{"draft", false, false}));
    CK_CHECK(editor->document()->text() == "draft");

    const auto undo = fixture.app.commands().id_for("todo.note.undo");
    const auto redo = fixture.app.commands().id_for("todo.note.redo");
    const auto wrap = fixture.app.commands().id_for("todo.note.wrap");
    CK_CHECK(undo && redo && wrap);
    if (!undo || !redo || !wrap) return;
    CK_CHECK(fixture.app.execute_command(*undo));
    CK_CHECK(editor->document()->text().empty());
    CK_CHECK(fixture.app.execute_command(*redo));
    CK_CHECK(editor->document()->text() == "draft");
    CK_CHECK(fixture.app.execute_command(*wrap));
    CK_CHECK(editor->wrap_mode() == ckv::widgets::WrapMode::None);
}

CK_TEST(todo_clean_open_note_adopts_an_external_edit) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    auto* editor = dynamic_cast<ckv::widgets::TextEditor*>(fixture.app.focused());
    CK_CHECK(editor != nullptr);
    if (editor == nullptr) return;

    commit_external_note(fixture.repository, TaskId{1}, "external note");
    fixture.todo.poll_repository();
    CK_CHECK(editor->document()->text() == "external note");
    CK_CHECK(!editor->read_only());
}

CK_TEST(todo_dirty_open_note_requires_explicit_resolution_after_a_poll) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    auto* editor = dynamic_cast<ckv::widgets::TextEditor*>(fixture.app.focused());
    CK_CHECK(editor != nullptr);
    if (editor == nullptr) return;
    CK_CHECK(fixture.app.dispatch(TextEvent{"local note", false, false}));
    commit_external_note(fixture.repository, TaskId{1}, "external note", "2026-08-25T11:30:00Z");

    fixture.todo.poll_repository();
    CK_CHECK(editor->document()->text() == "local note");
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1})->note == "external note");
    fixture.monotonic.advance(150'000'000);
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.app.is_modal());
    const auto before_resolution = fixture.repository.load();
    CK_CHECK(before_resolution &&
             before_resolution.value->workspace.find_task(TaskId{1})->note == "external note");

    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(!fixture.app.is_modal());
    const auto after_resolution = fixture.repository.load();
    CK_CHECK(after_resolution &&
             after_resolution.value->workspace.find_task(TaskId{1})->note == "local note");
}

CK_TEST(todo_open_note_retains_local_text_when_the_task_is_removed_externally) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    auto* editor = dynamic_cast<ckv::widgets::TextEditor*>(fixture.app.focused());
    CK_CHECK(editor != nullptr);
    if (editor == nullptr) return;
    CK_CHECK(fixture.app.dispatch(TextEvent{"local unsaved text", false, false}));

    commit_external_delete(fixture.repository, TaskId{1});
    fixture.todo.poll_repository();
    CK_CHECK(editor->document()->text() == "local unsaved text");
    CK_CHECK(editor->read_only());
}

CK_TEST(todo_overlapping_note_edit_can_choose_the_external_text) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    auto* editor = dynamic_cast<ckv::widgets::TextEditor*>(fixture.app.focused());
    CK_CHECK(editor != nullptr);
    if (editor == nullptr) return;
    CK_CHECK(fixture.app.dispatch(TextEvent{"local note", false, false}));
    commit_external_note(fixture.repository, TaskId{1}, "external note");

    fixture.monotonic.advance(150'000'000);
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(editor->document()->text() == "local note");
    CK_CHECK(fixture.app.dispatch(key(Key::Down)));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(editor->document()->text() == "external note");
    CK_CHECK(!fixture.app.is_modal());
}

CK_TEST(todo_quit_pauses_for_an_overlapping_note_conflict) {
    SmokeFixture fixture;
    const auto note = fixture.app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && fixture.app.execute_command(*note));
    CK_CHECK(fixture.app.dispatch(TextEvent{"local note", false, false}));
    commit_external_note(fixture.repository, TaskId{1}, "external note");

    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().quit));
    CK_CHECK(!fixture.app.quit_requested());
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(fixture.app.dispatch(key(Key::Down)));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.app.execute_command(fixture.app.commands().standard().quit));
    CK_CHECK(fixture.app.quit_requested());
}

CK_TEST(todo_archive_confirmation_removes_only_after_yes) {
    SmokeFixture fixture;
    const auto archive = fixture.app.commands().id_for(TodoApp::kArchiveTaskKey);
    CK_CHECK(archive && fixture.app.execute_command(*archive));
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(!fixture.app.is_modal());
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1}) == nullptr);
    CK_CHECK(fixture.repository.archives().size() == 1);
}

CK_TEST(todo_theme_menu_is_radio_marked_and_rebuilds_after_selection) {
    SmokeFixture fixture;
    const ckv::widgets::MenuBarItem* view = menu_named(*fixture.todo.menu_bar(), "&View");
    CK_CHECK(view != nullptr);
    if (view == nullptr) return;
    const auto& choices = view->items.front().children();
    CK_CHECK(choices[0].mark() == ckv::widgets::MenuMark::RadioOn);
    const auto dark = fixture.app.commands().id_for(TodoApp::kThemeKeys[1]);
    CK_CHECK(dark && choices[1].command() == *dark);
    CK_CHECK(dark && fixture.app.execute_command(*dark));
    CK_CHECK(fixture.todo.theme() == TodoTheme::Dark);
}

CK_TEST(todo_task_delete_is_distinct_from_archive_and_requires_confirmation) {
    SmokeFixture fixture;
    const auto remove = fixture.app.commands().id_for(TodoApp::kDeleteTaskKey);
    CK_CHECK(remove && fixture.app.execute_command(*remove));
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1}) == nullptr);
    CK_CHECK(fixture.repository.archives().empty());
}

CK_TEST(todo_lane_commands_insert_sort_and_expose_checked_menu_state) {
    SmokeFixture fixture;
    const auto insert = fixture.app.commands().id_for("todo.insert-lane-right");
    CK_CHECK(insert && fixture.app.execute_command(*insert));
    CK_CHECK(fixture.app.dispatch(TextEvent{"Review", false, false}));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.board_view()->lane_count() == 4);
    const Lane* review = fixture.todo.controller().workspace()->find_lane(LaneId{4});
    CK_CHECK(review != nullptr);
    if (review == nullptr) return;
    CK_CHECK(review->title == "Review");

    const auto priority_sort = fixture.app.commands().id_for("todo.lane-sort.priority");
    CK_CHECK(priority_sort && fixture.app.execute_command(*priority_sort));
    CK_CHECK(fixture.todo.controller().workspace()->find_lane(LaneId{4})->sort == SortMode::Priority);
    const ckv::widgets::MenuBarItem* lane = menu_named(*fixture.todo.menu_bar(), "&Lane");
    CK_CHECK(lane != nullptr);
    if (lane != nullptr) CK_CHECK(lane->items.front().children()[5].mark() == ckv::widgets::MenuMark::RadioOn);
}

CK_TEST(todo_board_creation_selects_and_persists_the_new_board) {
    SmokeFixture fixture;
    const auto add = fixture.app.commands().id_for(TodoApp::kNewBoardKey);
    CK_CHECK(add && fixture.app.execute_command(*add));
    CK_CHECK(fixture.app.dispatch(TextEvent{"Release", false, false}));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    const TodoWorkspace* workspace = fixture.todo.controller().workspace();
    CK_CHECK(workspace->snapshot().boards.size() == 2);
    CK_CHECK(workspace->snapshot().last_board_id == BoardId{2});
    CK_CHECK(fixture.todo.board_view()->board_id() == BoardId{2});
    CK_CHECK(fixture.todo.board_window()->title() == "TODO — Release");
    bool found_board = false;
    for (const ckv::widgets::StatusLineItem& item : fixture.todo.status_line()->items())
        if (item.label == "Board: Release") found_board = true;
    CK_CHECK(found_board);
}

CK_TEST(todo_lane_and_board_manager_shortcuts_are_board_scoped) {
    SmokeFixture fixture;
    CK_CHECK(fixture.app.commands().command_for_key(KeyChord{Key::F7, Modifier::None, {}}) ==
             fixture.app.commands().id_for(TodoApp::kLaneActionsKey));
    CK_CHECK(fixture.app.commands().command_for_key(KeyChord{Key::Char, Modifier::None, "m"}) ==
             fixture.app.commands().id_for(TodoApp::kBoardManagerKey));
    const auto manager = fixture.app.commands().id_for(TodoApp::kBoardManagerKey);
    CK_CHECK(manager && fixture.app.execute_command(*manager));
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(!fixture.app.is_modal());

    CK_CHECK(fixture.app.dispatch(
        KeyEvent{KeyChord{Key::F10, Modifier::Shift, {}}, KeyAction::Press, false}));
    CK_CHECK(fixture.app.input_capture() != nullptr);
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
}

CK_TEST(todo_descriptor_dialogs_inherit_contextual_help_topics) {
    SmokeFixture fixture;
    for (const auto& [command_key, help_key] : {
             std::pair{TodoApp::kEditTaskKey, std::string_view{"todo.tasks"}},
             std::pair{TodoApp::kLaneActionsKey, std::string_view{"todo.lanes"}},
             std::pair{TodoApp::kBoardManagerKey, std::string_view{"todo.boards"}},
             std::pair{TodoApp::kNewBoardKey, std::string_view{"todo.boards"}}}) {
        const auto command = fixture.app.commands().id_for(command_key);
        CK_CHECK(command && fixture.app.execute_command(*command));
        CK_CHECK(fixture.app.focused() != nullptr);
        const std::string* context = fixture.app.focused() != nullptr
                                         ? fixture.app.focused()->resolve_help_context_key()
                                         : nullptr;
        CK_CHECK(context != nullptr);
        if (context != nullptr) CK_CHECK(*context == help_key);
        CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
        fixture.app.step(fixture.monotonic.now_nanos());
    }
}

CK_TEST(todo_action_dialogs_hide_operations_that_the_model_cannot_accept) {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock monotonic;
    ckv::ui::Application app{terminal, monotonic};
    MemoryTodoRepository repository{one_lane_workspace()};
    FixedCalendarClock calendar{smoke_reading()};
    TodoApp todo{app, repository, calendar, "one-lane"};

    const auto lane_actions = app.commands().id_for(TodoApp::kLaneActionsKey);
    CK_CHECK(lane_actions && app.execute_command(*lane_actions));
    std::string rendered = frame_text(app);
    CK_CHECK(rendered.find("Merge into another lane") == std::string::npos);
    CK_CHECK(rendered.find("Archive lane") == std::string::npos);
    CK_CHECK(app.dispatch(key(Key::Escape)));
    app.step(monotonic.now_nanos());

    const auto board_manager = app.commands().id_for(TodoApp::kBoardManagerKey);
    CK_CHECK(board_manager && app.execute_command(*board_manager));
    rendered = frame_text(app);
    CK_CHECK(rendered.find("Rename Board...") == std::string::npos);
    CK_CHECK(rendered.find("Merge Board into...") == std::string::npos);
    CK_CHECK(rendered.find("Archive Board...") == std::string::npos);
}

CK_TEST(todo_board_management_chooses_a_valid_non_main_source_explicitly) {
    ckv::term::HeadlessTerminal terminal{Size{100, 30}};
    ManualClock monotonic;
    ckv::ui::Application app{terminal, monotonic};
    MemoryTodoRepository repository{two_board_workspace()};
    FixedCalendarClock calendar{smoke_reading()};
    TodoApp todo{app, repository, calendar, "two-boards"};

    const auto manager = app.commands().id_for(TodoApp::kBoardManagerKey);
    CK_CHECK(manager && app.execute_command(*manager));
    CK_CHECK(app.dispatch(key(Key::Tab)));
    CK_CHECK(app.dispatch(key(Key::Down)));
    CK_CHECK(app.dispatch(key(Key::Down)));
    CK_CHECK(app.dispatch(key(Key::Enter)));
    app.step(monotonic.now_nanos());
    auto* source = dynamic_cast<ckv::widgets::ComboBox*>(app.focused());
    CK_CHECK(source != nullptr);
    if (source != nullptr) {
        CK_CHECK(source->items().size() == 1);
        CK_CHECK(source->text() == "Release");
    }
    CK_CHECK(todo.desktop().active_window()->title() == "Choose Board to rename");
}

CK_TEST(todo_initial_board_and_note_windows_fit_a_small_desktop) {
    ckv::term::HeadlessTerminal terminal{Size{24, 10}};
    ManualClock monotonic;
    ckv::ui::Application app{terminal, monotonic};
    MemoryTodoRepository repository{smoke_workspace()};
    FixedCalendarClock calendar{smoke_reading()};
    TodoApp todo{app, repository, calendar, "small-desktop"};
    const Rect area = todo.desktop().content_area();
    CK_CHECK(is_inside(todo.board_window()->bounds(), area));

    const auto note = app.commands().id_for(TodoApp::kEditNoteKey);
    CK_CHECK(note && app.execute_command(*note));
    CK_CHECK(todo.note_window_count() == 1);
    CK_CHECK(todo.desktop().active_window() != nullptr);
    if (todo.desktop().active_window() != nullptr)
        CK_CHECK(is_inside(todo.desktop().active_window()->bounds(), area));
}

CK_TEST(todo_pointer_drag_moves_through_the_same_durable_controller_path) {
    SmokeFixture fixture;
    const Rect source_lane = fixture.todo.board_view()->lane_view(LaneId{1})->absolute_bounds();
    const Rect target_lane = fixture.todo.board_view()->lane_view(LaneId{2})->absolute_bounds();
    const Point source{source_lane.x + 3, source_lane.y + 1};
    const Point target{target_lane.x + 3, target_lane.y + 1};
    CK_CHECK(fixture.app.dispatch(
        MouseEvent{MouseAction::Down, MouseButton::Left, source, std::nullopt, Modifier::None}));
    CK_CHECK(fixture.app.dispatch(
        MouseEvent{MouseAction::Move, MouseButton::None, target, std::nullopt, Modifier::None}));
    CK_CHECK(fixture.app.dispatch(
        MouseEvent{MouseAction::Up, MouseButton::Left, target, std::nullopt, Modifier::None}));
    CK_CHECK(fixture.todo.controller().workspace()->lane_of(TaskId{1}) == LaneId{2});
}

CK_TEST(todo_task_right_click_opens_command_context_menu_and_escape_dismisses_it) {
    SmokeFixture fixture;
    const Rect lane = fixture.todo.board_view()->lane_view(LaneId{1})->absolute_bounds();
    CK_CHECK(fixture.app.dispatch(MouseEvent{MouseAction::Down, MouseButton::Right,
                                             Point{lane.x + 3, lane.y + 1}, std::nullopt, Modifier::None}));
    CK_CHECK(fixture.app.input_capture() != nullptr);
    auto* menu = dynamic_cast<ckv::widgets::DropdownMenu*>(fixture.app.input_capture());
    CK_CHECK(menu != nullptr && menu->highlight().enabled);
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
    CK_CHECK(fixture.app.input_capture() == nullptr);
}

CK_TEST(todo_about_reports_build_and_workspace_facts) {
    SmokeFixture fixture;
    const auto about = fixture.app.commands().id_for("todo.about");
    CK_CHECK(about && fixture.app.execute_command(*about));
    CK_CHECK(fixture.app.is_modal());
    CK_CHECK(frame_text(fixture.app).find(
                 "Copyright (c) 2026 C. Klukas. All rights reserved.") != std::string::npos);
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(!fixture.app.is_modal());
}

CK_TEST(todo_transient_status_expires_to_context_and_move_guidance) {
    SmokeFixture fixture;
    const auto move = fixture.app.commands().id_for(TodoApp::kMoveTaskKey);
    CK_CHECK(move && fixture.app.execute_command(*move));
    fixture.monotonic.advance(3'000'000'000);
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.status_line()->current_hint() ==
             "Arrows choose the insertion point; Enter commits; Esc cancels.");
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
}

CK_TEST(todo_q_alias_is_scoped_to_the_board) {
    SmokeFixture fixture;
    const auto edit = fixture.app.commands().id_for(TodoApp::kEditTaskKey);
    CK_CHECK(edit && fixture.app.execute_command(*edit));
    CK_CHECK(fixture.app.dispatch(key(Key::Char, "q")));
    CK_CHECK(!fixture.app.quit_requested());
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.app.dispatch(key(Key::Char, "q")));
    CK_CHECK(fixture.app.quit_requested());
}

CK_TEST(todo_first_workspace_option_creates_without_a_modal_prompt) {
    ckv::term::HeadlessTerminal terminal(Size{80, 24});
    ManualClock monotonic;
    ckv::ui::Application app(terminal, monotonic);
    MemoryTodoRepository repository;
    FixedCalendarClock calendar(smoke_reading());
    TodoApp todo(app, repository, calendar, "first-run", TodoAppOptions{InitialWorkspace::Empty, std::nullopt});
    CK_CHECK(todo.controller().is_open());
    CK_CHECK(todo.controller().workspace()->snapshot().tasks.empty());
    CK_CHECK(!app.is_modal());
    const auto add = app.commands().id_for(TodoApp::kAddTaskKey);
    CK_CHECK(add && app.command_available(*add));
    CK_CHECK(app.dispatch(KeyEvent{KeyChord{Key::Char, Modifier::Alt, "t"}}));
    auto* tasks_menu = dynamic_cast<ckv::widgets::DropdownMenu*>(app.input_capture());
    CK_CHECK(tasks_menu != nullptr && tasks_menu->highlight().enabled);
    CK_CHECK(app.dispatch(key(Key::Enter)));
    CK_CHECK(app.is_modal());
    CK_CHECK(app.dispatch(key(Key::Escape)));
}

CK_TEST(todo_tasks_menu_opens_the_selected_tasks_note_editor) {
    SmokeFixture fixture;
    CK_CHECK(fixture.app.dispatch(KeyEvent{KeyChord{Key::Char, Modifier::Alt, "t"}}));
    auto* tasks_menu = dynamic_cast<ckv::widgets::DropdownMenu*>(fixture.app.input_capture());
    CK_CHECK(tasks_menu != nullptr);
    if (tasks_menu == nullptr) return;
    CK_CHECK(fixture.app.dispatch(key(Key::Down)));
    CK_CHECK(fixture.app.dispatch(key(Key::Down)));
    CK_CHECK(tasks_menu->highlight().command ==
             fixture.app.commands().id_for(TodoApp::kEditNoteKey).value_or(ckv::ui::kInvalidCommand));
    CK_CHECK(tasks_menu->highlight().enabled);
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    CK_CHECK(fixture.todo.note_window_count() == 1);
}

CK_TEST(todo_missing_initial_board_is_reported_and_the_last_board_opens) {
    ckv::term::HeadlessTerminal terminal(Size{80, 24});
    ManualClock monotonic;
    ckv::ui::Application app(terminal, monotonic);
    MemoryTodoRepository repository(smoke_workspace());
    FixedCalendarClock calendar(smoke_reading());
    TodoApp todo(app, repository, calendar, "missing-board",
                 TodoAppOptions{std::nullopt, std::string{"Does not exist"}});
    CK_CHECK(todo.board_view()->board_id() == BoardId{1});
    CK_CHECK(todo.status_line()->current_hint().find("was not found") != std::string::npos);
}

CK_TEST(todo_existing_initial_board_is_selected_durably_and_survives_refresh) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    CK_CHECK(workspace.add_board("Release"));
    ckv::term::HeadlessTerminal terminal(Size{80, 24});
    ManualClock monotonic;
    ckv::ui::Application app(terminal, monotonic);
    MemoryTodoRepository repository(std::move(workspace));
    FixedCalendarClock calendar(smoke_reading());
    TodoApp todo(app, repository, calendar, "requested-board",
                 TodoAppOptions{std::nullopt, std::string{"Release"}});

    CK_CHECK(todo.board_view()->board_id() == BoardId{2});
    CK_CHECK(todo.controller().workspace()->snapshot().last_board_id == BoardId{2});
    const auto stored = repository.load();
    CK_CHECK(stored && stored.value->workspace.snapshot().last_board_id == BoardId{2});
    todo.refresh_board();
    CK_CHECK(todo.board_view()->board_id() == BoardId{2});
}

CK_TEST(todo_poll_adopts_an_external_committed_revision) {
    SmokeFixture fixture;
    auto external = fixture.repository.load();
    TaskDraft draft;
    draft.title = "Remote task";
    CK_CHECK(external.value->workspace.add_task(LaneId{1}, std::move(draft),
                                                {IsoTimestamp{"2026-08-25T12:01:00Z"}, "other"}));
    CK_CHECK(fixture.repository.commit(external.value->revision, external.value->workspace,
                                       IsoDate{"2026-08-25"}));
    fixture.todo.poll_repository();
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{4}) != nullptr);
    CK_CHECK(fixture.todo.board_view()->lane_view(LaneId{1})->lane().task_ids.size() == 2);
}

CK_TEST(todo_poll_defers_external_reload_while_a_modal_edit_is_open) {
    SmokeFixture fixture;
    const auto edit = fixture.app.commands().id_for(TodoApp::kEditTaskKey);
    CK_CHECK(edit && fixture.app.execute_command(*edit));
    CK_CHECK(fixture.app.is_modal());
    commit_external_task_edit(fixture.repository, TaskId{1}, "Externally renamed");

    fixture.todo.poll_repository();
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1})->title !=
             "Externally renamed");
    CK_CHECK(fixture.app.dispatch(key(Key::Escape)));
    fixture.app.step(fixture.monotonic.now_nanos());
    fixture.todo.poll_repository();
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1})->title ==
             "Externally renamed");
}

CK_TEST(todo_unchanged_poll_does_not_redraw_the_terminal) {
    SmokeFixture fixture;
    fixture.app.step(fixture.monotonic.now_nanos());
    fixture.terminal.clear_written();
    fixture.todo.poll_repository();
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.terminal.written_bytes().empty());
}

CK_TEST(todo_non_overlapping_conflict_reloads_and_retries_once_automatically) {
    SmokeFixture fixture;
    commit_external_task(fixture.repository, "Remote task");

    const auto archive = fixture.app.commands().id_for(TodoApp::kArchiveTaskKey);
    CK_CHECK(archive);
    if (!archive) return;
    CK_CHECK(fixture.app.execute_command(*archive));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());

    const TodoWorkspace* workspace = fixture.todo.controller().workspace();
    CK_CHECK(workspace != nullptr);
    if (workspace == nullptr) return;
    CK_CHECK(workspace->find_task(TaskId{1}) == nullptr);
    CK_CHECK(workspace->find_task(TaskId{4}) != nullptr);
    CK_CHECK(fixture.repository.archives().size() == 1);
    CK_CHECK(!fixture.app.is_modal());
}

CK_TEST(todo_overlapping_conflict_waits_for_explicit_resolution) {
    SmokeFixture fixture;
    commit_external_task_edit(fixture.repository, TaskId{1}, "Externally renamed");

    const auto remove = fixture.app.commands().id_for(TodoApp::kDeleteTaskKey);
    CK_CHECK(remove);
    if (!remove) return;
    CK_CHECK(fixture.app.execute_command(*remove));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());

    const auto resolve = fixture.app.commands().id_for("todo.resolve-conflict");
    CK_CHECK(resolve);
    if (!resolve) return;
    CK_CHECK(fixture.app.commands().is_enabled(*resolve));
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1}) != nullptr);
    CK_CHECK(fixture.app.is_modal());

    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.todo.controller().workspace()->find_task(TaskId{1}) == nullptr);
    CK_CHECK(!fixture.app.commands().is_enabled(*resolve));
    CK_CHECK(!fixture.app.is_modal());
}

CK_TEST(todo_conflict_can_keep_the_external_version) {
    SmokeFixture fixture;
    commit_external_task_edit(fixture.repository, TaskId{1}, "Externally renamed");

    const auto remove = fixture.app.commands().id_for(TodoApp::kDeleteTaskKey);
    CK_CHECK(remove);
    if (!remove) return;
    CK_CHECK(fixture.app.execute_command(*remove));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    CK_CHECK(fixture.app.is_modal());

    CK_CHECK(fixture.app.dispatch(key(Key::Down)));
    CK_CHECK(fixture.app.dispatch(key(Key::Enter)));
    fixture.app.step(fixture.monotonic.now_nanos());
    const Task* task = fixture.todo.controller().workspace()->find_task(TaskId{1});
    CK_CHECK(task != nullptr);
    if (task == nullptr) return;
    CK_CHECK(task->title == "Externally renamed");
    CK_CHECK(!fixture.app.is_modal());
}

CK_TEST(todo_app_destruction_detaches_ui_and_withdraws_private_commands) {
    ckv::term::HeadlessTerminal terminal(Size{80, 24});
    ManualClock monotonic;
    ckv::ui::Application app(terminal, monotonic);
    MemoryTodoRepository repository(smoke_workspace());
    FixedCalendarClock calendar(smoke_reading());
    auto todo = std::make_unique<TodoApp>(app, repository, calendar, "lifetime");
    CK_CHECK(!app.root().children().empty());
    todo.reset();
    CK_CHECK(app.root().children().empty());
    CK_CHECK(!app.commands().id_for(TodoApp::kAddTaskKey).has_value());
}

// Help names keys through the registry: a runtime rebind of Add task shows in
// the board topic the next time help opens, and the new chord adds a task.
CK_TEST(todo_help_names_the_chord_a_command_is_bound_to_now) {
    SmokeFixture fixture;
    const auto on_screen = [&fixture](std::string_view needle) {
        const FrameView frame = fixture.terminal.display().frame();
        for (int y = 0; y < frame.size().height; ++y) {
            std::string row;
            for (int x = 0; x < frame.size().width; ++x) row += frame.at(Point{x, y}).grapheme();
            if (row.find(needle) != std::string::npos) return true;
        }
        return false;
    };
    const auto press = [&fixture](KeyChord chord) {
        fixture.terminal.inject_event(KeyEvent{std::move(chord)});
        fixture.app.step(fixture.monotonic.now_nanos());
    };
    fixture.app.step(0);
    press(KeyChord{Key::F1, Modifier::None, {}});
    CK_CHECK(on_screen("F2 adds"));
    press(KeyChord{Key::Escape, Modifier::None, {}});

    const ckv::ui::CommandId add = *fixture.app.commands().id_for(TodoApp::kAddTaskKey);
    while (const auto bound = fixture.app.commands().chord_for_command(add))
        fixture.app.commands().unbind_key(*bound);
    fixture.app.commands().bind_key(KeyChord{Key::F11, Modifier::None, {}}, add);
    press(KeyChord{Key::F1, Modifier::None, {}});
    CK_CHECK(on_screen("F11 adds"));
    CK_CHECK(!on_screen("F2 adds"));
    press(KeyChord{Key::Escape, Modifier::None, {}});

    CK_CHECK(!fixture.app.is_modal());
    press(KeyChord{Key::F11, Modifier::None, {}});
    CK_CHECK(fixture.app.is_modal());  // the Add task dialog
}
