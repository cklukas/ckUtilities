// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_model.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include "cvision/testing/cktest.hpp"

namespace {

using namespace ck::todo;

AuditStamp stamp() { return {IsoTimestamp{"2026-08-25T10:30:00Z"}, "demo-user"}; }

WorkspaceSnapshot valid_snapshot() { return TodoWorkspace::empty().snapshot(); }

ModelErrorCode error_for(WorkspaceSnapshot snapshot) {
    return TodoWorkspace::from_snapshot(std::move(snapshot)).error.code;
}

Task one_task() {
    return Task{TaskId{1},
                "Ship the demo",
                "Complete the deterministic model",
                "A note\nwith two lines.",
                Priority::High,
                IsoDate{"2026-08-31"},
                std::nullopt,
                TodoColor::Blue,
                IsoTimestamp{"2026-08-25T10:30:00Z"},
                "creator",
                IsoTimestamp{"2026-08-25T10:31:00Z"},
                "editor"};
}

WorkspaceSnapshot snapshot_with_task() {
    WorkspaceSnapshot snapshot = valid_snapshot();
    snapshot.tasks.push_back(one_task());
    snapshot.boards.front().lanes.front().task_ids.push_back(TaskId{1});
    snapshot.next_task_id = 2;
    return snapshot;
}

Task task_with(TaskId id,
               std::string title,
               Priority priority,
               std::optional<IsoDate> due,
               std::optional<TodoColor> color,
               std::string created,
               std::string modified) {
    Task task = one_task();
    task.id = id;
    task.title = std::move(title);
    task.priority = priority;
    task.due_date = std::move(due);
    task.color = color;
    task.created_at = IsoTimestamp{std::move(created)};
    task.modified_at = IsoTimestamp{std::move(modified)};
    return task;
}

WorkspaceSnapshot sortable_snapshot() {
    WorkspaceSnapshot snapshot = valid_snapshot();
    snapshot.tasks = {
        task_with(TaskId{1}, "one", Priority::Low, IsoDate{"2026-08-31"}, TodoColor::Blue,
                  "2026-08-25T10:00:00Z", "2026-08-25T10:04:00Z"),
        task_with(TaskId{2}, "two", Priority::High, IsoDate{"2026-08-30"}, std::nullopt,
                  "2026-08-25T10:01:00Z", "2026-08-25T10:03:00Z"),
        task_with(TaskId{3}, "three", Priority::Normal, std::nullopt, TodoColor::DarkRed,
                  "2026-08-25T10:02:00Z", "2026-08-25T10:02:00Z"),
        task_with(TaskId{4}, "four", Priority::High, IsoDate{"2026-08-30"}, TodoColor::Blue,
                  "2026-08-25T10:03:00Z", "2026-08-25T10:05:00Z"),
    };
    snapshot.boards.front().lanes.front().task_ids = {TaskId{1}, TaskId{2}, TaskId{3}, TaskId{4}};
    snapshot.next_task_id = 5;
    return snapshot;
}

TodoWorkspace workspace_from(WorkspaceSnapshot snapshot) {
    auto result = TodoWorkspace::from_snapshot(std::move(snapshot));
    if (!result) return TodoWorkspace::empty();
    return std::move(*result.value);
}

TaskDraft draft(std::string title = "New task") {
    return TaskDraft{
        std::move(title), "Details", "Note", Priority::Normal, std::nullopt, std::nullopt, std::nullopt};
}

WorkspaceSnapshot board_merge_snapshot() {
    WorkspaceSnapshot snapshot = valid_snapshot();
    snapshot.boards.front().lanes[0].color = TodoColor::Green;
    snapshot.boards.front().lanes[0].sort = SortMode::Due;
    snapshot.boards.front().lanes[2].title = "To Do";
    snapshot.boards.push_back(
        Board{BoardId{2},
              "project",
              {Lane{LaneId{4}, "To Do", TodoColor::Red, SortMode::Priority, {TaskId{3}}},
               Lane{LaneId{5}, "Later", std::nullopt, SortMode::Manual, {TaskId{4}}},
               Lane{LaneId{6}, "Later", TodoColor::Blue, SortMode::Color, {TaskId{5}}}}});
    snapshot.tasks = {
        task_with(TaskId{1}, "target todo", Priority::Normal, std::nullopt, std::nullopt,
                  "2026-08-25T10:00:00Z", "2026-08-25T10:00:00Z"),
        task_with(TaskId{2}, "target doing", Priority::Normal, std::nullopt, std::nullopt,
                  "2026-08-25T10:01:00Z", "2026-08-25T10:01:00Z"),
        task_with(TaskId{3}, "source todo", Priority::Normal, std::nullopt, std::nullopt,
                  "2026-08-25T10:02:00Z", "2026-08-25T10:02:00Z"),
        task_with(TaskId{4}, "source later one", Priority::Normal, std::nullopt, std::nullopt,
                  "2026-08-25T10:03:00Z", "2026-08-25T10:03:00Z"),
        task_with(TaskId{5}, "source later two", Priority::Normal, std::nullopt, std::nullopt,
                  "2026-08-25T10:04:00Z", "2026-08-25T10:04:00Z"),
    };
    snapshot.boards[0].lanes[0].task_ids = {TaskId{1}};
    snapshot.boards[0].lanes[1].task_ids = {TaskId{2}};
    snapshot.last_board_id = BoardId{2};
    snapshot.next_board_id = 3;
    snapshot.next_lane_id = 7;
    snapshot.next_task_id = 6;
    return snapshot;
}

}  // namespace

