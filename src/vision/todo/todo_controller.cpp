// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_controller.hpp"

#include <string_view>
#include <utility>

#include "cvision/core/utf8.hpp"

namespace ck::todo {
namespace {

bool valid_identity(std::string_view identity) noexcept {
    if (identity.empty() || identity.size() > TodoLimits::max_identity_bytes || !ckv::utf8::is_valid(identity)) {
        return false;
    }
    for (const unsigned char byte : identity) {
        if (byte < 0x20U || byte == 0x7FU) return false;
    }
    return true;
}

template <class T>
ControllerResult<T> not_open() {
    return ControllerResult<T>::failure(
        ControllerError{ControllerErrorCode::NotOpen, ModelErrorCode::None, RepositoryErrorCode::None,
                        "TODO workspace is not open"});
}

}  // namespace

TodoController::TodoController(TodoRepository& repository, CalendarClock& clock, std::string identity)
    : repository_(repository), clock_(clock), identity_(std::move(identity)) {}

ControllerResult<WorkspaceOpenState> TodoController::open() {
    if (opened_) {
        return ControllerResult<WorkspaceOpenState>::failure(
            ControllerError{ControllerErrorCode::AlreadyOpen, ModelErrorCode::None, RepositoryErrorCode::None,
                            "TODO workspace is already open"});
    }
    TodoLoadResult loaded = repository_.load();
    if (!loaded) {
        if (loaded.error.code == RepositoryErrorCode::Missing) {
            opened_ = true;
            revision_ = {};
            return ControllerResult<WorkspaceOpenState>::success(WorkspaceOpenState::Missing);
        }
        return ControllerResult<WorkspaceOpenState>::failure(repository_error(std::move(loaded.error)));
    }
    workspace_ = std::move(loaded.value->workspace);
    revision_ = std::move(loaded.value->revision);
    opened_ = true;
    return ControllerResult<WorkspaceOpenState>::success(WorkspaceOpenState::Loaded);
}

ControllerResult<Mutation> TodoController::create(InitialWorkspace initial) {
    if (!opened_) return not_open<Mutation>();
    if (workspace_) {
        return ControllerResult<Mutation>::failure(
            ControllerError{ControllerErrorCode::AlreadyOpen, ModelErrorCode::None, RepositoryErrorCode::None,
                            "TODO workspace is already open"});
    }
    const auto reading = read_calendar();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    ModelResult<TodoWorkspace> initial_workspace = initial == InitialWorkspace::Guided
                                                       ? TodoWorkspace::guided(
                                                             AuditStamp{reading.value->utc_timestamp, identity_})
                                                       : ModelResult<TodoWorkspace>::success(TodoWorkspace::empty());
    if (!initial_workspace) {
        return ControllerResult<Mutation>::failure(model_error(std::move(initial_workspace.error)));
    }
    TodoCommitResult committed = repository_.commit(revision_, *initial_workspace.value, reading.value->local_date);
    if (!committed) return ControllerResult<Mutation>::failure(repository_error(std::move(committed.error)));
    workspace_ = std::move(*initial_workspace.value);
    revision_ = std::move(committed.value->revision);
    return ControllerResult<Mutation>::success(Mutation{committed.value->changed});
}

ControllerResult<Mutation> TodoController::reload_if_changed() {
    if (!workspace_) return not_open<Mutation>();
    TodoRevisionResult current = repository_.revision();
    if (!current) return ControllerResult<Mutation>::failure(repository_error(std::move(current.error)));
    if (*current.value == revision_) return ControllerResult<Mutation>::success(Mutation{});
    TodoLoadResult loaded = repository_.load();
    if (!loaded) return ControllerResult<Mutation>::failure(repository_error(std::move(loaded.error)));
    workspace_ = std::move(loaded.value->workspace);
    revision_ = std::move(loaded.value->revision);
    return ControllerResult<Mutation>::success(Mutation{true});
}

ControllerResult<TaskId> TodoController::add_task(LaneId lane_id,
                                                  TaskDraft draft,
                                                  std::optional<TaskId> before) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<TaskId>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.add_task(lane_id, std::move(draft),
                                     AuditStamp{reading.value->utc_timestamp, identity_}, before);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::edit_task(TaskId task_id, TaskDraft draft) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.edit_task(task_id, std::move(draft),
                                      AuditStamp{reading.value->utc_timestamp, identity_});
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::delete_task(TaskId task_id) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.delete_task(task_id);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<PendingTaskMove> TodoController::begin_task_move(TaskId task_id) const {
    if (!workspace_) return not_open<PendingTaskMove>();
    auto result = workspace_->begin_task_move(task_id);
    if (!result) return ControllerResult<PendingTaskMove>::failure(model_error(std::move(result.error)));
    return ControllerResult<PendingTaskMove>::success(std::move(*result.value));
}

ControllerResult<PendingTaskMove> TodoController::stage_task_move(const PendingTaskMove& move,
                                                                  LaneId target_lane_id,
                                                                  std::optional<TaskId> before) const {
    if (!workspace_) return not_open<PendingTaskMove>();
    auto result = workspace_->stage_task_move(move, target_lane_id, before);
    if (!result) return ControllerResult<PendingTaskMove>::failure(model_error(std::move(result.error)));
    return ControllerResult<PendingTaskMove>::success(std::move(*result.value));
}

ControllerResult<Mutation> TodoController::commit_task_move(const PendingTaskMove& move) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.commit_task_move(move);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<LaneId> TodoController::insert_lane(BoardId board_id,
                                                     std::string title,
                                                     std::optional<LaneId> before) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<LaneId>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.insert_lane(board_id, std::move(title), before);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::rename_lane(LaneId lane_id, std::string title) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.rename_lane(lane_id, std::move(title));
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::set_lane_color(LaneId lane_id, std::optional<TodoColor> color) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.set_lane_color(lane_id, color);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::set_lane_sort(LaneId lane_id, SortMode sort) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.set_lane_sort(lane_id, sort);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::merge_lane(LaneId source_lane_id, LaneId target_lane_id) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.merge_lane(source_lane_id, target_lane_id);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<BoardId> TodoController::add_board(std::string name) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<BoardId>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.add_board(std::move(name));
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::rename_board(BoardId board_id, std::string name) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.rename_board(board_id, std::move(name));
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::switch_board(BoardId board_id) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.switch_board(board_id);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::merge_board(BoardId source_board_id, BoardId target_board_id) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    TodoWorkspace candidate = *workspace_;
    auto result = candidate.merge_board(source_board_id, target_board_id);
    return complete(std::move(candidate), std::move(result), *reading.value);
}

