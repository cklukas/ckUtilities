// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_model.hpp"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "cvision/core/utf8.hpp"

namespace ck::todo {
namespace {

template <class T>
ModelResult<T> failure(ModelErrorCode code, std::string diagnostic) {
    return ModelResult<T>::failure(code, std::move(diagnostic));
}

bool contains_control(std::string_view text, bool allow_line_breaks) noexcept {
    for (const unsigned char byte : text) {
        if (byte == 0x7FU) return true;
        if (byte >= 0x20U) continue;
        if (allow_line_breaks && (byte == '\t' || byte == '\r' || byte == '\n')) continue;
        return true;
    }
    return false;
}

std::optional<ModelError> validate_text(std::string_view text,
                                        std::size_t limit,
                                        bool required,
                                        bool allow_line_breaks,
                                        std::string_view field) {
    if (required && text.empty()) {
        return ModelError{ModelErrorCode::EmptyText, std::string(field) + " must not be empty"};
    }
    if (text.size() > limit) {
        return ModelError{ModelErrorCode::TextLimit, std::string(field) + " exceeds its byte limit"};
    }
    if (!ckv::utf8::is_valid(text)) {
        return ModelError{ModelErrorCode::InvalidUtf8, std::string(field) + " is not valid UTF-8"};
    }
    if (contains_control(text, allow_line_breaks)) {
        return ModelError{ModelErrorCode::InvalidText, std::string(field) + " contains a forbidden control"};
    }
    return std::nullopt;
}

bool all_digits(std::string_view value, std::size_t begin, std::size_t end) noexcept {
    for (std::size_t index = begin; index < end; ++index) {
        if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}

int decimal(std::string_view value, std::size_t begin, std::size_t end) noexcept {
    int result = 0;
    for (std::size_t index = begin; index < end; ++index) result = result * 10 + (value[index] - '0');
    return result;
}

bool leap_year(int year) noexcept { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

bool valid_date(std::string_view value) noexcept {
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    if (!all_digits(value, 0, 4) || !all_digits(value, 5, 7) || !all_digits(value, 8, 10)) return false;
    const int year = decimal(value, 0, 4);
    const int month = decimal(value, 5, 7);
    const int day = decimal(value, 8, 10);
    if (year == 0 || month < 1 || month > 12 || day < 1) return false;
    constexpr std::array<int, 12> month_days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const int limit = month == 2 && leap_year(year) ? 29 : month_days[static_cast<std::size_t>(month - 1)];
    return day <= limit;
}

bool valid_timestamp(std::string_view value) noexcept {
    if (value.size() != 20 || value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
        value[19] != 'Z') {
        return false;
    }
    if (!valid_date(value.substr(0, 10)) || !all_digits(value, 11, 13) || !all_digits(value, 14, 16) ||
        !all_digits(value, 17, 19)) {
        return false;
    }
    return decimal(value, 11, 13) <= 23 && decimal(value, 14, 16) <= 59 && decimal(value, 17, 19) <= 59;
}

bool valid_time(std::string_view value) noexcept {
    return value.size() == 5 && value[2] == ':' && all_digits(value, 0, 2) &&
           all_digits(value, 3, 5) && decimal(value, 0, 2) <= 23 &&
           decimal(value, 3, 5) <= 59;
}

bool valid_priority(Priority value) noexcept {
    return value >= Priority::High && value <= Priority::Idle;
}

bool valid_sort(SortMode value) noexcept {
    return value >= SortMode::Manual && value <= SortMode::Priority;
}

bool valid_color(TodoColor value) noexcept {
    return value >= TodoColor::Black && value <= TodoColor::White;
}

ModelResult<WorkspaceSnapshot> validate_snapshot(WorkspaceSnapshot snapshot) {
    if (snapshot.schema_version != todo_schema_version) {
        return failure<WorkspaceSnapshot>(ModelErrorCode::UnknownSchema, "unsupported TODO schema version");
    }
    if (snapshot.boards.empty() || snapshot.boards.size() > TodoLimits::max_boards) {
        return failure<WorkspaceSnapshot>(ModelErrorCode::WorkspaceLimit, "workspace must contain 1 to 64 boards");
    }
    if (snapshot.tasks.size() > TodoLimits::max_tasks) {
        return failure<WorkspaceSnapshot>(ModelErrorCode::WorkspaceLimit, "workspace task limit exceeded");
    }

    std::unordered_set<std::uint64_t> board_ids;
    std::unordered_set<std::uint64_t> lane_ids;
    std::unordered_set<std::uint64_t> task_ids;
    std::unordered_set<std::string> board_names;
    std::unordered_map<std::uint64_t, std::size_t> memberships;
    std::uint64_t known_string_bytes = 0;
    std::uint64_t max_board_id = 0;
    std::uint64_t max_lane_id = 0;
    std::uint64_t max_task_id = 0;
    std::size_t lane_count = 0;
    bool has_last_board = false;
    bool has_main = false;

    for (const Task& task : snapshot.tasks) {
        if (task.id.value == 0) return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidId, "task id is zero");
        if (!task_ids.insert(task.id.value).second) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::DuplicateId, "duplicate task id");
        }
        max_task_id = std::max(max_task_id, task.id.value);
        if (const auto error = validate_text(task.title, TodoLimits::max_title_bytes, true, false, "task title")) {
            return failure<WorkspaceSnapshot>(error->code, error->diagnostic);
        }
        if (const auto error =
                validate_text(task.details, TodoLimits::max_details_bytes, false, false, "task details")) {
            return failure<WorkspaceSnapshot>(error->code, error->diagnostic);
        }
        if (const auto error = validate_text(task.note, TodoLimits::max_note_bytes, false, true, "task note")) {
            return failure<WorkspaceSnapshot>(error->code, error->diagnostic);
        }
        if (!valid_priority(task.priority) || (task.color && !valid_color(*task.color))) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidValue, "task enum value is invalid");
        }
        if (task.due_date && !valid_date(task.due_date->value)) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidDate, "task due date is not YYYY-MM-DD");
        }
        if (task.due_time && !valid_time(task.due_time->value)) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidValue, "task due time is not HH:MM");
        }
        if (task.due_time && !task.due_date) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidValue, "task due time requires a due date");
        }
        if (!valid_timestamp(task.created_at.value) || !valid_timestamp(task.modified_at.value)) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidTimestamp, "task audit timestamp is invalid");
        }
        if (task.modified_at.value < task.created_at.value) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::TimestampRegression,
                                              "task modification precedes creation");
        }
        if (const auto error =
                validate_text(task.created_by, TodoLimits::max_identity_bytes, true, false, "creator identity")) {
            return failure<WorkspaceSnapshot>(error->code, error->diagnostic);
        }
        if (const auto error =
                validate_text(task.modified_by, TodoLimits::max_identity_bytes, true, false, "modifier identity")) {
            return failure<WorkspaceSnapshot>(error->code, error->diagnostic);
        }
        known_string_bytes += task.title.size() + task.details.size() + task.note.size() +
                              task.created_at.value.size() + task.created_by.size() + task.modified_at.value.size() +
                              task.modified_by.size() + (task.due_date ? task.due_date->value.size() : 0U) +
                              (task.due_time ? task.due_time->value.size() : 0U);
    }

    for (const Board& board : snapshot.boards) {
        if (board.id.value == 0) return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidId, "board id is zero");
        if (!board_ids.insert(board.id.value).second) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::DuplicateId, "duplicate board id");
        }
        max_board_id = std::max(max_board_id, board.id.value);
        if (const auto error = validate_text(board.name, TodoLimits::max_name_bytes, true, false, "board name")) {
            return failure<WorkspaceSnapshot>(error->code, error->diagnostic);
        }
        if (!board_names.insert(board.name).second) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::DuplicateBoardName, "duplicate board name");
        }
        if (board.id == BoardId{1}) {
            has_main = board.name == "main";
            if (!has_main) {
                return failure<WorkspaceSnapshot>(ModelErrorCode::ProtectedMainBoard,
                                                  "board 1 must be named main");
            }
        }
        if (board.id == snapshot.last_board_id) has_last_board = true;
        if (board.lanes.empty() || board.lanes.size() > TodoLimits::max_lanes_per_board) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::WorkspaceLimit,
                                              "each board must contain 1 to 64 lanes");
        }
        lane_count += board.lanes.size();
        known_string_bytes += board.name.size();
        for (const Lane& lane : board.lanes) {
            if (lane.id.value == 0) return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidId, "lane id is zero");
            if (!lane_ids.insert(lane.id.value).second) {
                return failure<WorkspaceSnapshot>(ModelErrorCode::DuplicateId, "duplicate lane id");
            }
            max_lane_id = std::max(max_lane_id, lane.id.value);
            if (const auto error = validate_text(lane.title, TodoLimits::max_name_bytes, true, false, "lane title")) {
                return failure<WorkspaceSnapshot>(error->code, error->diagnostic);
            }
            if (!valid_sort(lane.sort) || (lane.color && !valid_color(*lane.color))) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidValue, "lane enum value is invalid");
            }
            known_string_bytes += lane.title.size();
            for (TaskId task_id : lane.task_ids) {
                if (!task_ids.contains(task_id.value)) {
                    return failure<WorkspaceSnapshot>(ModelErrorCode::DanglingTask, "lane references an unknown task");
                }
                if (++memberships[task_id.value] != 1U) {
                    return failure<WorkspaceSnapshot>(ModelErrorCode::DuplicateTaskMembership,
                                                      "task belongs to more than one lane");
                }
            }
        }
    }

    if (!has_main) {
        return failure<WorkspaceSnapshot>(ModelErrorCode::ProtectedMainBoard, "workspace has no main board");
    }
    if (!has_last_board) {
        return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidLastBoard, "last board does not exist");
    }
    if (lane_count > TodoLimits::max_lanes) {
        return failure<WorkspaceSnapshot>(ModelErrorCode::WorkspaceLimit, "workspace lane limit exceeded");
    }
    for (std::uint64_t task_id : task_ids) {
        if (!memberships.contains(task_id)) {
            return failure<WorkspaceSnapshot>(ModelErrorCode::UnownedTask, "task does not belong to a lane");
        }
    }
    if (known_string_bytes > TodoLimits::max_known_string_bytes) {
        return failure<WorkspaceSnapshot>(ModelErrorCode::WorkspaceLimit,
                                          "aggregate known string byte limit exceeded");
    }
    if (max_board_id == std::numeric_limits<std::uint64_t>::max() ||
        snapshot.next_board_id <= max_board_id || max_lane_id == std::numeric_limits<std::uint64_t>::max() ||
        snapshot.next_lane_id <= max_lane_id || max_task_id == std::numeric_limits<std::uint64_t>::max() ||
        snapshot.next_task_id <= max_task_id) {
        return failure<WorkspaceSnapshot>(ModelErrorCode::InvalidCounter,
                                          "next id counters must exceed all stored ids");
    }
    std::sort(snapshot.tasks.begin(), snapshot.tasks.end(),
              [](const Task& left, const Task& right) { return left.id < right.id; });
    return ModelResult<WorkspaceSnapshot>::success(std::move(snapshot));
}