CK_TEST(todo_empty_factory_has_main_and_three_default_lanes) {
    const TodoWorkspace workspace = TodoWorkspace::empty();
    CK_CHECK(workspace.snapshot().schema_version == todo_schema_version);
    CK_CHECK(workspace.snapshot().last_board_id == BoardId{1});
    CK_CHECK(workspace.snapshot().boards.size() == 1);
    CK_CHECK(workspace.snapshot().boards.front().name == "main");
    CK_CHECK(workspace.snapshot().boards.front().lanes.size() == 3);
    CK_CHECK(workspace.snapshot().boards.front().lanes[0].title == "To Do");
    CK_CHECK(workspace.snapshot().boards.front().lanes[1].title == "Doing");
    CK_CHECK(workspace.snapshot().boards.front().lanes[2].title == "Done");
    CK_CHECK(workspace.snapshot().next_board_id == 2);
    CK_CHECK(workspace.snapshot().next_lane_id == 4);
    CK_CHECK(workspace.snapshot().next_task_id == 1);
    CK_CHECK(workspace.generation() == 0);
}

CK_TEST(todo_guided_factory_creates_three_ordinary_audited_tasks) {
    const auto result = TodoWorkspace::guided(stamp());
    CK_CHECK(result);
    if (!result) return;
    const TodoWorkspace& workspace = *result.value;
    CK_CHECK(workspace.snapshot().tasks.size() == 3);
    CK_CHECK(workspace.snapshot().next_task_id == 4);
    CK_CHECK(workspace.snapshot().boards.front().lanes[0].task_ids == std::vector<TaskId>{TaskId{1}});
    CK_CHECK(workspace.snapshot().boards.front().lanes[1].task_ids == std::vector<TaskId>{TaskId{2}});
    CK_CHECK(workspace.snapshot().boards.front().lanes[2].task_ids == std::vector<TaskId>{TaskId{3}});
    for (const Task& task : workspace.snapshot().tasks) {
        CK_CHECK(task.created_at == stamp().timestamp);
        CK_CHECK(task.modified_at == stamp().timestamp);
        CK_CHECK(task.created_by == stamp().identity);
        CK_CHECK(task.modified_by == stamp().identity);
    }
}

CK_TEST(todo_guided_factory_rejects_invalid_audit_facts) {
    CK_CHECK(!TodoWorkspace::guided({IsoTimestamp{"25 August"}, "demo-user"}));
    CK_CHECK(!TodoWorkspace::guided({IsoTimestamp{"2026-08-25T10:30:00Z"}, ""}));
}

CK_TEST(todo_snapshot_round_trip_preserves_every_field_and_lookup) {
    const WorkspaceSnapshot expected = snapshot_with_task();
    auto result = TodoWorkspace::from_snapshot(expected);
    CK_CHECK(result);
    if (!result) return;
    CK_CHECK(result.value->snapshot() == expected);
    CK_CHECK(result.value->find_board(BoardId{1}) != nullptr);
    CK_CHECK(result.value->find_lane(LaneId{1}) != nullptr);
    CK_CHECK(result.value->find_task(TaskId{1}) != nullptr);
    if (result.value->find_task(TaskId{1}) == nullptr) return;
    CK_CHECK(result.value->board_of(LaneId{1}) == BoardId{1});
    CK_CHECK(result.value->lane_of(TaskId{1}) == LaneId{1});
    CK_CHECK(result.value->find_task(TaskId{1})->note == "A note\nwith two lines.");
    CK_CHECK(result.value->find_board(BoardId{99}) == nullptr);
    CK_CHECK(!result.value->board_of(LaneId{99}));
    CK_CHECK(!result.value->lane_of(TaskId{99}));
}

CK_TEST(todo_snapshot_rejects_schema_main_and_selection_errors) {
    auto snapshot = valid_snapshot();
    snapshot.schema_version = 3;
    CK_CHECK(error_for(snapshot) == ModelErrorCode::UnknownSchema);

    snapshot = valid_snapshot();
    snapshot.boards.front().name = "not-main";
    CK_CHECK(error_for(snapshot) == ModelErrorCode::ProtectedMainBoard);

    snapshot = valid_snapshot();
    snapshot.last_board_id = BoardId{99};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidLastBoard);
}

