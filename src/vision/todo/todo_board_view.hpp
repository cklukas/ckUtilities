// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "cvision/ui/layout.hpp"
#include "cvision/ui/view.hpp"
#include "cvision/widgets/scroll_viewport.hpp"

#include "todo_lane_view.hpp"

namespace ck::todo {

class TodoBoardView final : public ckv::ui::View {
public:
    TodoBoardView();

    bool set_board(const TodoWorkspace& workspace, BoardId board_id);
    BoardId board_id() const noexcept { return board_id_; }
    std::optional<LaneId> active_lane() const noexcept;
    std::optional<TaskId> selected_task() const noexcept;
    TodoLaneView* lane_view(LaneId lane_id) const noexcept;
    std::size_t lane_count() const noexcept { return lanes_.size(); }

    bool select_task(TaskId task_id);
    void set_today(std::optional<IsoDate> today);
    void set_move_source(std::optional<TaskId> task_id);
    void set_move_target(std::optional<LaneId> lane_id);
    void begin_keyboard_move(TaskId task_id, LaneId source_lane,
                             std::optional<TaskId> original_before);
    void end_keyboard_move();
    std::optional<TaskId> keyboard_move_before() const noexcept;

    std::function<void(TaskId)> on_selection_changed;
    std::function<void(TaskId)> on_task_activate;
    std::function<void(TaskId)> on_move_toggle;
    std::function<void(LaneId)> on_active_lane_changed;
    std::function<void(TaskId, LaneId, std::optional<TaskId>)> on_task_drop;
    std::function<void(TaskId, ckv::Point)> on_task_context_menu;
    std::function<void(LaneId, ckv::Point)> on_lane_context_menu;
    std::function<void()> on_move_position_blocked;

    ckv::ui::SizeHint horizontal_size_hint() const override;
    ckv::ui::SizeHint vertical_size_hint() const override;
    void on_resized() override;
    bool on_mouse(const ckv::MouseEvent& event) override;

private:
    void rebuild(const TodoWorkspace& workspace, const Board& board);
    void layout_lanes();
    void activate_lane(LaneId lane_id);
    void step_lane(LaneId lane_id, int direction);
    void step_move_position(LaneId lane_id, int delta);
    void reset_move_position_for_active_lane();
    void update_keyboard_drop_marker();
    void ensure_lane_visible(std::size_t index);
    void handle_drag(TaskId task_id, ckv::MouseAction action, ckv::Point screen_cell);
    TodoLaneView* lane_at(ckv::Point screen_cell) const noexcept;
    void clear_drag_feedback();

    struct DragState {
        TaskId task_id;
        ckv::Point origin;
        bool started = false;
        std::optional<LaneId> target_lane;
        std::optional<TaskId> before_task;
    };

    BoardId board_id_;
    std::vector<TodoLaneView*> lanes_;
    std::size_t active_lane_index_ = 0;
    std::optional<TaskId> move_source_;
    std::optional<LaneId> move_target_;
    std::optional<DragState> drag_;
    struct KeyboardMoveState {
        TaskId task_id;
        LaneId source_lane;
        std::optional<TaskId> original_before;
        std::optional<TaskId> before;
    };
    std::optional<KeyboardMoveState> keyboard_move_;
    std::optional<IsoDate> today_;
    ckv::widgets::ScrollViewport* viewport_ = nullptr;
    ckv::ui::Row* row_ = nullptr;
};

}  // namespace ck::todo
