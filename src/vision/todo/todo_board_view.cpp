// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_board_view.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

#include "cvision/ui/application.hpp"

namespace ck::todo {
namespace {

constexpr int kLaneGap = 1;
constexpr int kMinimumLaneWidth = 20;
constexpr int kPreferredLaneWidth = 26;

}  // namespace

TodoBoardView::TodoBoardView() {
    set_help_context_key("todo.board");
    auto viewport = std::make_unique<ckv::widgets::ScrollViewport>();
    viewport_ = viewport.get();
    viewport_->set_vertical_scrollbar_policy(ckv::widgets::ScrollbarPolicy::Hidden);
    viewport_->set_horizontal_scrollbar_policy(ckv::widgets::ScrollbarPolicy::Auto);
    auto row = std::make_unique<ckv::ui::Row>();
    row_ = row.get();
    row_->set_spacing(kLaneGap);
    viewport_->set_content(std::move(row));
    add(std::move(viewport));
}

bool TodoBoardView::set_board(const TodoWorkspace& workspace, BoardId board_id) {
    const Board* board = workspace.find_board(board_id);
    if (board == nullptr) return false;
    rebuild(workspace, *board);
    board_id_ = board_id;
    layout_lanes();
    invalidate();
    return true;
}

std::optional<LaneId> TodoBoardView::active_lane() const noexcept {
    if (active_lane_index_ >= lanes_.size()) return std::nullopt;
    return lanes_[active_lane_index_]->lane_id();
}

std::optional<TaskId> TodoBoardView::selected_task() const noexcept {
    if (active_lane_index_ >= lanes_.size()) return std::nullopt;
    return lanes_[active_lane_index_]->selected_task();
}

TodoLaneView* TodoBoardView::lane_view(LaneId lane_id) const noexcept {
    const auto found = std::find_if(lanes_.begin(), lanes_.end(),
                                    [lane_id](const TodoLaneView* lane) { return lane->lane_id() == lane_id; });
    return found == lanes_.end() ? nullptr : *found;
}

bool TodoBoardView::select_task(TaskId task_id) {
    for (std::size_t index = 0; index < lanes_.size(); ++index) {
        TodoLaneView* lane = lanes_[index];
        const auto& ids = lane->lane().task_ids;
        if (std::find(ids.begin(), ids.end(), task_id) == ids.end()) continue;
        lane->select_task(task_id);
        active_lane_index_ = index;
        ensure_lane_visible(index);
        if (context().app != nullptr) context().app->set_focus(lane);
        return true;
    }
    return false;
}

void TodoBoardView::set_today(std::optional<IsoDate> today) {
    if (today_ == today) return;
    today_ = std::move(today);
    for (TodoLaneView* lane : lanes_) lane->set_today(today_);
}

void TodoBoardView::set_move_source(std::optional<TaskId> task_id) {
    move_source_ = task_id;
    for (TodoLaneView* lane : lanes_) lane->set_move_source(task_id);
}

void TodoBoardView::set_move_target(std::optional<LaneId> lane_id) {
    move_target_ = lane_id;
    for (TodoLaneView* lane : lanes_) lane->set_move_target(lane_id == lane->lane_id());
}

void TodoBoardView::begin_keyboard_move(TaskId task_id, LaneId source_lane,
                                        std::optional<TaskId> original_before) {
    keyboard_move_ = KeyboardMoveState{task_id, source_lane, original_before, original_before};
    set_move_source(task_id);
    set_move_target(source_lane);
    update_keyboard_drop_marker();
}

void TodoBoardView::end_keyboard_move() {
    keyboard_move_.reset();
    for (TodoLaneView* lane : lanes_) lane->set_drop_marker(std::nullopt, false);
    set_move_source(std::nullopt);
    set_move_target(std::nullopt);
}

std::optional<TaskId> TodoBoardView::keyboard_move_before() const noexcept {
    return keyboard_move_ ? keyboard_move_->before : std::nullopt;
}

ckv::ui::SizeHint TodoBoardView::horizontal_size_hint() const {
    return {kMinimumLaneWidth, kPreferredLaneWidth * 3 + kLaneGap * 2, ckv::ui::kUnboundedExtent};
}

ckv::ui::SizeHint TodoBoardView::vertical_size_hint() const { return {6, 18, ckv::ui::kUnboundedExtent}; }

void TodoBoardView::on_resized() { layout_lanes(); }

bool TodoBoardView::on_mouse(const ckv::MouseEvent& event) {
    if (event.action != ckv::MouseAction::Wheel || viewport_ == nullptr) return false;
    if (event.button == ckv::MouseButton::WheelLeft)
        viewport_->set_scroll(viewport_->scroll_x() - kPreferredLaneWidth, viewport_->scroll_y());
    else if (event.button == ckv::MouseButton::WheelRight)
        viewport_->set_scroll(viewport_->scroll_x() + kPreferredLaneWidth, viewport_->scroll_y());
    else
        return false;
    return true;
}

void TodoBoardView::rebuild(const TodoWorkspace& workspace, const Board& board) {
    std::map<LaneId, std::optional<TaskId>> selections;
    for (TodoLaneView* lane : lanes_) selections.emplace(lane->lane_id(), lane->selected_task());
    const std::optional<LaneId> previous_active = active_lane();
    auto row = std::make_unique<ckv::ui::Row>();
    row_ = row.get();
    row_->set_spacing(kLaneGap);
    lanes_.clear();

    for (const Lane& lane : board.lanes) {
        auto view = std::make_unique<TodoLaneView>();
        TodoLaneView* observer = view.get();
        observer->set_lane(workspace, lane.id);
        observer->set_today(today_);
        if (const auto found = selections.find(lane.id); found != selections.end() && found->second)
            observer->select_task(*found->second);
        observer->set_move_source(move_source_);
        observer->set_move_target(move_target_ == lane.id);
        observer->on_selection_changed = [this, lane_id = lane.id](TaskId task_id) {
            activate_lane(lane_id);
            if (on_selection_changed) on_selection_changed(task_id);
        };
        observer->on_activate = [this](TaskId task_id) {
            if (on_task_activate) on_task_activate(task_id);
        };
        observer->on_move_toggle = [this](TaskId task_id) {
            if (on_move_toggle) on_move_toggle(task_id);
        };
        observer->on_move_step = [this](LaneId lane_id, int delta) { step_move_position(lane_id, delta); };
        observer->on_lane_step = [this](LaneId lane_id, int direction) { step_lane(lane_id, direction); };
        observer->on_focused = [this](LaneId lane_id) { activate_lane(lane_id); };
        observer->on_drag = [this](TaskId task_id, ckv::MouseAction action, ckv::Point screen_cell) {
            handle_drag(task_id, action, screen_cell);
        };
        observer->on_task_context_menu = [this](TaskId task_id, ckv::Point screen_cell) {
            select_task(task_id);
            if (on_task_context_menu) on_task_context_menu(task_id, screen_cell);
        };
        observer->on_lane_context_menu = [this](LaneId lane_id, ckv::Point screen_cell) {
            activate_lane(lane_id);
            if (context().app != nullptr) context().app->set_focus(lane_view(lane_id));
            if (on_lane_context_menu) on_lane_context_menu(lane_id, screen_cell);
        };
        lanes_.push_back(static_cast<TodoLaneView*>(
            row_->add_item(std::move(view), ckv::ui::LayoutSpec{ckv::ui::SizePolicy::Expanding, 1})));
    }

    viewport_->set_content(std::move(row));

    active_lane_index_ = 0;
    if (previous_active) {
        const auto found = std::find_if(lanes_.begin(), lanes_.end(), [previous_active](const TodoLaneView* lane) {
            return lane->lane_id() == *previous_active;
        });
        if (found != lanes_.end()) active_lane_index_ = static_cast<std::size_t>(found - lanes_.begin());
    }
}

void TodoBoardView::layout_lanes() {
    if (viewport_ == nullptr) return;
    viewport_->set_bounds(ckv::Rect{0, 0, std::max(0, bounds().width), std::max(0, bounds().height)});
}

void TodoBoardView::activate_lane(LaneId lane_id) {
    const auto found = std::find_if(lanes_.begin(), lanes_.end(),
                                    [lane_id](const TodoLaneView* lane) { return lane->lane_id() == lane_id; });
    if (found == lanes_.end()) return;
    const std::size_t index = static_cast<std::size_t>(found - lanes_.begin());
    if (active_lane_index_ == index) return;
    active_lane_index_ = index;
    ensure_lane_visible(index);
    if (on_active_lane_changed) on_active_lane_changed(lane_id);
}

void TodoBoardView::step_lane(LaneId lane_id, int direction) {
    const auto found = std::find_if(lanes_.begin(), lanes_.end(),
                                    [lane_id](const TodoLaneView* lane) { return lane->lane_id() == lane_id; });
    if (found == lanes_.end() || lanes_.empty()) return;
    const int index = static_cast<int>(found - lanes_.begin());
    const int count = static_cast<int>(lanes_.size());
    const int target = ((index + direction) % count + count) % count;
    active_lane_index_ = static_cast<std::size_t>(target);
    if (keyboard_move_) reset_move_position_for_active_lane();
    ensure_lane_visible(active_lane_index_);
    if (context().app != nullptr) context().app->set_focus(lanes_[active_lane_index_]);
    if (on_active_lane_changed) on_active_lane_changed(lanes_[active_lane_index_]->lane_id());
}

void TodoBoardView::step_move_position(LaneId lane_id, int delta) {
    if (!keyboard_move_ || active_lane() != lane_id) return;
    TodoLaneView* lane = lane_view(lane_id);
    if (lane == nullptr) return;
    if (lane->lane().sort != SortMode::Manual) {
        if (on_move_position_blocked) on_move_position_blocked();
        return;
    }
    std::vector<TaskId> anchors;
    anchors.reserve(lane->lane().task_ids.size());
    for (const TaskId task_id : lane->lane().task_ids)
        if (task_id != keyboard_move_->task_id) anchors.push_back(task_id);
    int slot = static_cast<int>(anchors.size());
    if (keyboard_move_->before) {
        const auto found = std::find(anchors.begin(), anchors.end(), *keyboard_move_->before);
        if (found != anchors.end()) slot = static_cast<int>(found - anchors.begin());
    }
    slot = std::clamp(slot + delta, 0, static_cast<int>(anchors.size()));
    keyboard_move_->before = slot < static_cast<int>(anchors.size())
                                 ? std::optional<TaskId>{anchors[static_cast<std::size_t>(slot)]}
                                 : std::nullopt;
    update_keyboard_drop_marker();
}

void TodoBoardView::reset_move_position_for_active_lane() {
    if (!keyboard_move_) return;
    const auto lane_id = active_lane();
    TodoLaneView* lane = lane_id ? lane_view(*lane_id) : nullptr;
    if (lane == nullptr) return;
    if (*lane_id == keyboard_move_->source_lane) {
        keyboard_move_->before = keyboard_move_->original_before;
    } else if (lane->lane().sort == SortMode::Manual) {
        keyboard_move_->before = lane->selected_task();
    } else {
        keyboard_move_->before.reset();
    }
    update_keyboard_drop_marker();
}

void TodoBoardView::update_keyboard_drop_marker() {
    for (TodoLaneView* lane : lanes_) lane->set_drop_marker(std::nullopt, false);
    if (!keyboard_move_) return;
    const auto lane_id = active_lane();
    TodoLaneView* lane = lane_id ? lane_view(*lane_id) : nullptr;
    if (lane == nullptr) return;
    lane->set_drop_marker(keyboard_move_->before, !keyboard_move_->before.has_value());
}

void TodoBoardView::ensure_lane_visible(std::size_t index) {
    if (index >= lanes_.size() || viewport_ == nullptr) return;
    viewport_->ensure_visible(*lanes_[index]);
}

TodoLaneView* TodoBoardView::lane_at(ckv::Point screen_cell) const noexcept {
    const auto found = std::find_if(lanes_.begin(), lanes_.end(), [screen_cell](const TodoLaneView* lane) {
        return lane->absolute_bounds().contains(screen_cell);
    });
    return found != lanes_.end() ? *found : nullptr;
}

void TodoBoardView::handle_drag(TaskId task_id, ckv::MouseAction action, ckv::Point screen_cell) {
    if (action == ckv::MouseAction::Down) {
        drag_ = DragState{task_id, screen_cell, false, std::nullopt, std::nullopt};
        return;
    }
    if (!drag_ || drag_->task_id != task_id) return;
    if (action == ckv::MouseAction::Move && !drag_->started && screen_cell != drag_->origin) {
        drag_->started = true;
        set_move_source(task_id);
    }
    if (action == ckv::MouseAction::Move && drag_->started) {
        TodoLaneView* target = lane_at(screen_cell);
        for (TodoLaneView* lane : lanes_) lane->set_drop_marker(std::nullopt, false);
        drag_->target_lane.reset();
        drag_->before_task.reset();
        if (target == nullptr) {
            set_move_target(std::nullopt);
            return;
        }
        activate_lane(target->lane_id());
        drag_->target_lane = target->lane_id();
        drag_->before_task = target->task_at(screen_cell);
        set_move_target(target->lane_id());
        target->set_drop_marker(drag_->before_task, !drag_->before_task.has_value());
        return;
    }
    if (action != ckv::MouseAction::Up) return;
    const std::optional<DragState> completed = drag_;
    clear_drag_feedback();
    if (completed->started && completed->target_lane && on_task_drop)
        on_task_drop(completed->task_id, *completed->target_lane, completed->before_task);
}

void TodoBoardView::clear_drag_feedback() {
    drag_.reset();
    set_move_source(std::nullopt);
    set_move_target(std::nullopt);
    for (TodoLaneView* lane : lanes_) lane->set_drop_marker(std::nullopt, false);
}

}  // namespace ck::todo