CK_TEST(todo_snapshot_rejects_zero_duplicate_and_nonmonotonic_ids) {
    auto snapshot = valid_snapshot();
    snapshot.boards.front().id = BoardId{};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidId);

    snapshot = valid_snapshot();
    snapshot.boards.front().lanes[1].id = snapshot.boards.front().lanes[0].id;
    CK_CHECK(error_for(snapshot) == ModelErrorCode::DuplicateId);

    snapshot = snapshot_with_task();
    snapshot.next_task_id = 1;
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidCounter);
}

CK_TEST(todo_snapshot_rejects_dangling_duplicate_and_unowned_tasks) {
    auto snapshot = valid_snapshot();
    snapshot.boards.front().lanes.front().task_ids = {TaskId{99}};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::DanglingTask);

    snapshot = snapshot_with_task();
    snapshot.boards.front().lanes[1].task_ids = {TaskId{1}};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::DuplicateTaskMembership);

    snapshot = snapshot_with_task();
    snapshot.boards.front().lanes.front().task_ids.clear();
    CK_CHECK(error_for(snapshot) == ModelErrorCode::UnownedTask);
}

CK_TEST(todo_snapshot_rejects_invalid_utf8_empty_and_oversized_text) {
    auto snapshot = valid_snapshot();
    snapshot.boards.front().name = std::string("bad\xFF", 4);
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidUtf8);

    snapshot = valid_snapshot();
    snapshot.boards.front().lanes.front().title.clear();
    CK_CHECK(error_for(snapshot) == ModelErrorCode::EmptyText);

    snapshot = snapshot_with_task();
    snapshot.tasks.front().title.assign(TodoLimits::max_title_bytes + 1, 'x');
    CK_CHECK(error_for(snapshot) == ModelErrorCode::TextLimit);

    snapshot = snapshot_with_task();
    snapshot.tasks.front().title = "two\tcolumns";
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidText);

    snapshot = snapshot_with_task();
    snapshot.tasks.front().note = "tabs\tand\nlines are valid";
    CK_CHECK(TodoWorkspace::from_snapshot(snapshot));
}

CK_TEST(todo_snapshot_rejects_unknown_enum_values) {
    auto snapshot = snapshot_with_task();
    snapshot.tasks.front().priority = static_cast<Priority>(99);
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidValue);

    snapshot = valid_snapshot();
    snapshot.boards.front().lanes.front().sort = static_cast<SortMode>(99);
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidValue);
}

CK_TEST(todo_snapshot_accepts_real_leap_day_and_rejects_invalid_dates) {
    auto snapshot = snapshot_with_task();
    snapshot.tasks.front().due_date = IsoDate{"2024-02-29"};
    CK_CHECK(TodoWorkspace::from_snapshot(snapshot));

    snapshot.tasks.front().due_date = IsoDate{"2025-02-29"};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidDate);
    snapshot.tasks.front().due_date = IsoDate{"2026-13-01"};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidDate);
}

CK_TEST(todo_snapshot_accepts_canonical_due_times_and_requires_a_due_date) {
    auto snapshot = snapshot_with_task();
    snapshot.tasks.front().due_time = IsoTime{"14:30"};
    CK_CHECK(TodoWorkspace::from_snapshot(snapshot));

    snapshot.tasks.front().due_time = IsoTime{"24:00"};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidValue);
    snapshot.tasks.front().due_time = IsoTime{"2:30"};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidValue);
    snapshot.tasks.front().due_time = IsoTime{"14:30"};
    snapshot.tasks.front().due_date.reset();
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidValue);
}

CK_TEST(todo_snapshot_rejects_invalid_and_regressing_timestamps) {
    auto snapshot = snapshot_with_task();
    snapshot.tasks.front().modified_at = IsoTimestamp{"2026-08-25T25:00:00Z"};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::InvalidTimestamp);

    snapshot = snapshot_with_task();
    snapshot.tasks.front().modified_at = IsoTimestamp{"2026-08-25T10:29:59Z"};
    CK_CHECK(error_for(snapshot) == ModelErrorCode::TimestampRegression);
}

CK_TEST(todo_snapshot_enforces_board_lane_task_and_aggregate_limits) {
    auto snapshot = valid_snapshot();
    snapshot.boards.front().lanes.resize(TodoLimits::max_lanes_per_board + 1,
                                         Lane{LaneId{99}, "extra", std::nullopt, SortMode::Manual, {}});
    CK_CHECK(error_for(snapshot) == ModelErrorCode::WorkspaceLimit);

    snapshot = valid_snapshot();
    snapshot.boards.clear();
    CK_CHECK(error_for(snapshot) == ModelErrorCode::WorkspaceLimit);

    snapshot = snapshot_with_task();
    snapshot.tasks.front().note.assign(TodoLimits::max_note_bytes, 'n');
    snapshot.tasks.front().details.assign(TodoLimits::max_details_bytes, 'd');
    CK_CHECK(TodoWorkspace::from_snapshot(snapshot));
}

