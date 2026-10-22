// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Visual contracts for the TODO demo. Regenerate deliberately with:
//   build/tools/docgen/capture_todo_screenshots docs/generated/screenshots tests/golden
// and review both the golden and SVG diffs before committing them.
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "cvision/core/golden.hpp"
#include "cvision/scene/golden_capture.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/testing/cktest.hpp"

#include "memory_todo_repository.hpp"
#include "todo_app.hpp"

namespace {

using namespace ckv;
using namespace ck::todo;

CalendarReading golden_reading() {
    return {IsoTimestamp{"2026-08-25T12:00:00Z"}, IsoDate{"2026-08-25"}, IsoTime{"14:30"}};
}

AuditStamp golden_stamp(std::string identity = "demo") {
    return {IsoTimestamp{"2026-08-25T11:00:00Z"}, std::move(identity)};
}

TodoWorkspace golden_workspace() {
    const auto guided = TodoWorkspace::guided(golden_stamp());
    return guided ? *guided.value : TodoWorkspace::empty();
}

struct GoldenScene {
    ckv::term::HeadlessTerminal terminal;
    ManualClock monotonic;
    ckv::ui::Application app;
    MemoryTodoRepository repository;
    FixedCalendarClock calendar;
    TodoApp todo;

    explicit GoldenScene(Size size = {100, 30}, TodoWorkspace workspace = golden_workspace())
        : terminal(size, ckv::term::headless_no_graphics_profile()),
          app(terminal, monotonic),
          repository(std::move(workspace)),
          calendar(golden_reading()),
          todo(app, repository, calendar, "capture", {.workspace_description = "in-memory demo workspace"}) {}
};

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

bool execute(GoldenScene& scene, std::string_view key) {
    const auto command = scene.app.commands().id_for(key);
    return command.has_value() && scene.app.execute_command(*command);
}

KeyEvent key(Key value) { return KeyEvent{KeyChord{value, Modifier::None, {}}}; }

void expect_golden(std::string_view name, GoldenScene& scene) {
    scene.app.step(scene.monotonic.now_nanos());
    const std::string actual =
        golden::serialize(ckv::scene::capture(scene.app.composed_surface(), scene.app.current_cursor()));
    const std::string expected = read_file("golden/" + std::string(name) + ".dump");
    CK_CHECK(!expected.empty());
    CK_CHECK(actual == expected);
}

TodoWorkspace many_lane_workspace() {
    TodoWorkspace workspace = golden_workspace();
    for (std::string name : {"Review", "Blocked", "Later"}) {
        const auto lane = workspace.insert_lane(BoardId{1}, std::move(name));
        if (!lane) return TodoWorkspace::empty();
        TaskDraft task;
        task.title = "A task in this lane";
        task.details = "Horizontal scrolling keeps every lane useful";
        if (!workspace.add_task(*lane.value, std::move(task),
                                {IsoTimestamp{"2026-08-25T12:01:00Z"}, "external-instance"}))
            return TodoWorkspace::empty();
    }
    return workspace;
}

void commit_external_title(GoldenScene& scene) {
    auto loaded = scene.repository.load();
    CK_CHECK(loaded);
    if (!loaded) return;
    const Task* task = loaded.value->workspace.find_task(TaskId{1});
    CK_CHECK(task != nullptr);
    if (task == nullptr) return;
    TaskDraft draft{
        task->title, task->details, task->note, task->priority, task->due_date, task->due_time, task->color};
    draft.title = "Changed in another instance";
    CK_CHECK(loaded.value->workspace.edit_task(
        TaskId{1}, std::move(draft), {IsoTimestamp{"2026-08-25T12:01:00Z"}, "external-instance"}));
    CK_CHECK(scene.repository.commit(loaded.value->revision, loaded.value->workspace,
                                     golden_reading().local_date));
}

}  // namespace

CK_TEST(todo_guided_board_matches_its_pinned_golden) {
    GoldenScene scene;
    expect_golden("todo-guided", scene);
}

