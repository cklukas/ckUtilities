// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <string_view>
#include <utility>
#include <vector>

#include "cvision/ui/application.hpp"
#include "cvision/ui/standard_roles.hpp"
#include "cvision/widgets/dialog.hpp"
#include "cvision/widgets/help_viewer.hpp"
#include "cvision/widgets/message_box.hpp"

#include "todo_board_view.hpp"
#include "todo_controller.hpp"

namespace ckv::widgets {
class Desktop;
class EditorDocument;
class MenuBar;
struct MenuBarItem;
class StatusLine;
class TextEditor;
class Window;
}  // namespace ckv::widgets

namespace ck::todo {

enum class TodoTheme { Classic, Dark, Light, Mono, HighContrast };

struct TodoAppOptions {
    std::optional<InitialWorkspace> first_workspace;
    std::optional<std::string> initial_board_name;
    std::string workspace_description = "persistent local workspace";
};

class TodoApp {
public:
    TodoApp(ckv::ui::Application& app,
            TodoRepository& repository,
            CalendarClock& calendar,
            std::string identity,
            TodoAppOptions options = {});
    ~TodoApp();

    TodoApp(const TodoApp&) = delete;
    TodoApp& operator=(const TodoApp&) = delete;

    static constexpr std::string_view kAddTaskKey = "todo.add-task";
    static constexpr std::string_view kEditTaskKey = "todo.edit-task";
    static constexpr std::string_view kEditNoteKey = "todo.edit-note";
    static constexpr std::string_view kArchiveTaskKey = "todo.archive-task";
    static constexpr std::string_view kDeleteTaskKey = "todo.delete-task";
    static constexpr std::string_view kMoveTaskKey = "todo.move-task";
    static constexpr std::string_view kCancelMoveKey = "todo.cancel-move";
    static constexpr std::string_view kLaneActionsKey = "todo.lane-actions";
    static constexpr std::string_view kBoardManagerKey = "todo.board-manager";
    static constexpr std::string_view kNewBoardKey = "todo.new-board";
    static constexpr std::array<std::string_view, 5> kThemeKeys{
        "todo.theme-classic", "todo.theme-dark", "todo.theme-light",
        "todo.theme-mono", "todo.theme-high-contrast"};

    ckv::widgets::Desktop& desktop() noexcept { return *desktop_; }
    ckv::widgets::Window* board_window() const noexcept { return board_window_; }
    TodoBoardView* board_view() const noexcept { return board_view_; }
    ckv::widgets::MenuBar* menu_bar() const noexcept { return menu_bar_; }
    ckv::widgets::StatusLine* status_line() const noexcept { return status_line_; }
    TodoController& controller() noexcept { return controller_; }
    const TodoController& controller() const noexcept { return controller_; }
    bool move_active() const noexcept { return move_.has_value(); }
    std::size_t note_window_count() const noexcept { return notes_.size(); }
    TodoTheme theme() const noexcept { return theme_; }

    void initialize_workspace(InitialWorkspace initial);
    void refresh_board();
    void poll_repository();

private:
    struct NoteSession;
    struct PendingUiConflict;

    void declare_commands();
    void bind_board_aliases();
    std::vector<ckv::widgets::MenuBarItem> build_menus();
    void build_chrome();
    void build_board_window();
    void install_help();
    // (Re)writes the help topics. The ones that name keys ask the registry
    // for the chords bound now, and are rewritten each time help is shown.
    void add_help_topics();
    // "F2 adds, F3 edits and F9 moves": each bound command's chord with its
    // phrase, in order; a command with no chord bound is left out.
    std::string key_phrases(const std::vector<std::pair<ckv::ui::CommandId, std::string>>& phrases,
                            std::string_view separator, std::string_view last_separator) const;
    void show_help_topic(std::string key);
    void show_about();
    void present_welcome();
    void request_quit();

    void present_task_dialog(bool editing);
    void accept_task_dialog(bool editing, std::optional<TaskId> task_id,
                            std::string preserved_note, const ckv::widgets::DialogResult& result);
    void present_archive_confirmation();
    void present_delete_confirmation();
    void open_note_editor();
    void schedule_note_save(TaskId task_id);
    bool flush_note(TaskId task_id);
    void retry_note_over_external(TaskId task_id);
    void adopt_external_note(TaskId task_id);
    void close_note_session(NoteSession* session);
    void sync_note_sessions_after_reload();
    void refresh_note_footer(NoteSession& session);
    NoteSession* focused_note() const noexcept;