CK_TEST(todo_snapshot_allows_duplicate_lane_titles_but_not_board_names) {
    auto snapshot = valid_snapshot();
    snapshot.boards.front().lanes[1].title = snapshot.boards.front().lanes[0].title;
    CK_CHECK(TodoWorkspace::from_snapshot(snapshot));

    snapshot = valid_snapshot();
    snapshot.boards.push_back(Board{BoardId{2}, "main", {Lane{LaneId{4}, "Only", std::nullopt, {}, {}}}});
    snapshot.next_board_id = 3;
    snapshot.next_lane_id = 5;
    CK_CHECK(error_for(snapshot) == ModelErrorCode::DuplicateBoardName);
}

CK_TEST(todo_task_add_uses_stable_ids_anchors_and_audit_facts) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    auto first = workspace.add_task(LaneId{1}, draft("first"), stamp());
    CK_CHECK(first && *first.value == TaskId{1});
    CK_CHECK(workspace.generation() == 1);
    const Task* stored = workspace.find_task(TaskId{1});
    CK_CHECK(stored != nullptr);
    if (stored == nullptr) return;
    CK_CHECK(stored->created_at == stamp().timestamp);
    CK_CHECK(stored->modified_at == stamp().timestamp);
    CK_CHECK(stored->created_by == stamp().identity);

    auto second = workspace.add_task(LaneId{1}, draft("before first"), stamp(), TaskId{1});
    CK_CHECK(second && *second.value == TaskId{2});
    CK_CHECK(workspace.find_lane(LaneId{1})->task_ids == std::vector<TaskId>({TaskId{2}, TaskId{1}}));
    CK_CHECK(workspace.snapshot().next_task_id == 3);
}

CK_TEST(todo_task_add_failure_preserves_ids_generation_and_snapshot) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    const WorkspaceSnapshot before = workspace.snapshot();
    TaskDraft invalid = draft();
    invalid.title.clear();
    const auto rejected = workspace.add_task(LaneId{1}, invalid, stamp());
    CK_CHECK(!rejected && rejected.error.code == ModelErrorCode::EmptyText);
    CK_CHECK(workspace.snapshot() == before);
    CK_CHECK(workspace.generation() == 0);

    const auto missing_lane = workspace.add_task(LaneId{99}, draft(), stamp());
    CK_CHECK(!missing_lane && missing_lane.error.code == ModelErrorCode::LaneNotFound);
    CK_CHECK(workspace.snapshot().next_task_id == 1);
}

CK_TEST(todo_task_edit_updates_only_modified_facts_and_noop_is_free) {
    TodoWorkspace workspace = workspace_from(snapshot_with_task());
    const Task original = *workspace.find_task(TaskId{1});
    TaskDraft same{original.title,
                   original.details,
                   original.note,
                   original.priority,
                   original.due_date,
                   original.due_time,
                   original.color};
    const auto no_op = workspace.edit_task(TaskId{1}, same, {IsoTimestamp{"invalid"}, ""});
    CK_CHECK(no_op && !no_op.value->changed);
    CK_CHECK(workspace.generation() == 0);

    same.title = "Updated title";
    const AuditStamp update{IsoTimestamp{"2026-08-25T10:32:00Z"}, "second-editor"};
    const auto changed = workspace.edit_task(TaskId{1}, same, update);
    CK_CHECK(changed && changed.value->changed);
    const Task* edited = workspace.find_task(TaskId{1});
    CK_CHECK(edited != nullptr);
    if (edited == nullptr) return;
    CK_CHECK(edited->created_at == original.created_at);
    CK_CHECK(edited->created_by == original.created_by);
    CK_CHECK(edited->modified_at == update.timestamp);
    CK_CHECK(edited->modified_by == update.identity);
    CK_CHECK(workspace.generation() == 1);
}

CK_TEST(todo_task_edit_rejects_regression_and_invalid_content_atomically) {
    TodoWorkspace workspace = workspace_from(snapshot_with_task());
    const WorkspaceSnapshot before = workspace.snapshot();
    TaskDraft changed = draft("changed");
    auto result = workspace.edit_task(
        TaskId{1}, changed, {IsoTimestamp{"2026-08-25T10:30:30Z"}, "editor"});
    CK_CHECK(!result && result.error.code == ModelErrorCode::TimestampRegression);
    CK_CHECK(workspace.snapshot() == before);

    changed.title = std::string("bad\xFF", 4);
    result = workspace.edit_task(
        TaskId{1}, changed, {IsoTimestamp{"2026-08-25T10:32:00Z"}, "editor"});
    CK_CHECK(!result && result.error.code == ModelErrorCode::InvalidUtf8);
    CK_CHECK(workspace.snapshot() == before);
}