Task guided_task(TaskId id, std::string title, std::string details, const AuditStamp& stamp) {
    return Task{id,
                std::move(title),
                std::move(details),
                {},
                Priority::Normal,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                stamp.timestamp,
                stamp.identity,
                stamp.timestamp,
                stamp.identity};
}

Lane* mutable_lane(WorkspaceSnapshot& snapshot, LaneId id) noexcept {
    for (Board& board : snapshot.boards) {
        const auto found =
            std::find_if(board.lanes.begin(), board.lanes.end(), [id](const Lane& lane) { return lane.id == id; });
        if (found != board.lanes.end()) return &*found;
    }
    return nullptr;
}

Board* mutable_board(WorkspaceSnapshot& snapshot, BoardId id) noexcept {
    const auto found = std::find_if(snapshot.boards.begin(), snapshot.boards.end(),
                                    [id](const Board& board) { return board.id == id; });
    return found == snapshot.boards.end() ? nullptr : &*found;
}

Task* mutable_task(WorkspaceSnapshot& snapshot, TaskId id) noexcept {
    const auto found =
        std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(), [id](const Task& task) { return task.id == id; });
    return found == snapshot.tasks.end() ? nullptr : &*found;
}

std::optional<TaskId> task_after(const Lane& lane, TaskId id) noexcept {
    const auto found = std::find(lane.task_ids.begin(), lane.task_ids.end(), id);
    if (found == lane.task_ids.end() || std::next(found) == lane.task_ids.end()) return std::nullopt;
    return *std::next(found);
}

