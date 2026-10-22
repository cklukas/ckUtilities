// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_controller.hpp"
#include "memory_todo_repository.hpp"

#include <string>
#include <utility>

#include "cvision/testing/cktest.hpp"

namespace {

using namespace ck::todo;

CalendarReading controller_reading() {
    return {IsoTimestamp{"2026-08-25T12:00:00Z"}, IsoDate{"2026-08-25"}, IsoTime{"14:30"}};
}

TaskDraft draft(std::string title) {
    TaskDraft value;
    value.title = std::move(title);
    return value;
}

TodoWorkspace workspace_with_task() {
    TodoWorkspace workspace = TodoWorkspace::empty();
    const auto result = workspace.add_task(
        LaneId{1}, draft("Existing"), {IsoTimestamp{"2026-08-25T11:00:00Z"}, "fixture"});
    if (!result) return TodoWorkspace::empty();
    return workspace;
}

}  // namespace

CK_TEST(todo_controller_requires_open_before_create_or_mutation) {
    MemoryTodoRepository repository;
    FixedCalendarClock clock(controller_reading());
    TodoController controller(repository, clock, "operator");
    CK_CHECK(controller.create(InitialWorkspace::Empty).error.code == ControllerErrorCode::NotOpen);
    CK_CHECK(controller.add_task(LaneId{1}, draft("No workspace")).error.code ==
             ControllerErrorCode::NotOpen);
    CK_CHECK(!controller.is_open());
}

CK_TEST(todo_controller_opens_existing_or_reports_missing_then_creates) {
    MemoryTodoRepository missing_repository;
    FixedCalendarClock clock(controller_reading());
    TodoController missing(missing_repository, clock, "operator");
    const auto open_missing = missing.open();
    CK_CHECK(open_missing && *open_missing.value == WorkspaceOpenState::Missing);
    const auto created = missing.create(InitialWorkspace::Guided);
    CK_CHECK(created && created.value->changed);
    CK_CHECK(missing.is_open());
    CK_CHECK(missing.workspace()->snapshot().tasks.size() == 3);
    CK_CHECK(missing_repository.load().value->workspace.snapshot() == missing.workspace()->snapshot());
    CK_CHECK(missing.open().error.code == ControllerErrorCode::AlreadyOpen);

    MemoryTodoRepository existing_repository(workspace_with_task());
    TodoController existing(existing_repository, clock, "operator");
    const auto open_existing = existing.open();
    CK_CHECK(open_existing && *open_existing.value == WorkspaceOpenState::Loaded);
    CK_CHECK(existing.workspace()->find_task(TaskId{1}) != nullptr);
}

CK_TEST(todo_controller_rejects_bad_identity_and_clock_without_writes) {
    MemoryTodoRepository repository;
    FixedCalendarClock clock(controller_reading());
    TodoController bad_identity(repository, clock, "bad\nidentity");
    CK_CHECK(bad_identity.open().value == WorkspaceOpenState::Missing);
    CK_CHECK(bad_identity.create(InitialWorkspace::Empty).error.code == ControllerErrorCode::InvalidIdentity);
    CK_CHECK(!repository.revision().value->exists());

    TodoController failed_clock(repository, clock, "operator");
    CK_CHECK(failed_clock.open().value == WorkspaceOpenState::Missing);
    clock.fail("time source failed");
    const auto failed = failed_clock.create(InitialWorkspace::Empty);
    CK_CHECK(!failed && failed.error.code == ControllerErrorCode::ClockFailure);
    CK_CHECK(failed.error.diagnostic == "time source failed");
    CK_CHECK(!repository.revision().value->exists());

    clock.set({IsoTimestamp{"not-a-time"}, IsoDate{"2026-08-25"}, IsoTime{"14:30"}});
    const auto malformed = failed_clock.create(InitialWorkspace::Empty);
    CK_CHECK(!malformed && malformed.error.code == ControllerErrorCode::ClockFailure);
    CK_CHECK(!repository.revision().value->exists());

    clock.set({IsoTimestamp{"2026-08-25T12:00:00Z"}, IsoDate{"2026-08-25"}, IsoTime{"25:00"}});
    const auto malformed_local_time = failed_clock.create(InitialWorkspace::Empty);
    CK_CHECK(!malformed_local_time && malformed_local_time.error.code == ControllerErrorCode::ClockFailure);
    CK_CHECK(!repository.revision().value->exists());
}