CK_TEST(todo_task_delete_removes_membership_without_reusing_the_id) {
    TodoWorkspace workspace = workspace_from(snapshot_with_task());
    const auto result = workspace.delete_task(TaskId{1});
    CK_CHECK(result && result.value->changed);
    CK_CHECK(workspace.find_task(TaskId{1}) == nullptr);
    CK_CHECK(workspace.find_lane(LaneId{1})->task_ids.empty());
    CK_CHECK(workspace.snapshot().next_task_id == 2);
    CK_CHECK(workspace.generation() == 1);
    const auto missing = workspace.delete_task(TaskId{1});
    CK_CHECK(!missing && missing.error.code == ModelErrorCode::TaskNotFound);
    CK_CHECK(workspace.generation() == 1);
}

CK_TEST(todo_task_ordering_is_stable_for_every_sort_mode) {
    struct Case {
        SortMode mode;
        std::vector<TaskId> expected;
    };
    const std::vector<Case> cases = {
        {SortMode::Manual, {TaskId{1}, TaskId{2}, TaskId{3}, TaskId{4}}},
        {SortMode::Color, {TaskId{3}, TaskId{1}, TaskId{4}, TaskId{2}}},
        {SortMode::Due, {TaskId{2}, TaskId{4}, TaskId{1}, TaskId{3}}},
        {SortMode::Created, {TaskId{1}, TaskId{2}, TaskId{3}, TaskId{4}}},
        {SortMode::Modified, {TaskId{3}, TaskId{2}, TaskId{1}, TaskId{4}}},
        {SortMode::Priority, {TaskId{2}, TaskId{4}, TaskId{3}, TaskId{1}}},
    };
    for (const Case& item : cases) {
        WorkspaceSnapshot snapshot = sortable_snapshot();
        snapshot.boards.front().lanes.front().sort = item.mode;
        TodoWorkspace workspace = workspace_from(std::move(snapshot));
        const auto ordered = workspace.ordered_tasks(LaneId{1});
        CK_CHECK(ordered && *ordered.value == item.expected);
        CK_CHECK(workspace.find_lane(LaneId{1})->task_ids ==
                 std::vector<TaskId>({TaskId{1}, TaskId{2}, TaskId{3}, TaskId{4}}));
    }
    CK_CHECK(TodoWorkspace::empty().ordered_tasks(LaneId{99}).error.code == ModelErrorCode::LaneNotFound);
}

CK_TEST(todo_due_order_places_all_day_tasks_before_timed_tasks_on_the_same_date) {
    WorkspaceSnapshot snapshot = valid_snapshot();
    Task all_day = task_with(TaskId{1}, "all day", Priority::Normal, IsoDate{"2026-08-30"},
                             std::nullopt, "2026-08-25T10:00:00Z", "2026-08-25T10:00:00Z");
    Task late = task_with(TaskId{2}, "late", Priority::Normal, IsoDate{"2026-08-30"},
                          std::nullopt, "2026-08-25T10:00:00Z", "2026-08-25T10:00:00Z");
    late.due_time = IsoTime{"17:00"};
    Task early = task_with(TaskId{3}, "early", Priority::Normal, IsoDate{"2026-08-30"},
                           std::nullopt, "2026-08-25T10:00:00Z", "2026-08-25T10:00:00Z");
    early.due_time = IsoTime{"08:30"};
    snapshot.tasks = {all_day, late, early};
    snapshot.boards.front().lanes.front().sort = SortMode::Due;
    snapshot.boards.front().lanes.front().task_ids = {TaskId{2}, TaskId{1}, TaskId{3}};
    snapshot.next_task_id = 4;
    TodoWorkspace workspace = workspace_from(std::move(snapshot));
    const auto ordered = workspace.ordered_tasks(LaneId{1});
    CK_CHECK(ordered && *ordered.value == std::vector<TaskId>({TaskId{1}, TaskId{3}, TaskId{2}}));
}

CK_TEST(todo_task_move_stages_without_mutation_and_commit_uses_stable_anchor) {
    TodoWorkspace workspace = workspace_from(sortable_snapshot());
    const WorkspaceSnapshot original = workspace.snapshot();
    const auto begun = workspace.begin_task_move(TaskId{2});
    CK_CHECK(begun && begun.value->original_before_task_id == TaskId{3});
    const auto staged = workspace.stage_task_move(*begun.value, LaneId{1}, TaskId{1});
    CK_CHECK(staged);
    CK_CHECK(workspace.snapshot() == original);
    const auto committed = workspace.commit_task_move(*staged.value);
    CK_CHECK(committed && committed.value->changed);
    CK_CHECK(workspace.find_lane(LaneId{1})->task_ids ==
             std::vector<TaskId>({TaskId{2}, TaskId{1}, TaskId{3}, TaskId{4}}));
    CK_CHECK(workspace.generation() == 1);
}