    void present_lane_actions();
    void present_lane_rename();
    void present_lane_color();
    void present_lane_sort();
    void present_lane_insert(bool before_active);
    void present_lane_merge();
    void present_lane_archive();
    void set_lane_sort(SortMode sort);

    void present_board_manager();
    void present_board_action_source(bool rename, bool merge);
    void present_new_board();
    void present_board_rename(BoardId board_id);
    void present_board_merge(BoardId board_id);
    void present_board_archive(BoardId board_id);
    void switch_board(BoardId board_id);

    void toggle_move();
    void cancel_move();
    void drop_task(TaskId task_id, LaneId target_lane_id, std::optional<TaskId> before_task_id);
    void show_task_context_menu(ckv::Point screen_cell);
    void show_lane_context_menu(ckv::Point screen_cell);
    void set_theme(TodoTheme theme);
    void refresh_status_items();
    void show_error(std::string title, const ControllerError& error);
    bool handle_conflict(std::string title,
                         const ControllerError& error,
                         std::function<bool(const TodoWorkspace&)> can_rebase,
                         std::function<void()> retry,
                         std::function<void()> use_remote = {});
    void present_conflict_resolution();
    void set_status(std::string message);

    const Task* selected_task() const noexcept;
    std::optional<LaneId> active_lane() const noexcept;
    const Lane* active_lane_record() const noexcept;
    const Board* active_board() const noexcept;
    std::optional<BoardId> board_named(std::string_view name) const noexcept;
    TaskDraft draft_from(const Task& task) const;

    ckv::ui::Application& app_;
    CalendarClock& calendar_;
    TodoController controller_;
    TodoAppOptions options_;
    ckv::ui::StandardRoles roles_;
    ckv::widgets::MemoryHelpProvider help_;

    ckv::widgets::Desktop* desktop_ = nullptr;
    ckv::widgets::MenuBar* menu_bar_ = nullptr;
    ckv::widgets::StatusLine* status_line_ = nullptr;
    ckv::widgets::Window* board_window_ = nullptr;
    TodoBoardView* board_view_ = nullptr;

    ckv::ui::CommandId add_task_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId edit_task_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId edit_note_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId note_undo_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId note_redo_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId note_cut_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId note_copy_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId note_paste_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId note_find_selection_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId note_find_next_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId note_wrap_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId archive_task_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId delete_task_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId move_task_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId cancel_move_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId lane_actions_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId lane_rename_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId lane_color_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId lane_insert_left_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId lane_insert_right_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId lane_merge_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId lane_archive_command_ = ckv::ui::kInvalidCommand;
    std::array<ckv::ui::CommandId, 6> lane_sort_commands_{};
    std::array<ckv::ui::CommandId, 5> theme_commands_{};
    ckv::ui::CommandId board_manager_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId new_board_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId keyboard_help_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId about_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId resolve_conflict_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId board_help_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId board_quit_command_ = ckv::ui::kInvalidCommand;

    std::optional<PendingTaskMove> move_;
    std::unique_ptr<PendingUiConflict> pending_conflict_;
    TodoTheme theme_ = TodoTheme::Classic;
    std::vector<std::unique_ptr<NoteSession>> notes_;
    ckv::ui::Application::TimerId poll_timer_ = 0;
    ckv::ui::Application::TimerId status_timer_ = 0;

    std::optional<ckv::widgets::DescriptorDialogPresentation> welcome_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> task_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> archive_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> delete_confirmation_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> lane_actions_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> lane_edit_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> lane_confirmation_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> board_manager_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> board_action_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> board_edit_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> board_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> message_box_;
    std::optional<ckv::widgets::MessageBoxPresentation> about_box_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> conflict_dialog_;
    std::optional<ckv::widgets::HelpViewerPresentation> help_viewer_;
};

}  // namespace ck::todo