Task make_task(TaskId id, TaskDraft draft, const AuditStamp& stamp) {
    return Task{id,
                std::move(draft.title),
                std::move(draft.details),
                std::move(draft.note),
                draft.priority,
                std::move(draft.due_date),
                std::move(draft.due_time),
                draft.color,
                stamp.timestamp,
                stamp.identity,
                stamp.timestamp,
                stamp.identity};
}

bool content_matches(const Task& task, const TaskDraft& draft) noexcept {
    return task.title == draft.title && task.details == draft.details && task.note == draft.note &&
           task.priority == draft.priority && task.due_date == draft.due_date &&
           task.due_time == draft.due_time && task.color == draft.color;
}

ModelResult<WorkspaceSnapshot> checked_candidate(WorkspaceSnapshot candidate) {
    return validate_snapshot(std::move(candidate));
}

std::optional<ModelError> validate_stamp(const AuditStamp& stamp) {
    if (!valid_timestamp(stamp.timestamp.value)) {
        return ModelError{ModelErrorCode::InvalidTimestamp, "archive timestamp is invalid"};
    }
    return validate_text(stamp.identity, TodoLimits::max_identity_bytes, true, false, "archive identity");
}

const Board* board_containing(const WorkspaceSnapshot& snapshot, LaneId lane_id) noexcept {
    for (const Board& board : snapshot.boards) {
        if (std::any_of(board.lanes.begin(), board.lanes.end(),
                        [lane_id](const Lane& lane) { return lane.id == lane_id; })) {
            return &board;
        }
    }
    return nullptr;
}

ArchivedTask archived_task(const WorkspaceSnapshot& snapshot,
                           const Board& board,
                           const Lane& lane,
                           TaskId task_id,
                           const AuditStamp& stamp) {
    const auto found = std::find_if(snapshot.tasks.begin(), snapshot.tasks.end(),
                                    [task_id](const Task& task) { return task.id == task_id; });
    return ArchivedTask{*found, board.id, board.name, lane.id, lane.title, stamp};
}

void erase_archived_tasks(WorkspaceSnapshot& snapshot, const std::vector<ArchivedTask>& records) {
    std::unordered_set<std::uint64_t> archived_ids;
    for (const ArchivedTask& record : records) archived_ids.insert(record.task.id.value);
    snapshot.tasks.erase(std::remove_if(snapshot.tasks.begin(), snapshot.tasks.end(), [&](const Task& task) {
                             return archived_ids.contains(task.id.value);
                         }),
                         snapshot.tasks.end());
}

}  // namespace

ModelResult<TodoWorkspace> TodoWorkspace::from_snapshot(WorkspaceSnapshot snapshot) {
    auto validated = validate_snapshot(std::move(snapshot));
    if (!validated) return failure<TodoWorkspace>(validated.error.code, std::move(validated.error.diagnostic));
    return ModelResult<TodoWorkspace>::success(TodoWorkspace(std::move(*validated.value)));
}

bool is_valid(IsoDate date) noexcept { return valid_date(date.value); }