ControllerResult<Mutation> TodoController::archive_task(TaskId task_id) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    return complete_archive(workspace_->prepare_task_archive(
                                task_id, AuditStamp{reading.value->utc_timestamp, identity_}),
                            *reading.value);
}

ControllerResult<Mutation> TodoController::archive_lane(LaneId lane_id) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    return complete_archive(workspace_->prepare_lane_archive(
                                lane_id, AuditStamp{reading.value->utc_timestamp, identity_}),
                            *reading.value);
}

ControllerResult<Mutation> TodoController::archive_board(BoardId board_id) {
    const auto reading = mutation_reading();
    if (!reading) return ControllerResult<Mutation>::failure(reading.error);
    return complete_archive(workspace_->prepare_board_archive(
                                board_id, AuditStamp{reading.value->utc_timestamp, identity_}),
                            *reading.value);
}

ControllerResult<CalendarReading> TodoController::read_calendar() const {
    if (!valid_identity(identity_)) {
        return ControllerResult<CalendarReading>::failure(
            ControllerError{ControllerErrorCode::InvalidIdentity, ModelErrorCode::None, RepositoryErrorCode::None,
                            "TODO identity must be non-empty valid UTF-8 without controls"});
    }
    CalendarReadResult reading = clock_.read();
    if (!reading) {
        return ControllerResult<CalendarReading>::failure(
            ControllerError{ControllerErrorCode::ClockFailure, ModelErrorCode::None, RepositoryErrorCode::None,
                            std::move(reading.diagnostic)});
    }
    if (!is_valid(reading.value->utc_timestamp) || !is_valid(reading.value->local_date) ||
        !is_valid(reading.value->local_time)) {
        return ControllerResult<CalendarReading>::failure(
            ControllerError{ControllerErrorCode::ClockFailure, ModelErrorCode::None, RepositoryErrorCode::None,
                            "calendar clock returned a non-canonical reading"});
    }
    return ControllerResult<CalendarReading>::success(std::move(*reading.value));
}

ControllerResult<CalendarReading> TodoController::mutation_reading() const {
    if (!workspace_) return not_open<CalendarReading>();
    return read_calendar();
}

ControllerResult<Mutation> TodoController::complete_archive(ModelResult<ArchivePlan> plan,
                                                             const CalendarReading& reading) {
    if (!plan) return ControllerResult<Mutation>::failure(model_error(std::move(plan.error)));
    for (const ArchivedTask& record : plan.value->records) {
        TodoArchiveResult stored = repository_.store_archive(record);
        if (!stored) return ControllerResult<Mutation>::failure(repository_error(std::move(stored.error)));
    }
    TodoWorkspace candidate = *workspace_;
    auto applied = candidate.apply_archive(*plan.value);
    return complete(std::move(candidate), std::move(applied), reading);
}

ControllerError TodoController::model_error(ModelError error) {
    return ControllerError{ControllerErrorCode::ModelFailure, error.code, RepositoryErrorCode::None,
                           std::move(error.diagnostic)};
}

ControllerError TodoController::repository_error(RepositoryError error) {
    const ControllerErrorCode code = error.code == RepositoryErrorCode::Conflict ||
                                             error.code == RepositoryErrorCode::ArchiveConflict
                                         ? ControllerErrorCode::Conflict
                                         : ControllerErrorCode::RepositoryFailure;
    return ControllerError{code, ModelErrorCode::None, error.code, std::move(error.diagnostic)};
}

}  // namespace ck::todo