CK_TEST(todo_task_move_cancel_and_same_position_commit_are_noops) {
    TodoWorkspace workspace = workspace_from(sortable_snapshot());
    const WorkspaceSnapshot original = workspace.snapshot();
    const auto begun = workspace.begin_task_move(TaskId{2});
    CK_CHECK(begun);
    CK_CHECK(workspace.snapshot() == original);
    CK_CHECK(workspace.generation() == 0);

    const auto committed = workspace.commit_task_move(*begun.value);
    CK_CHECK(committed && !committed.value->changed);
    CK_CHECK(workspace.snapshot() == original);
    CK_CHECK(workspace.generation() == 0);
}

CK_TEST(todo_task_move_crosses_lanes_and_sorted_targets_append) {
    WorkspaceSnapshot snapshot = sortable_snapshot();
    snapshot.boards.front().lanes[1].sort = SortMode::Due;
    TodoWorkspace workspace = workspace_from(std::move(snapshot));
    const auto begun = workspace.begin_task_move(TaskId{2});
    const auto staged = workspace.stage_task_move(*begun.value, LaneId{2}, std::nullopt);
    CK_CHECK(staged);
    const auto committed = workspace.commit_task_move(*staged.value);
    CK_CHECK(committed && committed.value->changed);
    CK_CHECK(workspace.lane_of(TaskId{2}) == LaneId{2});
    CK_CHECK(workspace.find_lane(LaneId{1})->task_ids == std::vector<TaskId>({TaskId{1}, TaskId{3}, TaskId{4}}));
    CK_CHECK(workspace.find_lane(LaneId{2})->task_ids == std::vector<TaskId>{TaskId{2}});
}

CK_TEST(todo_task_move_rejects_sorted_reorder_invalid_anchor_and_stale_token) {
    WorkspaceSnapshot snapshot = sortable_snapshot();
    snapshot.boards.front().lanes.front().sort = SortMode::Priority;
    TodoWorkspace workspace = workspace_from(std::move(snapshot));
    const auto begun = workspace.begin_task_move(TaskId{2});
    auto staged = workspace.stage_task_move(*begun.value, LaneId{1}, TaskId{1});
    CK_CHECK(!staged && staged.error.code == ModelErrorCode::ManualOrderRequired);
    staged = workspace.stage_task_move(*begun.value, LaneId{2}, TaskId{99});
    CK_CHECK(!staged && staged.error.code == ModelErrorCode::InvalidInsertionAnchor);

    TaskDraft changed = draft("changed after move began");
    const auto edited = workspace.edit_task(
        TaskId{1}, changed, {IsoTimestamp{"2026-08-25T10:06:00Z"}, "editor"});
    CK_CHECK(edited);
    const auto stale = workspace.commit_task_move(*begun.value);
    CK_CHECK(!stale && stale.error.code == ModelErrorCode::StaleTransaction);
}

CK_TEST(todo_lane_insert_rename_color_and_sort_are_validated_transitions) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    const auto inserted = workspace.insert_lane(BoardId{1}, "Review", LaneId{2});
    CK_CHECK(inserted && *inserted.value == LaneId{4});
    CK_CHECK(workspace.snapshot().boards.front().lanes[1].id == LaneId{4});
    CK_CHECK(workspace.snapshot().next_lane_id == 5);

    auto mutation = workspace.rename_lane(LaneId{4}, "Ready");
    CK_CHECK(mutation && mutation.value->changed);
    mutation = workspace.set_lane_color(LaneId{4}, TodoColor::DarkCyan);
    CK_CHECK(mutation && mutation.value->changed);
    mutation = workspace.set_lane_sort(LaneId{4}, SortMode::Modified);
    CK_CHECK(mutation && mutation.value->changed);
    CK_CHECK(workspace.find_lane(LaneId{4})->title == "Ready");
    CK_CHECK(workspace.find_lane(LaneId{4})->color == TodoColor::DarkCyan);
    CK_CHECK(workspace.find_lane(LaneId{4})->sort == SortMode::Modified);

    const std::uint64_t generation = workspace.generation();
    mutation = workspace.rename_lane(LaneId{4}, "Ready");
    CK_CHECK(mutation && !mutation.value->changed);
    CK_CHECK(workspace.generation() == generation);
    const auto invalid = workspace.insert_lane(BoardId{1}, "Bad", LaneId{99});
    CK_CHECK(!invalid && invalid.error.code == ModelErrorCode::InvalidInsertionAnchor);
    CK_CHECK(workspace.snapshot().next_lane_id == 5);
}

CK_TEST(todo_lane_merge_appends_stored_order_and_preserves_target_metadata) {
    WorkspaceSnapshot snapshot = sortable_snapshot();
    snapshot.boards.front().lanes[0].task_ids = {TaskId{1}, TaskId{2}, TaskId{3}};
    snapshot.boards.front().lanes[1].task_ids = {TaskId{4}};
    snapshot.boards.front().lanes[1].color = TodoColor::Yellow;
    snapshot.boards.front().lanes[1].sort = SortMode::Priority;
    TodoWorkspace workspace = workspace_from(std::move(snapshot));
    const auto merged = workspace.merge_lane(LaneId{1}, LaneId{2});
    CK_CHECK(merged && merged.value->changed);
    CK_CHECK(workspace.find_lane(LaneId{1}) == nullptr);
    const Lane* target = workspace.find_lane(LaneId{2});
    CK_CHECK(target != nullptr);
    if (target == nullptr) return;
    CK_CHECK(target->task_ids == std::vector<TaskId>({TaskId{4}, TaskId{1}, TaskId{2}, TaskId{3}}));
    CK_CHECK(target->color == TodoColor::Yellow);
    CK_CHECK(target->sort == SortMode::Priority);
}