CK_TEST(todo_task_editor_matches_its_pinned_golden) {
    GoldenScene scene;
    CK_CHECK(execute(scene, TodoApp::kEditTaskKey));
    expect_golden("todo-task-editor", scene);
}

CK_TEST(todo_move_mode_matches_its_pinned_golden) {
    GoldenScene scene;
    CK_CHECK(execute(scene, TodoApp::kMoveTaskKey));
    CK_CHECK(scene.app.dispatch(key(Key::Right)));
    expect_golden("todo-move-mode", scene);
}

CK_TEST(todo_pointer_drag_insertion_matches_its_pinned_golden) {
    GoldenScene scene;
    scene.app.step(scene.monotonic.now_nanos());
    const Rect source = scene.todo.board_view()->lane_view(LaneId{1})->absolute_bounds();
    const Rect target = scene.todo.board_view()->lane_view(LaneId{2})->absolute_bounds();
    CK_CHECK(scene.app.dispatch(MouseEvent{MouseAction::Down, MouseButton::Left,
                                           Point{source.x + 3, source.y + 1},
                                           std::nullopt, Modifier::None}));
    CK_CHECK(scene.app.dispatch(MouseEvent{MouseAction::Move, MouseButton::None,
                                           Point{target.x + 3, target.y + 1},
                                           std::nullopt, Modifier::None}));
    expect_golden("todo-drag-insertion", scene);
}

CK_TEST(todo_destructive_confirmations_match_their_pinned_goldens) {
    GoldenScene archive;
    CK_CHECK(execute(archive, TodoApp::kArchiveTaskKey));
    expect_golden("todo-archive-confirmation", archive);

    GoldenScene remove;
    CK_CHECK(execute(remove, TodoApp::kDeleteTaskKey));
    expect_golden("todo-delete-confirmation", remove);
}

CK_TEST(todo_lane_actions_match_their_pinned_golden) {
    GoldenScene scene;
    CK_CHECK(execute(scene, TodoApp::kLaneActionsKey));
    expect_golden("todo-lane-actions", scene);
}

CK_TEST(todo_board_manager_matches_its_pinned_golden) {
    GoldenScene scene;
    CK_CHECK(execute(scene, TodoApp::kBoardManagerKey));
    expect_golden("todo-board-manager", scene);
}

CK_TEST(todo_note_editor_matches_its_pinned_golden) {
    GoldenScene scene;
    CK_CHECK(execute(scene, TodoApp::kEditNoteKey));
    CK_CHECK(scene.app.dispatch(TextEvent{
        "A modeless ckVision TextEditor note.\nUndo, find, clipboard, and wrap stay available.", false, false}));
    expect_golden("todo-note-editor", scene);
}

CK_TEST(todo_narrow_many_lane_board_matches_its_pinned_golden) {
    GoldenScene scene({72, 22}, many_lane_workspace());
    expect_golden("todo-narrow-many-lanes", scene);
}

CK_TEST(todo_conflict_resolution_matches_its_pinned_golden) {
    GoldenScene scene;
    commit_external_title(scene);
    CK_CHECK(execute(scene, TodoApp::kDeleteTaskKey));
    CK_CHECK(scene.app.dispatch(key(Key::Enter)));
    scene.app.step(scene.monotonic.now_nanos());
    expect_golden("todo-conflict-resolution", scene);
}

CK_TEST(todo_themes_match_their_pinned_goldens) {
    for (const auto& [theme, name] : {std::pair{TodoTheme::Dark, "todo-theme-dark"},
                                    std::pair{TodoTheme::Light, "todo-theme-light"},
                                    std::pair{TodoTheme::Mono, "todo-theme-mono"},
                                    std::pair{TodoTheme::HighContrast, "todo-theme-high-contrast"}}) {
        GoldenScene scene;
        CK_CHECK(execute(scene, TodoApp::kThemeKeys[static_cast<std::size_t>(theme)]));
        expect_golden(name, scene);
    }
}