bool is_valid(IsoTime time) noexcept { return valid_time(time.value); }

bool is_valid(IsoTimestamp timestamp) noexcept { return valid_timestamp(timestamp.value); }

TodoWorkspace TodoWorkspace::empty() {
    WorkspaceSnapshot snapshot;
    snapshot.last_board_id = BoardId{1};
    snapshot.next_board_id = 2;
    snapshot.next_lane_id = 4;
    snapshot.next_task_id = 1;
    snapshot.boards = {Board{BoardId{1},
                             "main",
                             {Lane{LaneId{1}, "To Do", std::nullopt, SortMode::Manual, {}},
                              Lane{LaneId{2}, "Doing", std::nullopt, SortMode::Manual, {}},
                              Lane{LaneId{3}, "Done", std::nullopt, SortMode::Manual, {}}}}};
    return TodoWorkspace(std::move(snapshot));
}

ModelResult<TodoWorkspace> TodoWorkspace::guided(const AuditStamp& stamp) {
    WorkspaceSnapshot snapshot = empty().snapshot();
    snapshot.next_task_id = 4;
    // The guidance is saved with the workspace and outlives any keymap, so
    // it names the menu entry rather than a key: a key written into a task
    // would still be advertised after the reader had rebound it. The menus
    // and the status line show whatever chord each command has now.
    snapshot.tasks = {guided_task(TaskId{1}, "Add your first task", "Tasks > Add task", stamp),
                      guided_task(TaskId{2}, "Open contextual help", "Help > Context Help", stamp),
                      guided_task(TaskId{3}, "Try move mode", "Tasks > Move task", stamp)};
    snapshot.boards.front().lanes[0].task_ids = {TaskId{1}};
    snapshot.boards.front().lanes[1].task_ids = {TaskId{2}};
    snapshot.boards.front().lanes[2].task_ids = {TaskId{3}};
    return from_snapshot(std::move(snapshot));
}

const Board* TodoWorkspace::find_board(BoardId id) const noexcept {
    const auto found = std::find_if(snapshot_.boards.begin(), snapshot_.boards.end(),
                                    [id](const Board& board) { return board.id == id; });
    return found == snapshot_.boards.end() ? nullptr : &*found;
}

const Lane* TodoWorkspace::find_lane(LaneId id) const noexcept {
    for (const Board& board : snapshot_.boards) {
        const auto found = std::find_if(board.lanes.begin(), board.lanes.end(),
                                        [id](const Lane& lane) { return lane.id == id; });
        if (found != board.lanes.end()) return &*found;
    }
    return nullptr;
}

const Task* TodoWorkspace::find_task(TaskId id) const noexcept {
    const auto found = std::find_if(snapshot_.tasks.begin(), snapshot_.tasks.end(),
                                    [id](const Task& task) { return task.id == id; });
    return found == snapshot_.tasks.end() ? nullptr : &*found;
}

std::optional<BoardId> TodoWorkspace::board_of(LaneId id) const noexcept {
    for (const Board& board : snapshot_.boards) {
        if (std::any_of(board.lanes.begin(), board.lanes.end(), [id](const Lane& lane) { return lane.id == id; })) {
            return board.id;
        }
    }
    return std::nullopt;
}

std::optional<LaneId> TodoWorkspace::lane_of(TaskId id) const noexcept {
    for (const Board& board : snapshot_.boards) {
        for (const Lane& lane : board.lanes) {
            if (std::find(lane.task_ids.begin(), lane.task_ids.end(), id) != lane.task_ids.end()) return lane.id;
        }
    }
    return std::nullopt;
}

ModelResult<std::vector<TaskId>> TodoWorkspace::ordered_tasks(LaneId id) const {
    const Lane* lane = find_lane(id);
    if (lane == nullptr) return failure<std::vector<TaskId>>(ModelErrorCode::LaneNotFound, "lane does not exist");
    std::vector<TaskId> ordered = lane->task_ids;
    if (lane->sort == SortMode::Manual) return ModelResult<std::vector<TaskId>>::success(std::move(ordered));

    const auto task_for = [this](TaskId task_id) -> const Task& { return *find_task(task_id); };
    std::stable_sort(ordered.begin(), ordered.end(), [&](TaskId left_id, TaskId right_id) {
        const Task& left = task_for(left_id);
        const Task& right = task_for(right_id);
        switch (lane->sort) {
            case SortMode::Manual: return false;
            case SortMode::Color:
                if (left.color.has_value() != right.color.has_value()) return left.color.has_value();
                return left.color && right.color && *left.color < *right.color;
            case SortMode::Due:
                if (left.due_date.has_value() != right.due_date.has_value()) return left.due_date.has_value();
                if (!left.due_date || !right.due_date) return false;
                if (left.due_date->value != right.due_date->value) {
                    return left.due_date->value < right.due_date->value;
                }
                if (left.due_time.has_value() != right.due_time.has_value()) return !left.due_time.has_value();
                return left.due_time && right.due_time && left.due_time->value < right.due_time->value;
            case SortMode::Created: return left.created_at.value < right.created_at.value;
            case SortMode::Modified: return left.modified_at.value < right.modified_at.value;
            case SortMode::Priority: return left.priority < right.priority;
        }
        return false;
    });
    return ModelResult<std::vector<TaskId>>::success(std::move(ordered));
}