CK_TEST(todo_lane_merge_rejects_same_and_cross_board_sources) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    auto result = workspace.merge_lane(LaneId{1}, LaneId{1});
    CK_CHECK(!result && result.error.code == ModelErrorCode::SameSourceAndTarget);
    const auto board = workspace.add_board("project");
    CK_CHECK(board);
    result = workspace.merge_lane(LaneId{1}, LaneId{4});
    CK_CHECK(!result && result.error.code == ModelErrorCode::CrossBoardLaneMerge);
}

CK_TEST(todo_board_add_rename_and_switch_preserve_protected_main) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    const auto added = workspace.add_board("project");
    CK_CHECK(added && *added.value == BoardId{2});
    const Board* project = workspace.find_board(BoardId{2});
    CK_CHECK(project != nullptr);
    if (project == nullptr) return;
    CK_CHECK(project->lanes.size() == 3);
    CK_CHECK(project->lanes[0].id == LaneId{4});
    CK_CHECK(workspace.snapshot().next_lane_id == 7);

    auto mutation = workspace.rename_board(BoardId{2}, "release");
    CK_CHECK(mutation && mutation.value->changed);
    mutation = workspace.switch_board(BoardId{2});
    CK_CHECK(mutation && mutation.value->changed);
    CK_CHECK(workspace.snapshot().last_board_id == BoardId{2});
    mutation = workspace.switch_board(BoardId{2});
    CK_CHECK(mutation && !mutation.value->changed);

    mutation = workspace.rename_board(BoardId{1}, "renamed-main");
    CK_CHECK(!mutation && mutation.error.code == ModelErrorCode::ProtectedMainBoard);
    const auto duplicate = workspace.add_board("release");
    CK_CHECK(!duplicate && duplicate.error.code == ModelErrorCode::DuplicateBoardName);
    CK_CHECK(workspace.snapshot().next_board_id == 3);
}

CK_TEST(todo_board_merge_uses_original_first_title_match_and_exact_order) {
    TodoWorkspace workspace = workspace_from(board_merge_snapshot());
    const auto merged = workspace.merge_board(BoardId{2}, BoardId{1});
    CK_CHECK(merged && merged.value->changed);
    CK_CHECK(workspace.find_board(BoardId{2}) == nullptr);
    CK_CHECK(workspace.snapshot().last_board_id == BoardId{1});
    const Board* main = workspace.find_board(BoardId{1});
    CK_CHECK(main != nullptr);
    if (main == nullptr) return;
    CK_CHECK(main->lanes.size() == 5);
    CK_CHECK(main->lanes[0].id == LaneId{1});
    CK_CHECK(main->lanes[0].task_ids == std::vector<TaskId>({TaskId{1}, TaskId{3}}));
    CK_CHECK(main->lanes[0].color == TodoColor::Green);
    CK_CHECK(main->lanes[0].sort == SortMode::Due);
    CK_CHECK(main->lanes[2].id == LaneId{3});
    CK_CHECK(main->lanes[2].task_ids.empty());
    CK_CHECK(main->lanes[3].id == LaneId{5});
    CK_CHECK(main->lanes[4].id == LaneId{6});
    CK_CHECK(workspace.find_task(TaskId{3}) != nullptr);
    CK_CHECK(workspace.lane_of(TaskId{3}) == LaneId{1});
}

CK_TEST(todo_board_merge_rejects_protected_main_and_identical_target) {
    TodoWorkspace workspace = workspace_from(board_merge_snapshot());
    auto merged = workspace.merge_board(BoardId{1}, BoardId{2});
    CK_CHECK(!merged && merged.error.code == ModelErrorCode::ProtectedMainBoard);
    merged = workspace.merge_board(BoardId{2}, BoardId{2});
    CK_CHECK(!merged && merged.error.code == ModelErrorCode::SameSourceAndTarget);
}