CK_TEST(todo_controller_commits_task_lane_board_and_move_operations) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    FixedCalendarClock clock(controller_reading());
    TodoController controller(repository, clock, "operator");
    CK_CHECK(controller.open());

    const auto task = controller.add_task(LaneId{1}, draft("First"));
    CK_CHECK(task && *task.value == TaskId{1});
    TaskDraft edited = draft("Edited");
    edited.details = "Durable details";
    CK_CHECK(controller.edit_task(*task.value, edited));
    auto move = controller.begin_task_move(*task.value);
    CK_CHECK(move);
    move = controller.stage_task_move(*move.value, LaneId{2}, std::nullopt);
    CK_CHECK(move && controller.commit_task_move(*move.value));
    CK_CHECK(controller.workspace()->lane_of(*task.value) == LaneId{2});

    const auto lane = controller.insert_lane(BoardId{1}, "Review");
    CK_CHECK(lane);
    CK_CHECK(controller.rename_lane(*lane.value, "Ready"));
    CK_CHECK(controller.set_lane_color(*lane.value, TodoColor::DarkBlue));
    CK_CHECK(controller.set_lane_sort(*lane.value, SortMode::Due));
    const auto board = controller.add_board("work");
    CK_CHECK(board);
    CK_CHECK(controller.rename_board(*board.value, "project"));
    CK_CHECK(controller.switch_board(*board.value));
    CK_CHECK(controller.merge_board(*board.value, BoardId{1}));

    const auto persisted = repository.load();
    CK_CHECK(persisted.value->workspace.snapshot() == controller.workspace()->snapshot());
    CK_CHECK(persisted.value->workspace.find_task(TaskId{1})->title == "Edited");
}

CK_TEST(todo_controller_model_failure_does_not_touch_repository_or_local_state) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    FixedCalendarClock clock(controller_reading());
    TodoController controller(repository, clock, "operator");
    CK_CHECK(controller.open());
    const auto before_revision = controller.revision();
    const auto before = controller.workspace()->snapshot();
    const auto rejected = controller.rename_board(BoardId{1}, "renamed-main");
    CK_CHECK(!rejected && rejected.error.code == ControllerErrorCode::ModelFailure);
    CK_CHECK(rejected.error.model_code == ModelErrorCode::ProtectedMainBoard);
    CK_CHECK(controller.revision() == before_revision);
    CK_CHECK(controller.workspace()->snapshot() == before);
}

CK_TEST(todo_controller_primary_failure_never_publishes_candidate_locally) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    FixedCalendarClock clock(controller_reading());
    TodoController controller(repository, clock, "operator");
    CK_CHECK(controller.open());
    const auto before_revision = controller.revision();
    repository.fail_next(MemoryRepositoryFailure::PrimaryWrite);
    const auto failed = controller.add_task(LaneId{1}, draft("Unsaved"));
    CK_CHECK(!failed && failed.error.code == ControllerErrorCode::RepositoryFailure);
    CK_CHECK(controller.revision() == before_revision);
    CK_CHECK(controller.workspace()->find_task(TaskId{1}) == nullptr);
    CK_CHECK(repository.load().value->workspace.find_task(TaskId{1}) == nullptr);
}