ModelResult<TaskId> TodoWorkspace::add_task(LaneId lane_id,
                                            TaskDraft draft,
                                            const AuditStamp& stamp,
                                            std::optional<TaskId> before) {
    const Lane* existing_lane = find_lane(lane_id);
    if (existing_lane == nullptr) return failure<TaskId>(ModelErrorCode::LaneNotFound, "lane does not exist");
    if (snapshot_.tasks.size() >= TodoLimits::max_tasks) {
        return failure<TaskId>(ModelErrorCode::WorkspaceLimit, "workspace task limit exceeded");
    }
    if (before && std::find(existing_lane->task_ids.begin(), existing_lane->task_ids.end(), *before) ==
                      existing_lane->task_ids.end()) {
        return failure<TaskId>(ModelErrorCode::InvalidInsertionAnchor, "task insertion anchor is not in the lane");
    }
    if (before && existing_lane->sort != SortMode::Manual) {
        return failure<TaskId>(ModelErrorCode::ManualOrderRequired,
                               "positioned insertion requires manual lane sorting");
    }

    WorkspaceSnapshot candidate = snapshot_;
    const TaskId id{candidate.next_task_id};
    ++candidate.next_task_id;
    candidate.tasks.push_back(make_task(id, std::move(draft), stamp));
    Lane* lane = mutable_lane(candidate, lane_id);
    const auto position = before ? std::find(lane->task_ids.begin(), lane->task_ids.end(), *before) : lane->task_ids.end();
    lane->task_ids.insert(position, id);
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<TaskId>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<TaskId>::success(id);
}