CK_TEST(todo_task_archive_plan_is_complete_and_apply_is_explicit) {
    TodoWorkspace workspace = workspace_from(snapshot_with_task());
    const WorkspaceSnapshot before = workspace.snapshot();
    const auto plan = workspace.prepare_task_archive(TaskId{1}, stamp());
    CK_CHECK(plan);
    if (!plan) return;
    CK_CHECK(plan.value->scope == ArchiveScope::Task);
    CK_CHECK(plan.value->records.size() == 1);
    CK_CHECK(plan.value->records.front().task == one_task());
    CK_CHECK(plan.value->records.front().origin_board_name == "main");
    CK_CHECK(plan.value->records.front().origin_lane_title == "To Do");
    CK_CHECK(plan.value->records.front().archived == stamp());
    CK_CHECK(workspace.snapshot() == before);

    const auto applied = workspace.apply_archive(*plan.value);
    CK_CHECK(applied && applied.value->changed);
    CK_CHECK(workspace.find_task(TaskId{1}) == nullptr);
    CK_CHECK(workspace.find_lane(LaneId{1})->task_ids.empty());
}

CK_TEST(todo_archive_rejects_tampered_stale_and_invalid_plans_without_mutation) {
    TodoWorkspace workspace = workspace_from(snapshot_with_task());
    auto plan = workspace.prepare_task_archive(TaskId{1}, stamp());
    CK_CHECK(plan);
    const WorkspaceSnapshot before = workspace.snapshot();
    plan.value->records.front().origin_lane_title = "tampered";
    auto applied = workspace.apply_archive(*plan.value);
    CK_CHECK(!applied && applied.error.code == ModelErrorCode::InvalidArchivePlan);
    CK_CHECK(workspace.snapshot() == before);

    plan = workspace.prepare_task_archive(TaskId{1}, stamp());
    const auto renamed = workspace.rename_lane(LaneId{1}, "Inbox");
    CK_CHECK(renamed);
    applied = workspace.apply_archive(*plan.value);
    CK_CHECK(!applied && applied.error.code == ModelErrorCode::StaleTransaction);
    CK_CHECK(workspace.find_task(TaskId{1}) != nullptr);

    const auto invalid_stamp = workspace.prepare_task_archive(TaskId{1}, {IsoTimestamp{"invalid"}, "user"});
    CK_CHECK(!invalid_stamp && invalid_stamp.error.code == ModelErrorCode::InvalidTimestamp);
}

CK_TEST(todo_lane_archive_removes_tasks_and_rejects_the_final_lane) {
    TodoWorkspace workspace = workspace_from(snapshot_with_task());
    const auto plan = workspace.prepare_lane_archive(LaneId{1}, stamp());
    CK_CHECK(plan && plan.value->records.size() == 1);
    const auto applied = workspace.apply_archive(*plan.value);
    CK_CHECK(applied && applied.value->changed);
    CK_CHECK(workspace.find_lane(LaneId{1}) == nullptr);
    CK_CHECK(workspace.find_task(TaskId{1}) == nullptr);

    WorkspaceSnapshot single = valid_snapshot();
    single.boards.front().lanes.resize(1);
    TodoWorkspace final_lane = workspace_from(std::move(single));
    const auto rejected = final_lane.prepare_lane_archive(LaneId{1}, stamp());
    CK_CHECK(!rejected && rejected.error.code == ModelErrorCode::FinalLane);
}

CK_TEST(todo_board_archive_removes_all_tasks_and_selected_board_falls_back_to_main) {
    TodoWorkspace workspace = workspace_from(board_merge_snapshot());
    const auto plan = workspace.prepare_board_archive(BoardId{2}, stamp());
    CK_CHECK(plan && plan.value->records.size() == 3);
    CK_CHECK(workspace.find_board(BoardId{2}) != nullptr);
    const auto applied = workspace.apply_archive(*plan.value);
    CK_CHECK(applied && applied.value->changed);
    CK_CHECK(workspace.find_board(BoardId{2}) == nullptr);
    CK_CHECK(workspace.snapshot().last_board_id == BoardId{1});
    CK_CHECK(workspace.find_task(TaskId{3}) == nullptr);
    CK_CHECK(workspace.find_task(TaskId{4}) == nullptr);
    CK_CHECK(workspace.find_task(TaskId{5}) == nullptr);
    CK_CHECK(workspace.find_task(TaskId{1}) != nullptr);

    const auto protected_main = workspace.prepare_board_archive(BoardId{1}, stamp());
    CK_CHECK(!protected_main && protected_main.error.code == ModelErrorCode::ProtectedMainBoard);
}

CK_TEST(todo_empty_lane_and_board_archive_plans_apply_without_task_records) {
    TodoWorkspace workspace = TodoWorkspace::empty();
    const auto lane_plan = workspace.prepare_lane_archive(LaneId{3}, stamp());
    CK_CHECK(lane_plan && lane_plan.value->records.empty());
    CK_CHECK(workspace.apply_archive(*lane_plan.value));
    CK_CHECK(workspace.find_lane(LaneId{3}) == nullptr);

    CK_CHECK(workspace.add_board("empty"));
    const auto board_plan = workspace.prepare_board_archive(BoardId{2}, stamp());
    CK_CHECK(board_plan && board_plan.value->records.empty());
    CK_CHECK(workspace.apply_archive(*board_plan.value));
    CK_CHECK(workspace.find_board(BoardId{2}) == nullptr);
}
