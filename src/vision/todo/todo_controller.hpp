// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <utility>

#include "calendar_clock.hpp"
#include "todo_repository.hpp"

namespace ck::todo {

enum class ControllerErrorCode {
    None,
    NotOpen,
    AlreadyOpen,
    InvalidIdentity,
    ClockFailure,
    ModelFailure,
    RepositoryFailure,
    Conflict,
};

struct ControllerError {
    ControllerErrorCode code = ControllerErrorCode::None;
    ModelErrorCode model_code = ModelErrorCode::None;
    RepositoryErrorCode repository_code = RepositoryErrorCode::None;
    std::string diagnostic;
    friend bool operator==(const ControllerError&, const ControllerError&) = default;
};

template <class T>
struct ControllerResult {
    std::optional<T> value;
    ControllerError error;

    explicit operator bool() const noexcept { return value.has_value(); }

    static ControllerResult success(T result) {
        ControllerResult out;
        out.value.emplace(std::move(result));
        return out;
    }

    static ControllerResult failure(ControllerError error) {
        ControllerResult out;
        out.error = std::move(error);
        return out;
    }
};

enum class WorkspaceOpenState { Missing, Loaded };
enum class InitialWorkspace { Empty, Guided };

class TodoController {
public:
    TodoController(TodoRepository& repository, CalendarClock& clock, std::string identity);

    ControllerResult<WorkspaceOpenState> open();
    ControllerResult<Mutation> create(InitialWorkspace initial);
    ControllerResult<Mutation> reload_if_changed();

    bool is_open() const noexcept { return workspace_.has_value(); }
    const TodoWorkspace* workspace() const noexcept { return workspace_ ? &*workspace_ : nullptr; }
    const RepositoryRevision& revision() const noexcept { return revision_; }
    const std::string& identity() const noexcept { return identity_; }

    ControllerResult<TaskId> add_task(LaneId lane_id,
                                      TaskDraft draft,
                                      std::optional<TaskId> before = std::nullopt);
    ControllerResult<Mutation> edit_task(TaskId task_id, TaskDraft draft);
    ControllerResult<Mutation> delete_task(TaskId task_id);

    ControllerResult<PendingTaskMove> begin_task_move(TaskId task_id) const;
    ControllerResult<PendingTaskMove> stage_task_move(const PendingTaskMove& move,
                                                      LaneId target_lane_id,
                                                      std::optional<TaskId> before) const;
    ControllerResult<Mutation> commit_task_move(const PendingTaskMove& move);

    ControllerResult<LaneId> insert_lane(BoardId board_id,
                                         std::string title,
                                         std::optional<LaneId> before = std::nullopt);
    ControllerResult<Mutation> rename_lane(LaneId lane_id, std::string title);
    ControllerResult<Mutation> set_lane_color(LaneId lane_id, std::optional<TodoColor> color);
    ControllerResult<Mutation> set_lane_sort(LaneId lane_id, SortMode sort);
    ControllerResult<Mutation> merge_lane(LaneId source_lane_id, LaneId target_lane_id);

    ControllerResult<BoardId> add_board(std::string name);
    ControllerResult<Mutation> rename_board(BoardId board_id, std::string name);
    ControllerResult<Mutation> switch_board(BoardId board_id);
    ControllerResult<Mutation> merge_board(BoardId source_board_id, BoardId target_board_id);

    ControllerResult<Mutation> archive_task(TaskId task_id);
    ControllerResult<Mutation> archive_lane(LaneId lane_id);
    ControllerResult<Mutation> archive_board(BoardId board_id);

private:
    ControllerResult<CalendarReading> read_calendar() const;
    ControllerResult<CalendarReading> mutation_reading() const;
    ControllerResult<Mutation> complete_archive(ModelResult<ArchivePlan> plan, const CalendarReading& reading);

    // ckvision-doc: todo-controller-commit
    template <class T>
    ControllerResult<T> complete(TodoWorkspace candidate,
                                 ModelResult<T> model_result,
                                 const CalendarReading& reading) {
        if (!model_result) return ControllerResult<T>::failure(model_error(std::move(model_result.error)));
        const TodoCommitResult committed = repository_.commit(revision_, candidate, reading.local_date);
        if (!committed) return ControllerResult<T>::failure(repository_error(committed.error));
        workspace_ = std::move(candidate);
        revision_ = committed.value->revision;
        return ControllerResult<T>::success(std::move(*model_result.value));
    }
    // ckvision-doc-end: todo-controller-commit

    static ControllerError model_error(ModelError error);
    static ControllerError repository_error(RepositoryError error);

    TodoRepository& repository_;
    CalendarClock& clock_;
    std::string identity_;
    bool opened_ = false;
    std::optional<TodoWorkspace> workspace_;
    RepositoryRevision revision_;
};

}  // namespace ck::todo
