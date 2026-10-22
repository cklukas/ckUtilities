// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "cvision/ui/theme.hpp"
#include "cvision/ui/view.hpp"
#include "cvision/widgets/scrollbar.hpp"

#include "todo_model.hpp"

namespace ck::todo {

class TodoLaneView final : public ckv::ui::View {
public:
    TodoLaneView();

    void set_lane(const TodoWorkspace& workspace, LaneId lane_id);
    void set_today(std::optional<IsoDate> today);
    LaneId lane_id() const noexcept { return lane_.id; }
    const Lane& lane() const noexcept { return lane_; }
    std::optional<TaskId> selected_task() const noexcept;
    std::optional<TaskId> task_at(ckv::Point screen_cell) const noexcept;
    void select_task(TaskId task_id);

    void set_move_source(std::optional<TaskId> task_id);
    void set_move_target(bool target);
    bool move_target() const noexcept { return move_target_; }
    void set_drop_marker(std::optional<TaskId> before, bool at_end);

    std::function<void(TaskId)> on_selection_changed;
    std::function<void(TaskId)> on_activate;
    std::function<void(TaskId)> on_move_toggle;
    std::function<void(LaneId, int)> on_move_step;
    std::function<void(LaneId, int)> on_lane_step;
    std::function<void(LaneId)> on_focused;
    std::function<void(TaskId, ckv::MouseAction, ckv::Point)> on_drag;
    std::function<void(TaskId, ckv::Point)> on_task_context_menu;
    std::function<void(LaneId, ckv::Point)> on_lane_context_menu;

    ckv::ui::SizeHint horizontal_size_hint() const override;
    ckv::ui::SizeHint vertical_size_hint() const override;
    void on_resized() override;
    void on_attached() override;
    void on_focus(const ckv::FocusEvent& event) override;
    bool on_key(const ckv::KeyEvent& event) override;
    bool on_mouse(const ckv::MouseEvent& event) override;
    std::optional<ckv::PointerShape> pointer_shape_at(ckv::Point cell) const override;
    void draw(ckv::scene::Painter& painter) override;

private:
    int visible_cards() const noexcept;
    void move_selection(int index);
    void ensure_selection_visible();
    ckv::Style lane_style() const;
    ckv::Style frame_style() const;
    ckv::Style task_marker_style(const Task& task, ckv::Style base) const;
    std::string due_badge(const Task& task) const;

    Lane lane_;
    std::vector<Task> tasks_;
    int selected_ = -1;
    bool focused_ = false;
    bool move_target_ = false;
    std::optional<TaskId> move_source_;
    std::optional<TaskId> pressed_task_;
    std::optional<TaskId> drop_before_;
    std::optional<IsoDate> today_;
    std::optional<IsoDate> tomorrow_;
    bool drop_at_end_ = false;
    ckv::widgets::Scrollbar* scrollbar_ = nullptr;
    ckv::ui::RoleId normal_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId selected_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId inactive_selected_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId frame_active_role_ = ckv::ui::kInvalidRole;
    ckv::ui::RoleId frame_inactive_role_ = ckv::ui::kInvalidRole;
};

}  // namespace ck::todo