CK_TEST(todo_controller_conflict_keeps_local_snapshot_then_reload_adopts_remote) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    FixedCalendarClock clock(controller_reading());
    TodoController first(repository, clock, "first");
    TodoController second(repository, clock, "second");
    CK_CHECK(first.open() && second.open());
    CK_CHECK(first.add_task(LaneId{1}, draft("Remote")));
    const auto stale_revision = second.revision();
    const auto conflict = second.add_task(LaneId{1}, draft("Local"));
    CK_CHECK(!conflict && conflict.error.code == ControllerErrorCode::Conflict);
    CK_CHECK(conflict.error.repository_code == RepositoryErrorCode::Conflict);
    CK_CHECK(second.revision() == stale_revision);
    CK_CHECK(second.workspace()->find_task(TaskId{1}) == nullptr);
    const auto reloaded = second.reload_if_changed();
    CK_CHECK(reloaded && reloaded.value->changed);
    CK_CHECK(second.workspace()->find_task(TaskId{1})->title == "Remote");
    const auto unchanged = second.reload_if_changed();
    CK_CHECK(unchanged && !unchanged.value->changed);
}

CK_TEST(todo_controller_archive_write_failure_leaves_task_live) {
    MemoryTodoRepository repository(workspace_with_task());
    FixedCalendarClock clock(controller_reading());
    TodoController controller(repository, clock, "operator");
    CK_CHECK(controller.open());
    repository.fail_next(MemoryRepositoryFailure::ArchiveWrite);
    const auto failed = controller.archive_task(TaskId{1});
    CK_CHECK(!failed && failed.error.repository_code == RepositoryErrorCode::IoFailure);
    CK_CHECK(controller.workspace()->find_task(TaskId{1}) != nullptr);
    CK_CHECK(repository.load().value->workspace.find_task(TaskId{1}) != nullptr);
    CK_CHECK(repository.archives().empty());
}

CK_TEST(todo_controller_archive_is_durable_before_removal_and_retry_is_idempotent) {
    MemoryTodoRepository repository(workspace_with_task());
    FixedCalendarClock clock(controller_reading());
    TodoController controller(repository, clock, "operator");
    CK_CHECK(controller.open());
    repository.fail_next(MemoryRepositoryFailure::PrimaryWrite);
    const auto failed = controller.archive_task(TaskId{1});
    CK_CHECK(!failed && failed.error.repository_code == RepositoryErrorCode::IoFailure);
    CK_CHECK(repository.archives().size() == 1);
    CK_CHECK(controller.workspace()->find_task(TaskId{1}) != nullptr);
    CK_CHECK(repository.load().value->workspace.find_task(TaskId{1}) != nullptr);

    const auto retried = controller.archive_task(TaskId{1});
    CK_CHECK(retried && retried.value->changed);
    CK_CHECK(repository.archives().size() == 1);
    CK_CHECK(controller.workspace()->find_task(TaskId{1}) == nullptr);
    CK_CHECK(repository.load().value->workspace.find_task(TaskId{1}) == nullptr);
}

CK_TEST(todo_controller_can_archive_empty_lane_and_permanently_delete_a_task) {
    MemoryTodoRepository repository(workspace_with_task());
    FixedCalendarClock clock(controller_reading());
    TodoController controller(repository, clock, "operator");
    CK_CHECK(controller.open());
    const auto lane = controller.insert_lane(BoardId{1}, "Temporary");
    CK_CHECK(lane && controller.archive_lane(*lane.value));
    CK_CHECK(controller.workspace()->find_lane(*lane.value) == nullptr);
    CK_CHECK(repository.archives().empty());
    CK_CHECK(controller.delete_task(TaskId{1}));
    CK_CHECK(repository.load().value->workspace.find_task(TaskId{1}) == nullptr);
}

CK_TEST(todo_controller_reload_reports_repository_failures) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    FixedCalendarClock clock(controller_reading());
    TodoController controller(repository, clock, "operator");
    CK_CHECK(controller.open());
    repository.fail_next(MemoryRepositoryFailure::Revision);
    const auto failed = controller.reload_if_changed();
    CK_CHECK(!failed && failed.error.code == ControllerErrorCode::RepositoryFailure);
    CK_CHECK(failed.error.repository_code == RepositoryErrorCode::IoFailure);
}