ModelResult<Mutation> TodoWorkspace::edit_task(TaskId task_id, TaskDraft draft, const AuditStamp& stamp) {
    const Task* existing = find_task(task_id);
    if (existing == nullptr) return failure<Mutation>(ModelErrorCode::TaskNotFound, "task does not exist");
    if (content_matches(*existing, draft)) return ModelResult<Mutation>::success(Mutation{});
    if (stamp.timestamp.value < existing->modified_at.value) {
        return failure<Mutation>(ModelErrorCode::TimestampRegression, "task modification timestamp regressed");
    }

    WorkspaceSnapshot candidate = snapshot_;
    Task* task = mutable_task(candidate, task_id);
    task->title = std::move(draft.title);
    task->details = std::move(draft.details);
    task->note = std::move(draft.note);
    task->priority = draft.priority;
    task->due_date = std::move(draft.due_date);
    task->due_time = std::move(draft.due_time);
    task->color = draft.color;
    task->modified_at = stamp.timestamp;
    task->modified_by = stamp.identity;
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<Mutation> TodoWorkspace::delete_task(TaskId task_id) {
    if (find_task(task_id) == nullptr) return failure<Mutation>(ModelErrorCode::TaskNotFound, "task does not exist");
    WorkspaceSnapshot candidate = snapshot_;
    Lane* owner = mutable_lane(candidate, *lane_of(task_id));
    owner->task_ids.erase(std::find(owner->task_ids.begin(), owner->task_ids.end(), task_id));
    candidate.tasks.erase(std::find_if(candidate.tasks.begin(), candidate.tasks.end(),
                                       [task_id](const Task& task) { return task.id == task_id; }));
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<PendingTaskMove> TodoWorkspace::begin_task_move(TaskId task_id) const {
    const std::optional<LaneId> source_id = lane_of(task_id);
    if (!source_id) return failure<PendingTaskMove>(ModelErrorCode::TaskNotFound, "task does not exist");
    const Lane& source = *find_lane(*source_id);
    const std::optional<TaskId> before = task_after(source, task_id);
    return ModelResult<PendingTaskMove>::success(
        PendingTaskMove{task_id, *source_id, before, *source_id, before, generation_});
}

ModelResult<PendingTaskMove> TodoWorkspace::stage_task_move(const PendingTaskMove& move,
                                                            LaneId target_lane_id,
                                                            std::optional<TaskId> before) const {
    if (move.generation != generation_) {
        return failure<PendingTaskMove>(ModelErrorCode::StaleTransaction, "task move is stale");
    }
    if (lane_of(move.task_id) != move.source_lane_id) {
        return failure<PendingTaskMove>(ModelErrorCode::StaleTransaction, "task move source changed");
    }
    const Lane* target = find_lane(target_lane_id);
    if (target == nullptr) return failure<PendingTaskMove>(ModelErrorCode::LaneNotFound, "target lane does not exist");
    if (before == move.task_id ||
        (before && std::find(target->task_ids.begin(), target->task_ids.end(), *before) == target->task_ids.end())) {
        return failure<PendingTaskMove>(ModelErrorCode::InvalidInsertionAnchor,
                                       "task move anchor is not in the target lane");
    }
    if (target_lane_id == move.source_lane_id && target->sort != SortMode::Manual &&
        before != move.original_before_task_id) {
        return failure<PendingTaskMove>(ModelErrorCode::ManualOrderRequired,
                                       "reordering requires manual lane sorting");
    }
    if (target_lane_id != move.source_lane_id && target->sort != SortMode::Manual && before) {
        return failure<PendingTaskMove>(ModelErrorCode::ManualOrderRequired,
                                       "sorted target lanes accept moved tasks without a manual position");
    }
    PendingTaskMove staged = move;
    staged.target_lane_id = target_lane_id;
    staged.before_task_id = before;
    return ModelResult<PendingTaskMove>::success(std::move(staged));
}

ModelResult<Mutation> TodoWorkspace::commit_task_move(const PendingTaskMove& move) {
    if (move.generation != generation_ || lane_of(move.task_id) != move.source_lane_id) {
        return failure<Mutation>(ModelErrorCode::StaleTransaction, "task move is stale");
    }
    const Lane* target = find_lane(move.target_lane_id);
    if (target == nullptr) return failure<Mutation>(ModelErrorCode::LaneNotFound, "target lane does not exist");
    if (move.before_task_id == move.task_id ||
        (move.before_task_id &&
         std::find(target->task_ids.begin(), target->task_ids.end(), *move.before_task_id) == target->task_ids.end())) {
        return failure<Mutation>(ModelErrorCode::InvalidInsertionAnchor,
                                 "task move anchor is not in the target lane");
    }
    if (move.target_lane_id == move.source_lane_id && target->sort != SortMode::Manual &&
        move.before_task_id != move.original_before_task_id) {
        return failure<Mutation>(ModelErrorCode::ManualOrderRequired, "reordering requires manual lane sorting");
    }
    if (move.target_lane_id != move.source_lane_id && target->sort != SortMode::Manual && move.before_task_id) {
        return failure<Mutation>(ModelErrorCode::ManualOrderRequired,
                                 "sorted target lanes accept moved tasks without a manual position");
    }

    WorkspaceSnapshot candidate = snapshot_;
    Lane* source = mutable_lane(candidate, move.source_lane_id);
    source->task_ids.erase(std::find(source->task_ids.begin(), source->task_ids.end(), move.task_id));
    Lane* candidate_target = mutable_lane(candidate, move.target_lane_id);
    const auto position = move.before_task_id
                              ? std::find(candidate_target->task_ids.begin(), candidate_target->task_ids.end(),
                                          *move.before_task_id)
                              : candidate_target->task_ids.end();
    candidate_target->task_ids.insert(position, move.task_id);
    if (candidate == snapshot_) return ModelResult<Mutation>::success(Mutation{});
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<LaneId> TodoWorkspace::insert_lane(BoardId board_id,
                                               std::string title,
                                               std::optional<LaneId> before) {
    const Board* board = find_board(board_id);
    if (board == nullptr) return failure<LaneId>(ModelErrorCode::BoardNotFound, "board does not exist");
    if (board->lanes.size() >= TodoLimits::max_lanes_per_board) {
        return failure<LaneId>(ModelErrorCode::WorkspaceLimit, "board lane limit exceeded");
    }
    if (before && std::find_if(board->lanes.begin(), board->lanes.end(), [before](const Lane& lane) {
                      return lane.id == *before;
                  }) == board->lanes.end()) {
        return failure<LaneId>(ModelErrorCode::InvalidInsertionAnchor, "lane insertion anchor is not in the board");
    }

    WorkspaceSnapshot candidate = snapshot_;
    const LaneId id{candidate.next_lane_id};
    ++candidate.next_lane_id;
    Board* candidate_board = mutable_board(candidate, board_id);
    const auto position = before ? std::find_if(candidate_board->lanes.begin(), candidate_board->lanes.end(),
                                                [before](const Lane& lane) { return lane.id == *before; })
                                 : candidate_board->lanes.end();
    candidate_board->lanes.insert(position, Lane{id, std::move(title), std::nullopt, SortMode::Manual, {}});
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<LaneId>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<LaneId>::success(id);
}

ModelResult<Mutation> TodoWorkspace::rename_lane(LaneId lane_id, std::string title) {
    const Lane* existing = find_lane(lane_id);
    if (existing == nullptr) return failure<Mutation>(ModelErrorCode::LaneNotFound, "lane does not exist");
    if (existing->title == title) return ModelResult<Mutation>::success(Mutation{});
    WorkspaceSnapshot candidate = snapshot_;
    mutable_lane(candidate, lane_id)->title = std::move(title);
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<Mutation> TodoWorkspace::set_lane_color(LaneId lane_id, std::optional<TodoColor> color) {
    const Lane* existing = find_lane(lane_id);
    if (existing == nullptr) return failure<Mutation>(ModelErrorCode::LaneNotFound, "lane does not exist");
    if (existing->color == color) return ModelResult<Mutation>::success(Mutation{});
    WorkspaceSnapshot candidate = snapshot_;
    mutable_lane(candidate, lane_id)->color = color;
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<Mutation> TodoWorkspace::set_lane_sort(LaneId lane_id, SortMode sort) {
    const Lane* existing = find_lane(lane_id);
    if (existing == nullptr) return failure<Mutation>(ModelErrorCode::LaneNotFound, "lane does not exist");
    if (existing->sort == sort) return ModelResult<Mutation>::success(Mutation{});
    WorkspaceSnapshot candidate = snapshot_;
    mutable_lane(candidate, lane_id)->sort = sort;
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<Mutation> TodoWorkspace::merge_lane(LaneId source_lane_id, LaneId target_lane_id) {
    if (source_lane_id == target_lane_id) {
        return failure<Mutation>(ModelErrorCode::SameSourceAndTarget, "source and target lanes are identical");
    }
    const Lane* source = find_lane(source_lane_id);
    const Lane* target = find_lane(target_lane_id);
    if (source == nullptr || target == nullptr) {
        return failure<Mutation>(ModelErrorCode::LaneNotFound, "source or target lane does not exist");
    }
    const std::optional<BoardId> source_board_id = board_of(source_lane_id);
    if (source_board_id != board_of(target_lane_id)) {
        return failure<Mutation>(ModelErrorCode::CrossBoardLaneMerge, "lanes on different boards cannot merge");
    }

    WorkspaceSnapshot candidate = snapshot_;
    Board* board = mutable_board(candidate, *source_board_id);
    Lane* candidate_target = mutable_lane(candidate, target_lane_id);
    candidate_target->task_ids.insert(candidate_target->task_ids.end(), source->task_ids.begin(), source->task_ids.end());
    board->lanes.erase(std::find_if(board->lanes.begin(), board->lanes.end(),
                                    [source_lane_id](const Lane& lane) { return lane.id == source_lane_id; }));
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<BoardId> TodoWorkspace::add_board(std::string name) {
    if (snapshot_.boards.size() >= TodoLimits::max_boards) {
        return failure<BoardId>(ModelErrorCode::WorkspaceLimit, "workspace board or lane limit exceeded");
    }
    WorkspaceSnapshot candidate = snapshot_;
    const BoardId board_id{candidate.next_board_id};
    ++candidate.next_board_id;
    const LaneId todo_id{candidate.next_lane_id++};
    const LaneId doing_id{candidate.next_lane_id++};
    const LaneId done_id{candidate.next_lane_id++};
    candidate.boards.push_back(
        Board{board_id,
              std::move(name),
              {Lane{todo_id, "To Do", std::nullopt, SortMode::Manual, {}},
               Lane{doing_id, "Doing", std::nullopt, SortMode::Manual, {}},
               Lane{done_id, "Done", std::nullopt, SortMode::Manual, {}}}});
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<BoardId>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<BoardId>::success(board_id);
}

ModelResult<Mutation> TodoWorkspace::rename_board(BoardId board_id, std::string name) {
    const Board* board = find_board(board_id);
    if (board == nullptr) return failure<Mutation>(ModelErrorCode::BoardNotFound, "board does not exist");
    if (board_id == BoardId{1} && name != "main") {
        return failure<Mutation>(ModelErrorCode::ProtectedMainBoard, "main board cannot be renamed");
    }
    if (board->name == name) return ModelResult<Mutation>::success(Mutation{});
    WorkspaceSnapshot candidate = snapshot_;
    mutable_board(candidate, board_id)->name = std::move(name);
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<Mutation> TodoWorkspace::switch_board(BoardId board_id) {
    if (find_board(board_id) == nullptr) {
        return failure<Mutation>(ModelErrorCode::BoardNotFound, "board does not exist");
    }
    if (snapshot_.last_board_id == board_id) return ModelResult<Mutation>::success(Mutation{});
    snapshot_.last_board_id = board_id;
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<Mutation> TodoWorkspace::merge_board(BoardId source_board_id, BoardId target_board_id) {
    if (source_board_id == target_board_id) {
        return failure<Mutation>(ModelErrorCode::SameSourceAndTarget, "source and target boards are identical");
    }
    if (source_board_id == BoardId{1}) {
        return failure<Mutation>(ModelErrorCode::ProtectedMainBoard, "main board cannot be removed by merge");
    }
    const Board* source = find_board(source_board_id);
    const Board* target = find_board(target_board_id);
    if (source == nullptr || target == nullptr) {
        return failure<Mutation>(ModelErrorCode::BoardNotFound, "source or target board does not exist");
    }

    WorkspaceSnapshot candidate = snapshot_;
    Board* candidate_target = mutable_board(candidate, target_board_id);
    const std::size_t original_target_lane_count = candidate_target->lanes.size();
    for (const Lane& source_lane : source->lanes) {
        auto match = std::find_if(candidate_target->lanes.begin(),
                                  candidate_target->lanes.begin() +
                                      static_cast<std::ptrdiff_t>(original_target_lane_count),
                                  [&](const Lane& target_lane) { return target_lane.title == source_lane.title; });
        if (match == candidate_target->lanes.begin() + static_cast<std::ptrdiff_t>(original_target_lane_count)) {
            candidate_target->lanes.push_back(source_lane);
        } else {
            match->task_ids.insert(match->task_ids.end(), source_lane.task_ids.begin(), source_lane.task_ids.end());
        }
    }
    candidate.boards.erase(std::find_if(candidate.boards.begin(), candidate.boards.end(),
                                        [source_board_id](const Board& board) { return board.id == source_board_id; }));
    if (candidate.last_board_id == source_board_id) candidate.last_board_id = target_board_id;
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

ModelResult<ArchivePlan> TodoWorkspace::prepare_task_archive(TaskId task_id, const AuditStamp& stamp) const {
    if (const auto error = validate_stamp(stamp)) {
        return failure<ArchivePlan>(error->code, error->diagnostic);
    }
    const std::optional<LaneId> lane_id = lane_of(task_id);
    if (!lane_id) return failure<ArchivePlan>(ModelErrorCode::TaskNotFound, "task does not exist");
    const Lane& lane = *find_lane(*lane_id);
    const Board& board = *board_containing(snapshot_, *lane_id);
    ArchivePlan plan;
    plan.scope = ArchiveScope::Task;
    plan.generation = generation_;
    plan.archived = stamp;
    plan.task_id = task_id;
    plan.records.push_back(archived_task(snapshot_, board, lane, task_id, stamp));
    return ModelResult<ArchivePlan>::success(std::move(plan));
}

ModelResult<ArchivePlan> TodoWorkspace::prepare_lane_archive(LaneId lane_id, const AuditStamp& stamp) const {
    if (const auto error = validate_stamp(stamp)) {
        return failure<ArchivePlan>(error->code, error->diagnostic);
    }
    const Lane* lane = find_lane(lane_id);
    if (lane == nullptr) return failure<ArchivePlan>(ModelErrorCode::LaneNotFound, "lane does not exist");
    const Board& board = *board_containing(snapshot_, lane_id);
    if (board.lanes.size() == 1U) {
        return failure<ArchivePlan>(ModelErrorCode::FinalLane, "final lane cannot be removed");
    }
    ArchivePlan plan;
    plan.scope = ArchiveScope::Lane;
    plan.generation = generation_;
    plan.archived = stamp;
    plan.lane_id = lane_id;
    plan.board_id = board.id;
    for (TaskId task_id : lane->task_ids) {
        plan.records.push_back(archived_task(snapshot_, board, *lane, task_id, stamp));
    }
    return ModelResult<ArchivePlan>::success(std::move(plan));
}

ModelResult<ArchivePlan> TodoWorkspace::prepare_board_archive(BoardId board_id, const AuditStamp& stamp) const {
    if (const auto error = validate_stamp(stamp)) {
        return failure<ArchivePlan>(error->code, error->diagnostic);
    }
    if (board_id == BoardId{1}) {
        return failure<ArchivePlan>(ModelErrorCode::ProtectedMainBoard, "main board cannot be archived");
    }
    const Board* board = find_board(board_id);
    if (board == nullptr) return failure<ArchivePlan>(ModelErrorCode::BoardNotFound, "board does not exist");
    ArchivePlan plan;
    plan.scope = ArchiveScope::Board;
    plan.generation = generation_;
    plan.archived = stamp;
    plan.board_id = board_id;
    for (const Lane& lane : board->lanes) {
        for (TaskId task_id : lane.task_ids) {
            plan.records.push_back(archived_task(snapshot_, *board, lane, task_id, stamp));
        }
    }
    return ModelResult<ArchivePlan>::success(std::move(plan));
}

ModelResult<Mutation> TodoWorkspace::apply_archive(const ArchivePlan& plan) {
    if (plan.generation != generation_) {
        return failure<Mutation>(ModelErrorCode::StaleTransaction, "archive plan is stale");
    }
    ModelResult<ArchivePlan> expected;
    switch (plan.scope) {
        case ArchiveScope::Task:
            if (!plan.task_id) {
                return failure<Mutation>(ModelErrorCode::InvalidArchivePlan, "task archive plan has no task id");
            }
            expected = prepare_task_archive(*plan.task_id, plan.archived);
            break;
        case ArchiveScope::Lane:
            if (!plan.lane_id) {
                return failure<Mutation>(ModelErrorCode::InvalidArchivePlan, "lane archive plan has no lane id");
            }
            expected = prepare_lane_archive(*plan.lane_id, plan.archived);
            break;
        case ArchiveScope::Board:
            if (!plan.board_id) {
                return failure<Mutation>(ModelErrorCode::InvalidArchivePlan, "board archive plan has no board id");
            }
            expected = prepare_board_archive(*plan.board_id, plan.archived);
            break;
    }
    if (!expected || *expected.value != plan) {
        return failure<Mutation>(ModelErrorCode::InvalidArchivePlan, "archive plan no longer matches the workspace");
    }

    WorkspaceSnapshot candidate = snapshot_;
    erase_archived_tasks(candidate, plan.records);
    if (plan.scope == ArchiveScope::Task) {
        Lane* lane = mutable_lane(candidate, plan.records.front().origin_lane_id);
        lane->task_ids.erase(std::find(lane->task_ids.begin(), lane->task_ids.end(), *plan.task_id));
    } else if (plan.scope == ArchiveScope::Lane) {
        Board* board = mutable_board(candidate, *plan.board_id);
        board->lanes.erase(std::find_if(board->lanes.begin(), board->lanes.end(),
                                        [&](const Lane& lane) { return lane.id == *plan.lane_id; }));
    } else {
        candidate.boards.erase(std::find_if(candidate.boards.begin(), candidate.boards.end(),
                                            [&](const Board& board) { return board.id == *plan.board_id; }));
        if (candidate.last_board_id == *plan.board_id) candidate.last_board_id = BoardId{1};
    }
    auto validated = checked_candidate(std::move(candidate));
    if (!validated) return failure<Mutation>(validated.error.code, std::move(validated.error.diagnostic));
    snapshot_ = std::move(*validated.value);
    ++generation_;
    return ModelResult<Mutation>::success(Mutation{true});
}

}  // namespace ck::todo
