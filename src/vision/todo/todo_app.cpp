// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_app.hpp"

#include "host_about.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cvision/core/text.hpp"
#include "cvision/core/version.hpp"
#include "cvision/widgets/common_components.hpp"
#include "cvision/widgets/desktop.hpp"
#include "cvision/widgets/editor_document.hpp"
#include "cvision/widgets/menu.hpp"
#include "cvision/widgets/status_line.hpp"
#include "cvision/widgets/text_editor.hpp"
#include "cvision/widgets/window.hpp"

namespace ck::todo {
namespace {

constexpr std::int64_t kNoteSaveDelayNanos = 150'000'000;
constexpr std::int64_t kRepositoryPollNanos = 1'000'000'000;
constexpr std::int64_t kStatusHintNanos = 3'000'000'000;

const std::array<std::string, 4> kPriorityNames = {"High", "Normal", "Low", "Idle"};
const std::array<std::string, 6> kSortNames = {"Manual", "Color", "Due", "Created", "Modified", "Priority"};
const std::array<std::string_view, 6> kSortCommandKeys = {
    "todo.lane-sort.manual", "todo.lane-sort.color", "todo.lane-sort.due",
    "todo.lane-sort.created", "todo.lane-sort.modified", "todo.lane-sort.priority"};
const std::array<std::string, 5> kThemeNames = {
    "&Classic", "&Dark", "&Light", "&Mono", "&High contrast"};
const std::array<std::string, 17> kColorNames = {
    "None", "Black", "Dark blue", "Dark green", "Dark cyan", "Dark red", "Dark magenta", "Brown",
    "Light gray", "Dark gray", "Blue", "Green", "Cyan", "Red", "Magenta", "Yellow", "White"};

enum class LaneDialogAction { Rename, Color, Sort, InsertLeft, InsertRight, Merge, Archive };
enum class BoardDialogAction { Switch, Create, Rename, Merge, Archive };

int fitted_extent(int available, int preferred, int minimum, int margin) {
    available = std::max(0, available);
    const int lower = std::min(minimum, available);
    const int upper = std::min(preferred, available);
    return std::clamp(available - margin, lower, upper);
}

ckv::Rect centered_window_bounds(ckv::Rect area, ckv::Size preferred, ckv::Size minimum, ckv::Size margin) {
    const int width = fitted_extent(area.width, preferred.width, minimum.width, margin.width);
    const int height = fitted_extent(area.height, preferred.height, minimum.height, margin.height);
    return ckv::Rect{std::clamp((area.width - width) / 2, area.x, area.right() - width),
                std::clamp((area.height - height) / 2, area.y, area.bottom() - height), width, height};
}

ckv::Rect cascaded_window_bounds(ckv::Rect area, ckv::Size preferred, ckv::Size minimum, ckv::Point requested) {
    const int width = fitted_extent(area.width, preferred.width, minimum.width, 0);
    const int height = fitted_extent(area.height, preferred.height, minimum.height, 0);
    return ckv::Rect{std::clamp(requested.x, area.x, area.right() - width),
                std::clamp(requested.y, area.y, area.bottom() - height), width, height};
}

std::size_t lane_count(const TodoWorkspace& workspace) {
    std::size_t count = 0;
    for (const Board& board : workspace.snapshot().boards) count += board.lanes.size();
    return count;
}

ckv::widgets::FieldDescriptor combo_field(std::string label, std::vector<std::string> options, int selection) {
    ckv::widgets::FieldDescriptor field;
    field.label = std::move(label);
    field.kind = ckv::widgets::FieldKind::Combo;
    field.options = std::move(options);
    field.initial_selection = selection;
    return field;
}

ckv::widgets::FieldDescriptor radio_field(std::string label, std::vector<std::string> options, int selection) {
    ckv::widgets::FieldDescriptor field;
    field.label = std::move(label);
    field.kind = ckv::widgets::FieldKind::Radio;
    field.options = std::move(options);
    field.initial_selection = selection;
    return field;
}

std::vector<std::string> strings(const auto& values) {
    return std::vector<std::string>(values.begin(), values.end());
}

std::optional<TodoColor> color_from_selection(int selection) {
    if (selection <= 0 || selection > 16) return std::nullopt;
    return static_cast<TodoColor>(selection - 1);
}

int color_selection(std::optional<TodoColor> color) {
    return color ? static_cast<int>(*color) + 1 : 0;
}

std::optional<ckv::widgets::DateValue> date_value(const std::optional<IsoDate>& date) {
    return date ? ckv::widgets::parse_iso_date(date->value) : std::nullopt;
}

std::optional<ckv::widgets::DateValue> local_date_value(CalendarClock& calendar) {
    const CalendarReadResult reading = calendar.read();
    return reading ? ckv::widgets::parse_iso_date(reading.value->local_date.value) : std::nullopt;
}

std::optional<ckv::widgets::TimeValue> time_value(const std::optional<IsoTime>& time) {
    return time ? ckv::widgets::parse_iso_time(time->value) : std::nullopt;
}

std::optional<ckv::widgets::TimeValue> local_time_value(CalendarClock& calendar) {
    const CalendarReadResult reading = calendar.read();
    return reading ? ckv::widgets::parse_iso_time(reading.value->local_time.value) : std::nullopt;
}

bool same_status_items(const std::vector<ckv::widgets::StatusLineItem>& left,
                       const std::vector<ckv::widgets::StatusLineItem>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        if (a.label != b.label || a.command != b.command || a.priority != b.priority ||
            a.presentation.command != b.presentation.command ||
            a.presentation.label != b.presentation.label ||
            a.presentation.chord != b.presentation.chord)
            return false;
    }
    return true;
}

}  // namespace

struct TodoApp::NoteSession {
    TaskId task_id;
    ckv::widgets::Window* window = nullptr;
    std::shared_ptr<ckv::widgets::EditorDocument> document;
    ckv::widgets::EditorDocument::ObserverId observer = 0;
    ckv::widgets::TextEditor* editor = nullptr;
    ckv::ui::Application::TimerId timer = 0;
    std::string last_saved;
    std::string save_state = "Saved";
    bool applying_remote = false;
};

struct TodoApp::PendingUiConflict {
    std::string title;
    std::string message;
    std::function<void()> retry;
    std::function<void()> use_remote;
};

TodoApp::TodoApp(ckv::ui::Application& app,
                 TodoRepository& repository,
                 CalendarClock& calendar,
                 std::string identity,
                 TodoAppOptions options)
    : app_(app),
      calendar_(calendar),
      controller_(repository, calendar, std::move(identity)),
      options_(std::move(options)),
      roles_(ckv::ui::intern_standard_roles(app.roles())) {
    app_.theme() = ckv::ui::make_classic_theme(app_.roles(), roles_);
    auto desktop = std::make_unique<ckv::widgets::Desktop>(app_.root().bounds());
    desktop_ = desktop.get();
    app_.root().add(std::move(desktop));

    declare_commands();
    app_.commands().set_handler(app_.commands().standard().quit, [this] { request_quit(); });
    build_chrome();
    build_board_window();
    install_help();

    const auto opened = controller_.open();
    if (!opened) {
        show_error("Cannot open TODO workspace", opened.error);
    } else if (*opened.value == WorkspaceOpenState::Missing) {
        if (options_.first_workspace)
            initialize_workspace(*options_.first_workspace);
        else
            present_welcome();
    } else {
        refresh_board();
    }

    poll_timer_ = app_.start_timer(kRepositoryPollNanos, true, [this] { poll_repository(); });
}

TodoApp::~TodoApp() {
    if (poll_timer_ != 0) app_.cancel_timer(poll_timer_);
    if (status_timer_ != 0) app_.cancel_timer(status_timer_);
    while (!notes_.empty()) {
        std::unique_ptr<NoteSession> note = std::move(notes_.back());
        notes_.pop_back();
        if (note->timer != 0) app_.cancel_timer(note->timer);
        if (note->document != nullptr && note->observer != 0) note->document->unsubscribe(note->observer);
        if (note->window != nullptr) {
            note->window->close_request = {};
            note->window->on_closed = {};
            desktop_->remove_window(note->window);
        }
    }
    app_.set_help_provider({});
    if (desktop_ != nullptr && desktop_->parent() != nullptr) app_.root().remove_child(desktop_);
    desktop_ = nullptr;
    app_.commands().withdraw(add_task_command_);
    app_.commands().withdraw(edit_task_command_);
    app_.commands().withdraw(edit_note_command_);
    app_.commands().withdraw(note_undo_command_);
    app_.commands().withdraw(note_redo_command_);
    app_.commands().withdraw(note_cut_command_);
    app_.commands().withdraw(note_copy_command_);
    app_.commands().withdraw(note_paste_command_);
    app_.commands().withdraw(note_find_selection_command_);
    app_.commands().withdraw(note_find_next_command_);
    app_.commands().withdraw(note_wrap_command_);
    app_.commands().withdraw(archive_task_command_);
    app_.commands().withdraw(delete_task_command_);
    app_.commands().withdraw(move_task_command_);
    app_.commands().withdraw(cancel_move_command_);
    app_.commands().withdraw(lane_actions_command_);
    app_.commands().withdraw(lane_rename_command_);
    app_.commands().withdraw(lane_color_command_);
    app_.commands().withdraw(lane_insert_left_command_);
    app_.commands().withdraw(lane_insert_right_command_);
    app_.commands().withdraw(lane_merge_command_);
    app_.commands().withdraw(lane_archive_command_);
    for (const ckv::ui::CommandId command : lane_sort_commands_) app_.commands().withdraw(command);
    for (const ckv::ui::CommandId command : theme_commands_) app_.commands().withdraw(command);
    app_.commands().withdraw(board_manager_command_);
    app_.commands().withdraw(new_board_command_);
    app_.commands().withdraw(keyboard_help_command_);
    app_.commands().withdraw(about_command_);
    app_.commands().withdraw(resolve_conflict_command_);
    app_.commands().withdraw(board_help_command_);
    app_.commands().withdraw(board_quit_command_);
    ckv::ui::Application* const app = &app_;
    app_.commands().set_handler(app_.commands().standard().quit, [app] { app->request_quit(); });
}

void TodoApp::declare_commands() {
    // The board's verbs belong to the lanes; the note verbs to the note editor.
    const ckv::ui::CommandScope board_scope{.contexts = {"todo.board"}};
    const ckv::ui::CommandScope note_scope{.contexts = {"todo.note"}};
    add_task_command_ = app_.commands().declare(
        {.key = std::string(kAddTaskKey), .title = "&Add task...", .category = "Tasks", .scope = board_scope,
         .handler = [this] { present_task_dialog(false); }});
    edit_task_command_ = app_.commands().declare(
        {.key = std::string(kEditTaskKey), .title = "&Edit task...", .category = "Tasks", .scope = board_scope,
         .handler = [this] { present_task_dialog(true); }});
    edit_note_command_ = app_.commands().declare(
        {.key = std::string(kEditNoteKey), .title = "Edit &note", .category = "Tasks", .scope = board_scope,
         .handler = [this] { open_note_editor(); }});
    note_undo_command_ = app_.commands().declare(
        {.key = "todo.note.undo", .title = "&Undo", .category = "Note", .scope = note_scope,
         .handler = [this] {
             if (NoteSession* note = focused_note()) (void)note->editor->perform(ckv::widgets::EditorCommand::Undo);
         }});
    note_redo_command_ = app_.commands().declare(
        {.key = "todo.note.redo", .title = "&Redo", .category = "Note", .scope = note_scope,
         .handler = [this] {
             if (NoteSession* note = focused_note()) (void)note->editor->perform(ckv::widgets::EditorCommand::Redo);
         }});
    note_cut_command_ = app_.commands().declare(
        {.key = "todo.note.cut", .title = "Cu&t", .category = "Note", .scope = note_scope,
         .handler = [this] {
             if (NoteSession* note = focused_note()) (void)note->editor->cut_selection_to_clipboard();
         }});
    note_copy_command_ = app_.commands().declare(
        {.key = "todo.note.copy", .title = "&Copy", .category = "Note", .scope = note_scope,
         .handler = [this] {
             if (NoteSession* note = focused_note()) (void)note->editor->copy_selection_to_clipboard();
         }});
    note_paste_command_ = app_.commands().declare(
        {.key = "todo.note.paste", .title = "&Paste", .category = "Note", .scope = note_scope,
         .handler = [this] {
             if (NoteSession* note = focused_note()) (void)note->editor->paste_from_clipboard();
         }});
    note_find_selection_command_ = app_.commands().declare(
        {.key = "todo.note.find-selection", .title = "&Find selection", .category = "Note",
         .scope = note_scope, .handler = [this] {
             if (NoteSession* note = focused_note()) (void)note->editor->use_selection_as_search_query();
         }});
    note_find_next_command_ = app_.commands().declare(
        {.key = "todo.note.find-next", .title = "Find &next", .category = "Note", .scope = note_scope,
         .handler = [this] {
             if (NoteSession* note = focused_note()) (void)note->editor->find_next();
         }});
    note_wrap_command_ = app_.commands().declare(
        {.key = "todo.note.wrap", .title = "&Word wrap", .category = "Note", .scope = note_scope,
         .handler = [this] {
             if (NoteSession* note = focused_note()) {
                 const bool wrapped = note->editor->wrap_mode() == ckv::widgets::WrapMode::Word;
                 note->editor->set_wrap_mode(wrapped ? ckv::widgets::WrapMode::None : ckv::widgets::WrapMode::Word);
                 if (menu_bar_ != nullptr) menu_bar_->set_menus(build_menus());
             }
         }});
    archive_task_command_ = app_.commands().declare(
        {.key = std::string(kArchiveTaskKey), .title = "&Archive task...", .category = "Tasks", .scope = board_scope,
         .handler = [this] { present_archive_confirmation(); }});
    delete_task_command_ = app_.commands().declare(
        {.key = std::string(kDeleteTaskKey), .title = "&Delete task...", .category = "Tasks", .scope = board_scope,
         .handler = [this] { present_delete_confirmation(); }});
    move_task_command_ = app_.commands().declare(
        {.key = std::string(kMoveTaskKey), .title = "&Move task", .category = "Tasks", .scope = board_scope,
         .handler = [this] { toggle_move(); }});
    cancel_move_command_ = app_.commands().declare(
        {.key = std::string(kCancelMoveKey), .title = "Cancel move", .category = "Tasks", .scope = board_scope,
         .visibility = ckv::ui::CommandVisibility::Hidden, .handler = [this] { cancel_move(); }});
    lane_actions_command_ = app_.commands().declare(
        {.key = std::string(kLaneActionsKey), .title = "Lane &actions...", .category = "Lanes", .scope = board_scope,
         .handler = [this] { present_lane_actions(); }});
    lane_rename_command_ = app_.commands().declare(
        {.key = "todo.rename-lane", .title = "&Rename...", .category = "Lanes", .scope = board_scope,
         .handler = [this] { present_lane_rename(); }});
    lane_color_command_ = app_.commands().declare(
        {.key = "todo.color-lane", .title = "&Color...", .category = "Lanes", .scope = board_scope,
         .handler = [this] { present_lane_color(); }});
    lane_insert_left_command_ = app_.commands().declare(
        {.key = "todo.insert-lane-left", .title = "Insert &left...", .category = "Lanes", .scope = board_scope,
         .handler = [this] { present_lane_insert(true); }});
    lane_insert_right_command_ = app_.commands().declare(
        {.key = "todo.insert-lane-right", .title = "Insert &right...", .category = "Lanes", .scope = board_scope,
         .handler = [this] { present_lane_insert(false); }});
    lane_merge_command_ = app_.commands().declare(
        {.key = "todo.merge-lane", .title = "&Merge into...", .category = "Lanes", .scope = board_scope,
         .handler = [this] { present_lane_merge(); }});
    lane_archive_command_ = app_.commands().declare(
        {.key = "todo.archive-lane", .title = "Archive lane...", .category = "Lanes", .scope = board_scope,
         .handler = [this] { present_lane_archive(); }});
    for (std::size_t index = 0; index < lane_sort_commands_.size(); ++index) {
        const SortMode sort = static_cast<SortMode>(index);
        lane_sort_commands_[index] = app_.commands().declare(
            {.key = std::string(kSortCommandKeys[index]), .title = kSortNames[index], .category = "Lanes",
             .scope = board_scope, .handler = [this, sort] { set_lane_sort(sort); }});
    }
    for (std::size_t index = 0; index < theme_commands_.size(); ++index) {
        const TodoTheme theme = static_cast<TodoTheme>(index);
        theme_commands_[index] = app_.commands().declare(
            {.key = std::string(kThemeKeys[index]), .title = kThemeNames[index],
             .category = "View", .handler = [this, theme] { set_theme(theme); }});
    }
    board_manager_command_ = app_.commands().declare(
        {.key = std::string(kBoardManagerKey), .title = "&Board Manager...", .category = "Boards",
         .scope = board_scope, .handler = [this] { present_board_manager(); }});
    new_board_command_ = app_.commands().declare(
        {.key = std::string(kNewBoardKey), .title = "&New Board...", .category = "Boards",
         .scope = board_scope, .handler = [this] { present_new_board(); }});
    keyboard_help_command_ = app_.commands().declare(
        {.key = "todo.keyboard-help", .title = "&Keyboard reference", .category = "Help",
         .handler = [this] { show_help_topic("todo.keyboard"); }});
    about_command_ = app_.commands().declare(
        {.key = "todo.about", .title = "&About ck-todo", .category = "Help",
         .handler = [this] { show_about(); }});
    resolve_conflict_command_ = app_.commands().declare(
        {.key = "todo.resolve-conflict", .title = "&Resolve conflict...", .category = "File",
         .handler = [this] { present_conflict_resolution(); }});
    board_help_command_ = app_.commands().declare(
        {.key = "todo.board-help", .title = "Board help", .category = "Help", .scope = board_scope,
         .visibility = ckv::ui::CommandVisibility::Hidden,
         .handler = [this] { app_.execute_command(app_.commands().standard().help); }});
    board_quit_command_ = app_.commands().declare(
        {.key = "todo.board-quit", .title = "Board quit", .category = "File", .scope = board_scope,
         .visibility = ckv::ui::CommandVisibility::Hidden,
         .handler = [this] { app_.execute_command(app_.commands().standard().quit); }});

    app_.commands().set_enabled_predicate(add_task_command_, [this] { return active_lane().has_value() && !move_; });
    const auto task_available = [this] { return selected_task() != nullptr && !move_; };
    app_.commands().set_enabled_predicate(edit_task_command_, task_available);
    app_.commands().set_enabled_predicate(edit_note_command_, task_available);
    app_.commands().set_enabled_predicate(note_undo_command_, [this] {
        const NoteSession* note = focused_note();
        return note != nullptr && note->document->can_undo();
    });
    app_.commands().set_enabled_predicate(note_redo_command_, [this] {
        const NoteSession* note = focused_note();
        return note != nullptr && note->document->can_redo();
    });
    app_.commands().set_enabled_predicate(note_cut_command_, [this] {
        const NoteSession* note = focused_note();
        return note != nullptr && !note->editor->read_only() && note->editor->selection().has_value();
    });
    app_.commands().set_enabled_predicate(note_copy_command_, [this] {
        const NoteSession* note = focused_note();
        return note != nullptr && note->editor->selection().has_value();
    });
    app_.commands().set_enabled_predicate(note_paste_command_, [this] {
        const NoteSession* note = focused_note();
        return note != nullptr && !note->editor->read_only();
    });
    app_.commands().set_enabled_predicate(note_find_selection_command_, [this] {
        const NoteSession* note = focused_note();
        return note != nullptr && note->editor->selection().has_value();
    });
    app_.commands().set_enabled_predicate(note_find_next_command_, [this] {
        const NoteSession* note = focused_note();
        return note != nullptr && note->editor->search_match_count() != 0U;
    });
    app_.commands().set_enabled_predicate(note_wrap_command_, [this] { return focused_note() != nullptr; });
    app_.commands().set_enabled_predicate(archive_task_command_, task_available);
    app_.commands().set_enabled_predicate(delete_task_command_, task_available);
    app_.commands().set_enabled_predicate(move_task_command_,
                                          [this] { return move_.has_value() || selected_task() != nullptr; });
    app_.commands().set_enabled_predicate(cancel_move_command_, [this] { return move_.has_value(); });
    const auto lane_available = [this] { return active_lane().has_value() && !move_; };
    app_.commands().set_enabled_predicate(lane_actions_command_, lane_available);
    app_.commands().set_enabled_predicate(lane_rename_command_, lane_available);
    app_.commands().set_enabled_predicate(lane_color_command_, lane_available);
    app_.commands().set_enabled_predicate(lane_insert_left_command_, lane_available);
    app_.commands().set_enabled_predicate(lane_insert_right_command_, lane_available);
    app_.commands().set_enabled_predicate(lane_merge_command_, [this] {
        const Board* board = active_board();
        return board != nullptr && board->lanes.size() > 1 && !move_;
    });
    app_.commands().set_enabled_predicate(lane_archive_command_, [this] {
        const Board* board = active_board();
        return board != nullptr && board->lanes.size() > 1 && !move_;
    });
    for (const ckv::ui::CommandId command : lane_sort_commands_)
        app_.commands().set_enabled_predicate(command, lane_available);
    app_.commands().set_enabled_predicate(board_manager_command_, [this] {
        return controller_.workspace() != nullptr && !move_;
    });
    app_.commands().set_enabled_predicate(new_board_command_, [this] {
        return controller_.workspace() != nullptr && !move_;
    });
    app_.commands().set_enabled_predicate(resolve_conflict_command_, [this] {
        return pending_conflict_ != nullptr;
    });
    bind_board_aliases();
}

void TodoApp::bind_board_aliases() {
    const auto bind = [this](ckv::Key key, std::string text, ckv::ui::CommandId command,
                             ckv::Modifier modifiers = ckv::Modifier::None) {
        app_.commands().bind_key(ckv::KeyChord{key, modifiers, std::move(text)}, command);
    };
    bind(ckv::Key::F2, {}, add_task_command_);
    bind(ckv::Key::Insert, {}, add_task_command_);
    bind(ckv::Key::Char, "+", add_task_command_);
    bind(ckv::Key::F3, {}, edit_task_command_);
    bind(ckv::Key::Char, "e", edit_task_command_);
    bind(ckv::Key::F4, {}, edit_note_command_);
    bind(ckv::Key::Char, "n", edit_note_command_);
    bind(ckv::Key::F8, {}, archive_task_command_);
    bind(ckv::Key::Char, "a", archive_task_command_);
    bind(ckv::Key::Delete, {}, delete_task_command_);
    bind(ckv::Key::Char, "d", delete_task_command_);
    bind(ckv::Key::F9, {}, move_task_command_);
    bind(ckv::Key::Enter, {}, move_task_command_);
    bind(ckv::Key::Char, " ", move_task_command_);
    bind(ckv::Key::Escape, {}, cancel_move_command_);
    bind(ckv::Key::F7, {}, lane_actions_command_);
    bind(ckv::Key::Char, "m", board_manager_command_);
    bind(ckv::Key::Char, "h", board_help_command_);
    bind(ckv::Key::Char, "?", board_help_command_);
    bind(ckv::Key::Char, "q", board_quit_command_);
    bind(ckv::Key::Char, "z", note_undo_command_, ckv::Modifier::Ctrl);
    bind(ckv::Key::Char, "y", note_redo_command_, ckv::Modifier::Ctrl);
    bind(ckv::Key::Char, "x", note_cut_command_, ckv::Modifier::Ctrl);
    bind(ckv::Key::Char, "c", note_copy_command_, ckv::Modifier::Ctrl);
    bind(ckv::Key::Char, "v", note_paste_command_, ckv::Modifier::Ctrl);
    bind(ckv::Key::Char, "f", note_find_selection_command_, ckv::Modifier::Ctrl);
    bind(ckv::Key::Char, "g", note_find_next_command_, ckv::Modifier::Ctrl);
    bind(ckv::Key::Char, "w", note_wrap_command_, ckv::Modifier::Alt);
}

std::vector<ckv::widgets::MenuBarItem> TodoApp::build_menus() {
    ckv::widgets::MenuBarItem tasks{"&Tasks",
                               {ckv::widgets::MenuItem::command(add_task_command_),
                                ckv::widgets::MenuItem::command(edit_task_command_),
                                ckv::widgets::MenuItem::command(edit_note_command_),
                                ckv::widgets::MenuItem::separator(),
                                ckv::widgets::MenuItem::command(archive_task_command_),
                                ckv::widgets::MenuItem::command(delete_task_command_),
                                ckv::widgets::MenuItem::command(move_task_command_)}};
    const NoteSession* active_note = focused_note();
    ckv::widgets::MenuBarItem note{
        "&Note",
        {ckv::widgets::MenuItem::command(note_undo_command_), ckv::widgets::MenuItem::command(note_redo_command_),
         ckv::widgets::MenuItem::separator(), ckv::widgets::MenuItem::command(note_cut_command_),
         ckv::widgets::MenuItem::command(note_copy_command_), ckv::widgets::MenuItem::command(note_paste_command_),
         ckv::widgets::MenuItem::separator(), ckv::widgets::MenuItem::command(note_find_selection_command_),
         ckv::widgets::MenuItem::command(note_find_next_command_), ckv::widgets::MenuItem::separator(),
         ckv::widgets::MenuItem::command(note_wrap_command_)
             .with_mark(active_note != nullptr && active_note->editor->wrap_mode() == ckv::widgets::WrapMode::Word
                            ? ckv::widgets::MenuMark::Checked
                            : ckv::widgets::MenuMark::Unchecked)}};
    const SortMode active_sort = active_lane_record() != nullptr ? active_lane_record()->sort : SortMode::Manual;
    std::vector<ckv::widgets::MenuItem> sort_items;
    for (std::size_t index = 0; index < lane_sort_commands_.size(); ++index) {
        sort_items.push_back(ckv::widgets::MenuItem::command(lane_sort_commands_[index])
                                 .with_mark(static_cast<SortMode>(index) == active_sort
                                                ? ckv::widgets::MenuMark::RadioOn
                                                : ckv::widgets::MenuMark::RadioOff));
    }
    ckv::widgets::MenuBarItem lane{
        "&Lane",
        {ckv::widgets::MenuItem::submenu("&Sort", std::move(sort_items)),
         ckv::widgets::MenuItem::command(lane_color_command_), ckv::widgets::MenuItem::command(lane_rename_command_),
         ckv::widgets::MenuItem::command(lane_insert_left_command_),
         ckv::widgets::MenuItem::command(lane_insert_right_command_), ckv::widgets::MenuItem::separator(),
         ckv::widgets::MenuItem::command(lane_merge_command_), ckv::widgets::MenuItem::command(lane_archive_command_)}};
    std::vector<ckv::widgets::MenuItem> theme_items;
    for (std::size_t index = 0; index < theme_commands_.size(); ++index) {
        theme_items.push_back(
            ckv::widgets::MenuItem::command(theme_commands_[index])
                .with_mark(static_cast<TodoTheme>(index) == theme_ ? ckv::widgets::MenuMark::RadioOn
                                                                   : ckv::widgets::MenuMark::RadioOff));
    }
    ckv::widgets::MenuBarItem view{
        "&View",
        {ckv::widgets::MenuItem::submenu("&Theme", std::move(theme_items)),
         ckv::widgets::MenuItem::submenu(
             "&Window",
             {ckv::widgets::MenuItem::command(app_.commands().standard().next_window),
              ckv::widgets::MenuItem::command(app_.commands().standard().previous_window),
              ckv::widgets::MenuItem::separator(), ckv::widgets::MenuItem::command(app_.commands().standard().zoom),
              ckv::widgets::MenuItem::command(app_.commands().standard().minimize),
              ckv::widgets::MenuItem::command(app_.commands().standard().window_list),
              ckv::widgets::MenuItem::separator(), ckv::widgets::MenuItem::command(app_.commands().standard().tile_grid),
              ckv::widgets::MenuItem::command(app_.commands().standard().cascade)})}};
    ckv::widgets::MenuBarItem help{
        "&Help", {ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{app_.commands().standard().help,
                                                                           "&Context Help"}),
                  ckv::widgets::MenuItem::command(keyboard_help_command_), ckv::widgets::MenuItem::separator(),
                  ckv::widgets::MenuItem::command(about_command_)}};
    ckv::widgets::MenuBarItem file{
        "&File", {ckv::widgets::MenuItem::command(board_manager_command_), ckv::widgets::MenuItem::command(new_board_command_),
                  ckv::widgets::MenuItem::command(
                      ckv::widgets::CommandPresentation{about_command_, "Workspace &information..."}),
                  ckv::widgets::MenuItem::command(resolve_conflict_command_),
                  ckv::widgets::MenuItem::separator(),
                  ckv::widgets::MenuItem::command(
                      ckv::widgets::CommandPresentation{app_.commands().standard().quit, "E&xit"})}};
    return {std::move(file), std::move(tasks), std::move(note), std::move(lane), std::move(view), std::move(help)};
}

void TodoApp::build_chrome() {
    auto menu = std::make_unique<ckv::widgets::MenuBar>(build_menus());
    menu_bar_ = menu.get();
    desktop_->dock_top(std::move(menu));

    auto status = std::make_unique<ckv::widgets::StatusLine>();
    status_line_ = status.get();
    status->set_hint_provider([this](const std::string& key) {
        if (move_) {
            const auto lane_id = active_lane();
            const TodoWorkspace* workspace = controller_.workspace();
            const Lane* lane = lane_id && workspace != nullptr ? workspace->find_lane(*lane_id) : nullptr;
            if (lane != nullptr && lane->sort != SortMode::Manual)
                return std::string{"Sorted lane: Left/Right changes lane; displayed order follows its sort."};
            return std::string{"Arrows choose the insertion point; Enter commits; Esc cancels."};
        }
        if (key == "todo.board") return std::string{"Arrows select tasks and lanes; Enter starts or finishes a move."};
        if (key == "todo.note") return std::string{"Notes autosave after a short pause and flush on close."};
        return std::string{};
    });
    desktop_->dock_bottom(std::move(status));
    refresh_status_items();
}

void TodoApp::build_board_window() {
    auto window = std::make_unique<ckv::widgets::Window>("TODO");
    const ckv::Rect area = desktop_->content_area();
    window->set_bounds(centered_window_bounds(area, ckv::Size{92, 24}, ckv::Size{30, 8}, ckv::Size{4, 2}));
    window->set_min_size(ckv::Size{std::min(30, area.width), std::min(8, area.height)});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::AnchorEdges);
    auto board = std::make_unique<TodoBoardView>();
    board_view_ = board.get();
    board->on_task_activate = [this](TaskId) { present_task_dialog(true); };
    board->on_move_toggle = [this](TaskId) { toggle_move(); };
    board->on_task_drop = [this](TaskId task_id, LaneId lane_id, std::optional<TaskId> before) {
        drop_task(task_id, lane_id, before);
    };
    board->on_task_context_menu = [this](TaskId, ckv::Point screen_cell) { show_task_context_menu(screen_cell); };
    board->on_lane_context_menu = [this](LaneId, ckv::Point screen_cell) { show_lane_context_menu(screen_cell); };
    board->on_active_lane_changed = [this](LaneId lane_id) {
        if (move_) board_view_->set_move_target(lane_id);
        if (menu_bar_ != nullptr) menu_bar_->set_menus(build_menus());
    };
    board->on_move_position_blocked = [this] {
        set_status("This lane is sorted automatically; Left/Right changes lanes, and its sort chooses the position.");
    };
    window->set_content(std::move(board));
    window->close_request = [this] {
        request_quit();
        return false;
    };
    board_window_ = desktop_->add_window(std::move(window));
}

void TodoApp::install_help() {
    add_help_topics();
    app_.set_help_provider([this](const std::string& key) {
        show_help_topic(key.empty() ? "todo.board" : key);
    });
}

std::string TodoApp::key_phrases(const std::vector<std::pair<ckv::ui::CommandId, std::string>>& phrases,
                                 std::string_view separator, std::string_view last_separator) const {
    std::vector<std::string> bound;
    for (const auto& [command, phrase] : phrases) {
        const std::string chord = app_.commands().chord_text(command);
        if (!chord.empty()) bound.push_back(chord + " " + phrase);
    }
    std::string text;
    for (std::size_t index = 0; index < bound.size(); ++index) {
        if (index != 0) text += index + 1 == bound.size() ? last_separator : separator;
        text += bound[index];
    }
    return text;
}

void TodoApp::add_help_topics() {
    const ckv::ui::StandardCommands& standard = app_.commands().standard();
    const std::string task_keys = key_phrases({{add_task_command_, "adds"},
                                               {edit_task_command_, "edits"},
                                               {edit_note_command_, "opens a note"},
                                               {archive_task_command_, "archives"},
                                               {move_task_command_, "starts and finishes move mode"}},
                                              ", ", ", and ");
    help_.add_topic("todo.board", ckv::widgets::HelpTopic{
                                      "TODO board",
                                      {{"Use Up/Down to select a task and Left/Right to change lanes." +
                                            (task_keys.empty() ? std::string{} : " " + task_keys + ".")}},
                                      {{"todo.tasks", "Tasks"}, {"todo.moving", "Moving"},
                                       {"todo.lanes", "Lanes"}, {"todo.boards", "Boards"},
                                       {"todo.keyboard", "Keyboard"}, {"todo.persistence", "Persistence"}}});
    help_.add_topic("todo.tasks", ckv::widgets::HelpTopic{"Tasks", {{"Task changes are durable before the board reports success."}},
                                                       {{"todo.board", "Board"}, {"todo.persistence", "Persistence"}}});
    help_.add_topic("todo.moving", ckv::widgets::HelpTopic{"Moving", {{"Start move mode, choose a lane and insertion task with arrows, then press Enter. Escape cancels."}},
                                                        {{"todo.board", "Board"}}});
    help_.add_topic("todo.persistence", ckv::widgets::HelpTopic{"Persistence", {{"There is no Save command. Changes are atomically committed with daily backups; archive data is written before removal."}},
                                                             {{"todo.board", "Board"}, {"todo.boards", "Boards"}}});
    help_.add_topic("todo.note", ckv::widgets::HelpTopic{"Notes", {{"Note windows are modeless. Editing coalesces for 150 ms and closing flushes pending text."}},
                                                     {{"todo.board", "Board"}}});
    const std::string lane_keys = key_phrases({{lane_actions_command_, "opens lane actions."}}, "", "");
    help_.add_topic("todo.lanes", ckv::widgets::HelpTopic{
                                      "Lanes",
                                      {{(lane_keys.empty() ? std::string{"The Lane menu holds the lane actions."}
                                                           : lane_keys) +
                                            " Lanes can be renamed, colored, sorted, inserted, merged, or archived. "
                                            "Only Manual sorting permits reordering."}},
                                      {{"todo.board", "Board"}, {"todo.keyboard", "Keyboard"}}});
    help_.add_topic("todo.boards", ckv::widgets::HelpTopic{
                                       "Boards",
                                       {{"Press m or click the Board status item to create, switch, rename, merge, or archive Boards. The main Board is protected."}},
                                       {{"todo.board", "Board"}, {"todo.persistence", "Persistence"}}});
    help_.add_topic("todo.keyboard", ckv::widgets::HelpTopic{
                                         "Keyboard reference",
                                         {{key_phrases({{standard.help, "Help"},
                                                        {add_task_command_, "Add"},
                                                        {edit_task_command_, "Edit"},
                                                        {edit_note_command_, "Note"},
                                                        {standard.zoom, "Zoom"},
                                                        {standard.next_window, "Next window"},
                                                        {lane_actions_command_, "Lane actions"},
                                                        {archive_task_command_, "Archive"},
                                                        {move_task_command_, "Move"},
                                                        {standard.menu, "Menu"}},
                                                       "; ", "; ") +
                                               ". Delete removes permanently. m selects a Board; Esc cancels."}},
                                         {{"todo.board", "Board"}, {"todo.lanes", "Lanes"}, {"todo.boards", "Boards"}}});
}

void TodoApp::show_help_topic(std::string key) {
    add_help_topics();
    help_viewer_ = ckv::widgets::present_modeless_help_viewer(help_, std::move(key), app_, *desktop_, roles_);
}

void TodoApp::show_about() {
    const std::string help = app_.commands().chord_text(app_.commands().standard().help);
    about_box_ = ckv::widgets::present_modal_message_box(
        app_, *desktop_, roles_,
        {ckv::widgets::MessageBoxKind::Info, "About ck-todo",
         ck::host::about_text(
             "ck-todo \u2014 boards, lanes and tasks\n" +
             options_.workspace_description +
             "\n\nChanges save automatically." +
             (help.empty() ? std::string{} : " " + help + " opens contextual help.")),
         ckv::widgets::MessageBoxButtons::Ok});
    about_box_->set_completion_handler([](ckv::widgets::MessageBoxResult) {});
}

void TodoApp::present_welcome() {
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Welcome to ck-todo";
    descriptor.help_context_key = "todo.board";
    ckv::widgets::FieldDescriptor choice;
    choice.label = "&Start with";
    choice.kind = ckv::widgets::FieldKind::Radio;
    choice.options = {"Guided sample", "Empty board"};
    choice.initial_selection = 0;
    descriptor.fields.push_back(std::move(choice));
    descriptor.buttons.push_back({"&Start", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Quit", ckv::widgets::ButtonRole::Dismiss, nullptr});
    welcome_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    welcome_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted) {
            request_quit();
            return;
        }
        initialize_workspace(!result.selected.empty() && result.selected[0] == 1 ? InitialWorkspace::Empty
                                                                                 : InitialWorkspace::Guided);
    });
}

void TodoApp::request_quit() {
    std::vector<TaskId> note_tasks;
    note_tasks.reserve(notes_.size());
    for (const auto& note : notes_) note_tasks.push_back(note->task_id);
    for (const TaskId task_id : note_tasks) {
        if (!flush_note(task_id)) {
            set_status("Quit paused until the unsaved note is resolved.");
            return;
        }
    }
    app_.request_quit();
}

void TodoApp::initialize_workspace(InitialWorkspace initial) {
    const auto created = controller_.create(initial);
    if (!created) {
        show_error("Cannot create TODO workspace", created.error);
        return;
    }
    refresh_board();
    set_status("Workspace created and saved.");
}

void TodoApp::refresh_board() {
    const TodoWorkspace* workspace = controller_.workspace();
    if (workspace == nullptr || board_view_ == nullptr) return;
    BoardId board_id = workspace->snapshot().last_board_id;
    std::optional<std::string> missing_board;
    if (options_.initial_board_name) {
        const std::string requested_name = std::move(*options_.initial_board_name);
        options_.initial_board_name.reset();
        if (const auto requested = board_named(requested_name)) {
            if (*requested != board_id) {
                const auto switched = controller_.switch_board(*requested);
                if (!switched) {
                    show_error("Cannot select requested Board", switched.error);
                } else {
                    workspace = controller_.workspace();
                    if (workspace == nullptr) return;
                    board_id = workspace->snapshot().last_board_id;
                }
            }
        } else {
            missing_board = requested_name;
        }
    }
    if (!board_view_->set_board(*workspace, board_id)) return;
    if (move_) {
        board_view_->begin_keyboard_move(move_->task_id, move_->source_lane_id,
                                         move_->original_before_task_id);
    } else {
        board_view_->end_keyboard_move();
    }
    if (const Board* board = workspace->find_board(board_id)) board_window_->set_title("TODO — " + board->name);
    if (menu_bar_ != nullptr) menu_bar_->set_menus(build_menus());
    refresh_status_items();
    sync_note_sessions_after_reload();
    if (missing_board) set_status("Board ‘" + *missing_board + "’ was not found; opened the last Board.");
    if (app_.focused() == nullptr && board_view_->active_lane())
        app_.set_focus(board_view_->lane_view(*board_view_->active_lane()));
}

void TodoApp::poll_repository() {
    refresh_status_items();
    if (!controller_.is_open() || move_ || pending_conflict_ || app_.is_modal()) return;
    const auto reloaded = controller_.reload_if_changed();
    if (!reloaded) {
        set_status("Reload failed: " + reloaded.error.diagnostic);
        return;
    }
    if (reloaded.value->changed) {
        refresh_board();
        set_status("Reloaded changes from another TODO instance.");
    }
}

TaskDraft TodoApp::draft_from(const Task& task) const {
    return TaskDraft{task.title, task.details, task.note, task.priority, task.due_date, task.due_time, task.color};
}

void TodoApp::present_task_dialog(bool editing) {
    const Task* task = editing ? selected_task() : nullptr;
    if (editing && task == nullptr) return;
    const TaskDraft initial = task ? draft_from(*task) : TaskDraft{};
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = editing ? "Edit task" : "Add task";
    descriptor.help_context_key = "todo.tasks";
    descriptor.resizable = true;
    descriptor.minimum_window_size = ckv::Size{46, 17};
    descriptor.fields.push_back(ckv::widgets::FieldDescriptor{
        "&Title:", initial.title, [](const std::string& value) { return !value.empty() && value.size() <= TodoLimits::max_title_bytes; }});
    descriptor.fields.push_back(ckv::widgets::FieldDescriptor{
        "&Details:", initial.details, [](const std::string& value) { return value.size() <= TodoLimits::max_details_bytes; }});
    ckv::widgets::FieldDescriptor due;
    due.label = "&Due:";
    due.kind = ckv::widgets::FieldKind::Date;
    due.initial_date = date_value(initial.due_date);
    due.date_seed = local_date_value(calendar_);
    due.date_optional = true;
    descriptor.fields.push_back(std::move(due));
    ckv::widgets::FieldDescriptor include_time;
    include_time.label = "Set due &time";
    include_time.kind = ckv::widgets::FieldKind::Check;
    include_time.initial_checked = initial.due_time.has_value();
    descriptor.fields.push_back(std::move(include_time));
    ckv::widgets::FieldDescriptor due_time;
    due_time.label = "Due ti&me:";
    due_time.kind = ckv::widgets::FieldKind::Time;
    due_time.initial_time = time_value(initial.due_time).value_or(
        local_time_value(calendar_).value_or(ckv::widgets::TimeValue{}));
    due_time.time_show_seconds = false;
    due_time.time_24_hour = true;
    descriptor.fields.push_back(std::move(due_time));
    descriptor.fields.push_back(combo_field("&Priority:", strings(kPriorityNames), static_cast<int>(initial.priority) - 1));
    descriptor.fields.push_back(combo_field("&Color:", strings(kColorNames), color_selection(initial.color)));
    if (task != nullptr) {
        ckv::widgets::FieldDescriptor created;
        created.kind = ckv::widgets::FieldKind::Note;
        created.label = "Created " + task->created_at.value + " by " + task->created_by;
        descriptor.fields.push_back(std::move(created));
        ckv::widgets::FieldDescriptor modified;
        modified.kind = ckv::widgets::FieldKind::Note;
        modified.label = "Modified " + task->modified_at.value + " by " + task->modified_by;
        descriptor.fields.push_back(std::move(modified));
        descriptor.minimum_window_size.height += 2;
    }
    descriptor.buttons.push_back({editing ? "&Apply" : "&Add", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    const std::optional<TaskId> task_id = task ? std::optional<TaskId>(task->id) : std::nullopt;
    const std::string note = initial.note;
    task_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    task_dialog_->set_completion_handler([this, editing, task_id, note](ckv::widgets::DialogResult result) {
        if (result.accepted) accept_task_dialog(editing, task_id, note, result);
    });
}

void TodoApp::accept_task_dialog(bool editing,
                                 std::optional<TaskId> task_id,
                                 std::string preserved_note,
                                 const ckv::widgets::DialogResult& result) {
    if (result.values.size() < 7 || result.checked.size() < 7 || result.selected.size() < 7 ||
        result.dates.size() < 7 || result.times.size() < 7) {
        return;
    }
    TaskDraft draft;
    draft.title = result.values[0];
    draft.details = result.values[1];
    draft.note = std::move(preserved_note);
    if (result.dates[2]) draft.due_date = IsoDate{ckv::widgets::format_iso_date(*result.dates[2])};
    if (draft.due_date && result.checked[3] && result.times[4]) {
        draft.due_time = IsoTime{ckv::widgets::format_iso_time(*result.times[4], false)};
    }
    const int priority = result.selected[5] >= 0 ? result.selected[5] : 1;
    draft.priority = static_cast<Priority>(std::clamp(priority + 1, 1, 4));
    draft.color = color_from_selection(result.selected[6]);

    if (editing && task_id) {
        const Task* current = controller_.workspace() != nullptr ? controller_.workspace()->find_task(*task_id) : nullptr;
        if (current == nullptr) return;
        const Task original = *current;
        const TaskDraft retry_draft = draft;
        const auto changed = controller_.edit_task(*task_id, std::move(draft));
        if (!changed) {
            const TaskId edited_task_id = *task_id;
            handle_conflict(
                "Task edit conflict", changed.error,
                [edited_task_id, original](const TodoWorkspace& remote) {
                    const Task* remote_task = remote.find_task(edited_task_id);
                    return remote_task != nullptr && *remote_task == original;
                },
                [this, edited_task_id, retry_draft] {
                    const auto retried = controller_.edit_task(edited_task_id, retry_draft);
                    if (!retried) {
                        show_error("Cannot retry task edit", retried.error);
                        return;
                    }
                    refresh_board();
                    board_view_->select_task(edited_task_id);
                    set_status("Task edit rebased and saved.");
                });
            return;
        }
        refresh_board();
        board_view_->select_task(*task_id);
        set_status("Task updated and saved.");
        return;
    }
    const auto lane = active_lane();
    if (!lane) return;
    const TaskDraft retry_draft = draft;
    const auto added = controller_.add_task(*lane, std::move(draft));
    if (!added) {
        handle_conflict(
            "Task add conflict", added.error,
            [lane_id = *lane](const TodoWorkspace& remote) { return remote.find_lane(lane_id) != nullptr; },
            [this, lane_id = *lane, retry_draft] {
                const auto retried = controller_.add_task(lane_id, retry_draft);
                if (!retried) {
                    show_error("Cannot retry task add", retried.error);
                    return;
                }
                refresh_board();
                board_view_->select_task(*retried.value);
                set_status("Task add rebased and saved.");
            });
        return;
    }
    refresh_board();
    board_view_->select_task(*added.value);
    set_status("Task added and saved.");
}

void TodoApp::present_archive_confirmation() {
    const Task* task = selected_task();
    if (task == nullptr) return;
    const TaskId task_id = task->id;
    const Task original = *task;
    archive_confirmation_ = ckv::widgets::present_modal_message_box(
        app_, *desktop_, roles_,
        {ckv::widgets::MessageBoxKind::Confirm, "Archive task", "Archive ‘" + task->title + "’?",
         ckv::widgets::MessageBoxButtons::YesNo});
    archive_confirmation_->set_completion_handler([this, task_id, original](ckv::widgets::MessageBoxResult result) {
        if (result != ckv::widgets::MessageBoxResult::Yes) return;
        const auto archived = controller_.archive_task(task_id);
        if (!archived) {
            handle_conflict(
                "Task archive conflict", archived.error,
                [task_id, original](const TodoWorkspace& remote) {
                    const Task* remote_task = remote.find_task(task_id);
                    return remote_task != nullptr && *remote_task == original;
                },
                [this, task_id] {
                    const auto retried = controller_.archive_task(task_id);
                    if (!retried) {
                        show_error("Cannot retry task archive", retried.error);
                        return;
                    }
                    refresh_board();
                    set_status("Task archive rebased and saved.");
                });
            return;
        }
        refresh_board();
        set_status("Task archived before removal.");
    });
}

void TodoApp::present_delete_confirmation() {
    const Task* task = selected_task();
    if (task == nullptr) return;
    const TaskId task_id = task->id;
    const Task original = *task;
    delete_confirmation_ = ckv::widgets::present_modal_message_box(
        app_, *desktop_, roles_,
        {ckv::widgets::MessageBoxKind::Warning, "Delete task permanently",
         "Permanently delete ‘" + task->title + "’? This does not create an archive.",
         ckv::widgets::MessageBoxButtons::YesNo});
    delete_confirmation_->set_completion_handler([this, task_id, original](ckv::widgets::MessageBoxResult result) {
        if (result != ckv::widgets::MessageBoxResult::Yes) return;
        const auto removed = controller_.delete_task(task_id);
        if (!removed) {
            handle_conflict(
                "Task delete conflict", removed.error,
                [task_id, original](const TodoWorkspace& remote) {
                    const Task* remote_task = remote.find_task(task_id);
                    return remote_task != nullptr && *remote_task == original;
                },
                [this, task_id] {
                    const auto retried = controller_.delete_task(task_id);
                    if (!retried) {
                        show_error("Cannot retry task delete", retried.error);
                        return;
                    }
                    refresh_board();
                    set_status("Task delete rebased and saved.");
                });
            return;
        }
        refresh_board();
        set_status("Task permanently deleted.");
    });
}

void TodoApp::open_note_editor() {
    const Task* task = selected_task();
    if (task == nullptr) return;
    for (const auto& note : notes_) {
        if (note->task_id != task->id) continue;
        desktop_->activate(note->window);
        app_.set_focus(note->editor);
        return;
    }

    auto session = std::make_unique<NoteSession>();
    session->task_id = task->id;
    session->last_saved = task->note;
    session->document = std::make_shared<ckv::widgets::EditorDocument>(
        task->note, ckv::widgets::EditorDocumentOptions{.max_document_bytes = TodoLimits::max_note_bytes});
    NoteSession* observer = session.get();
    auto window = std::make_unique<ckv::widgets::Window>("Note — " + task->title);
    const int offset = static_cast<int>(notes_.size()) * 2;
    const ckv::Rect area = desktop_->content_area();
    window->set_bounds(cascaded_window_bounds(area, ckv::Size{58, 16}, ckv::Size{30, 8}, ckv::Point{6 + offset, 4 + offset}));
    window->set_min_size(ckv::Size{std::min(30, area.width), std::min(8, area.height)});
    auto editor = std::make_unique<ckv::widgets::TextEditor>(session->document);
    editor->set_help_context_key("todo.note");
    editor->set_command_context("todo.note");
    editor->set_wrap_mode(ckv::widgets::WrapMode::Word);
    observer->editor = editor.get();
    editor->set_status_changed_handler([this, observer](const ckv::widgets::EditorStatus&) {
        refresh_note_footer(*observer);
    });
    observer->observer = session->document->subscribe([this, observer](const ckv::widgets::DocumentChange&) {
        if (!observer->applying_remote) schedule_note_save(observer->task_id);
    });
    window->set_content(std::move(editor));
    window->close_request = [this, task_id = task->id] { return flush_note(task_id); };
    window->on_closed = [this, observer] { close_note_session(observer); };
    observer->window = desktop_->add_window(std::move(window));
    notes_.push_back(std::move(session));
    refresh_note_footer(*observer);
    if (menu_bar_ != nullptr) menu_bar_->set_menus(build_menus());
    app_.set_focus(observer->editor);
}

void TodoApp::schedule_note_save(TaskId task_id) {
    const auto found = std::find_if(notes_.begin(), notes_.end(), [task_id](const auto& note) {
        return note->task_id == task_id;
    });
    if (found == notes_.end()) return;
    NoteSession& note = **found;
    if (note.timer != 0) app_.cancel_timer(note.timer);
    note.save_state = "Saving...";
    refresh_note_footer(note);
    note.timer = app_.start_timer(kNoteSaveDelayNanos, false, [this, task_id] { flush_note(task_id); });
}

bool TodoApp::flush_note(TaskId task_id) {
    const auto found = std::find_if(notes_.begin(), notes_.end(), [task_id](const auto& note) {
        return note->task_id == task_id;
    });
    if (found == notes_.end()) return true;
    NoteSession& note = **found;
    if (note.timer != 0) {
        app_.cancel_timer(note.timer);
        note.timer = 0;
    }
    const Task* task = controller_.workspace() ? controller_.workspace()->find_task(task_id) : nullptr;
    if (task == nullptr) {
        note.editor->set_read_only(true);
        note.save_state = "Removed externally — local text retained";
        refresh_note_footer(note);
        return true;
    }
    const std::string current_text = note.document->text();
    if (current_text == note.last_saved) {
        note.save_state = "Saved";
        refresh_note_footer(note);
        return true;
    }
    if (task->note != note.last_saved) {
        note.save_state = "External update pending — local text retained";
        refresh_note_footer(note);
        const ControllerError conflict{ControllerErrorCode::Conflict, ModelErrorCode::None,
                                       RepositoryErrorCode::Conflict,
                                       "the note changed in another TODO instance"};
        handle_conflict(
            "Note edit conflict", conflict, [](const TodoWorkspace&) { return false; },
            [this, task_id] { retry_note_over_external(task_id); },
            [this, task_id] { adopt_external_note(task_id); });
        return false;
    }
    TaskDraft draft = draft_from(*task);
    draft.note = current_text;
    const Task original = *task;
    const auto saved = controller_.edit_task(task_id, std::move(draft));
    if (!saved) {
        note.save_state = "Not saved: " + saved.error.diagnostic;
        refresh_note_footer(note);
        set_status("Note not saved: " + saved.error.diagnostic);
        handle_conflict(
            "Note edit conflict", saved.error,
            [task_id, original_note = original.note](const TodoWorkspace& remote) {
                const Task* remote_task = remote.find_task(task_id);
                return remote_task != nullptr && remote_task->note == original_note;
            },
            [this, task_id] { retry_note_over_external(task_id); },
            [this, task_id] { adopt_external_note(task_id); });
        return note.last_saved == current_text;
    }
    note.last_saved = current_text;
    note.document->mark_clean();
    note.save_state = "Saved";
    refresh_note_footer(note);
    refresh_board();
    return true;
}

void TodoApp::retry_note_over_external(TaskId task_id) {
    const auto session_it = std::find_if(notes_.begin(), notes_.end(), [task_id](const auto& session) {
        return session->task_id == task_id;
    });
    const Task* remote_task = controller_.workspace() != nullptr
                                  ? controller_.workspace()->find_task(task_id)
                                  : nullptr;
    if (session_it == notes_.end() || remote_task == nullptr) return;
    NoteSession& session = **session_it;
    session.last_saved = remote_task->note;
    (void)flush_note(task_id);
}

void TodoApp::adopt_external_note(TaskId task_id) {
    const auto session_it = std::find_if(notes_.begin(), notes_.end(), [task_id](const auto& session) {
        return session->task_id == task_id;
    });
    const Task* remote_task = controller_.workspace() != nullptr
                                  ? controller_.workspace()->find_task(task_id)
                                  : nullptr;
    if (session_it == notes_.end()) return;
    NoteSession& session = **session_it;
    if (remote_task == nullptr) {
        session.editor->set_read_only(true);
        session.save_state = "Removed externally — local text retained";
        refresh_note_footer(session);
        return;
    }
    session.applying_remote = true;
    (void)session.document->set_text(remote_task->note);
    session.document->clear_history();
    session.document->mark_clean();
    session.applying_remote = false;
    session.last_saved = remote_task->note;
    session.save_state = "Saved — external version";
    refresh_note_footer(session);
}

void TodoApp::close_note_session(NoteSession* session) {
    if (session == nullptr) return;
    if (session->timer != 0) app_.cancel_timer(session->timer);
    if (session->document != nullptr && session->observer != 0) session->document->unsubscribe(session->observer);
    if (session->editor != nullptr) session->editor->set_status_changed_handler({});
    NoteSession* fallback_note = nullptr;
    for (auto it = notes_.rbegin(); it != notes_.rend(); ++it) {
        if (it->get() != session) {
            fallback_note = it->get();
            break;
        }
    }
    if (fallback_note != nullptr) {
        desktop_->activate(fallback_note->window);
        app_.set_focus(fallback_note->editor);
    } else if (board_window_ != nullptr) {
        desktop_->activate(board_window_);
        if (const auto lane = board_view_->active_lane()) app_.set_focus(board_view_->lane_view(*lane));
    }
    if (session->window != nullptr) {
        session->window->close_request = {};
        session->window->on_closed = {};
        ckv::widgets::schedule_self_detach(*session->window, app_);
    }
    notes_.erase(std::remove_if(notes_.begin(), notes_.end(), [session](const auto& item) {
                     return item.get() == session;
                 }),
                 notes_.end());
    if (menu_bar_ != nullptr) menu_bar_->set_menus(build_menus());
}

void TodoApp::sync_note_sessions_after_reload() {
    const TodoWorkspace* workspace = controller_.workspace();
    if (workspace == nullptr) return;
    for (const auto& item : notes_) {
        NoteSession& note = *item;
        const Task* remote = workspace->find_task(note.task_id);
        if (remote == nullptr) {
            note.editor->set_read_only(true);
            note.save_state = "Removed externally — local text retained";
            refresh_note_footer(note);
            continue;
        }
        if (note.document->text() != note.last_saved) {
            if (remote->note != note.last_saved) {
                note.save_state = "External update pending — local text retained";
                refresh_note_footer(note);
            }
            continue;
        }
        if (remote->note == note.last_saved) continue;
        note.applying_remote = true;
        (void)note.document->set_text(remote->note);
        note.document->clear_history();
        note.document->mark_clean();
        note.applying_remote = false;
        note.last_saved = remote->note;
        note.save_state = "Saved — external update";
        refresh_note_footer(note);
    }
}

void TodoApp::refresh_note_footer(NoteSession& session) {
    if (session.window == nullptr || session.editor == nullptr) return;
    const ckv::widgets::EditorStatus status = session.editor->status();
    session.window->set_footer("Ln " + std::to_string(status.line) + ", Col " +
                               std::to_string(status.column) + " · " + session.save_state);
}

TodoApp::NoteSession* TodoApp::focused_note() const noexcept {
    const ckv::ui::View* focused = app_.focused();
    const auto found = std::find_if(notes_.begin(), notes_.end(), [focused](const auto& note) {
        return note->editor == focused;
    });
    return found == notes_.end() ? nullptr : found->get();
}

void TodoApp::present_lane_actions() {
    const Lane* lane = active_lane_record();
    const Board* board = active_board();
    const TodoWorkspace* workspace = controller_.workspace();
    if (lane == nullptr || board == nullptr || workspace == nullptr) return;
    std::vector<LaneDialogAction> actions{
        LaneDialogAction::Rename, LaneDialogAction::Color, LaneDialogAction::Sort};
    std::vector<std::string> labels{"Rename", "Color", "Sort"};
    if (board->lanes.size() < TodoLimits::max_lanes_per_board && lane_count(*workspace) < TodoLimits::max_lanes) {
        actions.push_back(LaneDialogAction::InsertLeft);
        actions.push_back(LaneDialogAction::InsertRight);
        labels.push_back("Insert left");
        labels.push_back("Insert right");
    }
    if (board->lanes.size() > 1) {
        actions.push_back(LaneDialogAction::Merge);
        actions.push_back(LaneDialogAction::Archive);
        labels.push_back("Merge into another lane");
        labels.push_back("Archive lane");
    }
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Lane actions — " + lane->title;
    descriptor.help_context_key = "todo.lanes";
    descriptor.fields.push_back(radio_field("&Action", std::move(labels), 0));
    descriptor.buttons.push_back({"&Continue", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    lane_actions_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    lane_actions_dialog_->set_completion_handler(
        [this, captured_actions = std::move(actions)](ckv::widgets::DialogResult result) {
            if (!result.accepted || result.selected.empty() || result.selected[0] < 0 ||
                static_cast<std::size_t>(result.selected[0]) >= captured_actions.size())
                return;
            switch (captured_actions[static_cast<std::size_t>(result.selected[0])]) {
                case LaneDialogAction::Rename: present_lane_rename(); break;
                case LaneDialogAction::Color: present_lane_color(); break;
                case LaneDialogAction::Sort: present_lane_sort(); break;
                case LaneDialogAction::InsertLeft: present_lane_insert(true); break;
                case LaneDialogAction::InsertRight: present_lane_insert(false); break;
                case LaneDialogAction::Merge: present_lane_merge(); break;
                case LaneDialogAction::Archive: present_lane_archive(); break;
            }
        });
}

void TodoApp::present_lane_rename() {
    const Lane* lane = active_lane_record();
    if (lane == nullptr) return;
    const LaneId lane_id = lane->id;
    const Lane original = *lane;
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Rename lane";
    descriptor.help_context_key = "todo.lanes";
    descriptor.fields.push_back(ckv::widgets::FieldDescriptor{
        "&Name:", lane->title,
        [](const std::string& value) { return !value.empty() && value.size() <= TodoLimits::max_name_bytes; }});
    descriptor.buttons.push_back({"&Rename", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    lane_edit_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    lane_edit_dialog_->set_completion_handler([this, lane_id, original](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.empty()) return;
        const std::string name = result.values[0];
        const auto renamed = controller_.rename_lane(lane_id, name);
        if (!renamed) {
            handle_conflict(
                "Lane rename conflict", renamed.error,
                [lane_id, original](const TodoWorkspace& remote) {
                    const Lane* remote_lane = remote.find_lane(lane_id);
                    return remote_lane != nullptr && *remote_lane == original;
                },
                [this, lane_id, name] {
                    const auto retried = controller_.rename_lane(lane_id, name);
                    if (!retried) {
                        show_error("Cannot retry lane rename", retried.error);
                        return;
                    }
                    refresh_board();
                    set_status("Lane rename rebased and saved.");
                });
            return;
        }
        refresh_board();
        set_status("Lane renamed and saved.");
    });
}

void TodoApp::present_lane_color() {
    const Lane* lane = active_lane_record();
    if (lane == nullptr) return;
    const LaneId lane_id = lane->id;
    const Lane original = *lane;
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Lane color — " + lane->title;
    descriptor.help_context_key = "todo.lanes";
    descriptor.fields.push_back(combo_field("&Color:", strings(kColorNames), color_selection(lane->color)));
    descriptor.buttons.push_back({"&Apply", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    lane_edit_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    lane_edit_dialog_->set_completion_handler([this, lane_id, original](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.selected.empty()) return;
        const std::optional<TodoColor> color = color_from_selection(result.selected[0]);
        const auto changed = controller_.set_lane_color(lane_id, color);
        if (!changed) {
            handle_conflict(
                "Lane color conflict", changed.error,
                [lane_id, original](const TodoWorkspace& remote) {
                    const Lane* remote_lane = remote.find_lane(lane_id);
                    return remote_lane != nullptr && *remote_lane == original;
                },
                [this, lane_id, color] {
                    const auto retried = controller_.set_lane_color(lane_id, color);
                    if (!retried) {
                        show_error("Cannot retry lane color", retried.error);
                        return;
                    }
                    refresh_board();
                    set_status("Lane color rebased and saved.");
                });
            return;
        }
        refresh_board();
        set_status("Lane color saved.");
    });
}

void TodoApp::present_lane_sort() {
    const Lane* lane = active_lane_record();
    if (lane == nullptr) return;
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Sort lane — " + lane->title;
    descriptor.help_context_key = "todo.lanes";
    descriptor.fields.push_back(combo_field("&Sort by:", strings(kSortNames), static_cast<int>(lane->sort)));
    descriptor.buttons.push_back({"&Apply", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    lane_edit_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    lane_edit_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.selected.empty() || result.selected[0] < 0 || result.selected[0] > 5) return;
        set_lane_sort(static_cast<SortMode>(result.selected[0]));
    });
}

void TodoApp::present_lane_insert(bool before_active) {
    const Board* board = active_board();
    const auto lane_id = active_lane();
    if (board == nullptr || !lane_id) return;
    std::optional<LaneId> before;
    const auto found = std::find_if(board->lanes.begin(), board->lanes.end(), [lane_id](const Lane& lane) {
        return lane.id == *lane_id;
    });
    if (found == board->lanes.end()) return;
    if (before_active) before = *lane_id;
    else if (std::next(found) != board->lanes.end()) before = std::next(found)->id;
    const BoardId board_id = board->id;

    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = before_active ? "Insert lane left" : "Insert lane right";
    descriptor.help_context_key = "todo.lanes";
    descriptor.fields.push_back(ckv::widgets::FieldDescriptor{
        "&Name:", {},
        [](const std::string& value) { return !value.empty() && value.size() <= TodoLimits::max_name_bytes; }});
    descriptor.buttons.push_back({"&Insert", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    lane_edit_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    lane_edit_dialog_->set_completion_handler([this, board_id, before](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.empty()) return;
        const std::string name = result.values[0];
        const auto inserted = controller_.insert_lane(board_id, name, before);
        if (!inserted) {
            handle_conflict(
                "Lane insertion conflict", inserted.error,
                [board_id, before](const TodoWorkspace& remote) {
                    return remote.find_board(board_id) != nullptr && (!before || remote.find_lane(*before) != nullptr);
                },
                [this, board_id, before, name] {
                    const auto retried = controller_.insert_lane(board_id, name, before);
                    if (!retried) {
                        show_error("Cannot retry lane insertion", retried.error);
                        return;
                    }
                    refresh_board();
                    if (TodoLaneView* lane = board_view_->lane_view(*retried.value)) app_.set_focus(lane);
                    set_status("Lane insertion rebased and saved.");
                });
            return;
        }
        refresh_board();
        if (TodoLaneView* lane = board_view_->lane_view(*inserted.value)) app_.set_focus(lane);
        set_status("Lane inserted and saved.");
    });
}

void TodoApp::present_lane_merge() {
    const Board* board = active_board();
    const auto source_id = active_lane();
    if (board == nullptr || !source_id || board->lanes.size() < 2) return;
    const Lane* source = controller_.workspace()->find_lane(*source_id);
    if (source == nullptr) return;
    const Lane source_original = *source;
    std::vector<LaneId> targets;
    std::vector<std::string> labels;
    for (const Lane& lane : board->lanes) {
        if (lane.id == *source_id) continue;
        targets.push_back(lane.id);
        labels.push_back(lane.title + "  (#" + std::to_string(lane.id.value) + ")");
    }
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Merge lane";
    descriptor.help_context_key = "todo.lanes";
    descriptor.fields.push_back(combo_field("Merge &into:", std::move(labels), 0));
    descriptor.buttons.push_back({"&Merge", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    lane_edit_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    lane_edit_dialog_->set_completion_handler(
        [this, captured_source_id = *source_id, source_original,
         captured_targets = std::move(targets)](ckv::widgets::DialogResult result) {
            if (!result.accepted || result.selected.empty() || result.selected[0] < 0 ||
                static_cast<std::size_t>(result.selected[0]) >= captured_targets.size())
                return;
            const LaneId target_id = captured_targets[static_cast<std::size_t>(result.selected[0])];
            const Lane* target = controller_.workspace()->find_lane(target_id);
            if (target == nullptr) return;
            const Lane target_original = *target;
            const auto merged = controller_.merge_lane(captured_source_id, target_id);
            if (!merged) {
                handle_conflict(
                    "Lane merge conflict", merged.error,
                    [captured_source_id, target_id, source_original, target_original](const TodoWorkspace& remote) {
                        const Lane* remote_source = remote.find_lane(captured_source_id);
                        const Lane* remote_target = remote.find_lane(target_id);
                        return remote_source != nullptr && remote_target != nullptr &&
                               *remote_source == source_original && *remote_target == target_original;
                    },
                    [this, captured_source_id, target_id] {
                        const auto retried = controller_.merge_lane(captured_source_id, target_id);
                        if (!retried) {
                            show_error("Cannot retry lane merge", retried.error);
                            return;
                        }
                        refresh_board();
                        if (TodoLaneView* lane = board_view_->lane_view(target_id)) app_.set_focus(lane);
                        set_status("Lane merge rebased and saved.");
                    });
                return;
            }
            refresh_board();
            if (TodoLaneView* lane = board_view_->lane_view(target_id)) app_.set_focus(lane);
            set_status("Lane merged and removed.");
        });
}

void TodoApp::present_lane_archive() {
    const Lane* lane = active_lane_record();
    if (lane == nullptr) return;
    const LaneId lane_id = lane->id;
    const Lane original = *lane;
    lane_confirmation_ = ckv::widgets::present_modal_message_box(
        app_, *desktop_, roles_,
        {ckv::widgets::MessageBoxKind::Warning, "Archive lane",
         "Archive all " + std::to_string(lane->task_ids.size()) + " task(s) in ‘" + lane->title +
             "’ and remove the lane?",
         ckv::widgets::MessageBoxButtons::YesNo});
    lane_confirmation_->set_completion_handler([this, lane_id, original](ckv::widgets::MessageBoxResult result) {
        if (result != ckv::widgets::MessageBoxResult::Yes) return;
        const auto archived = controller_.archive_lane(lane_id);
        if (!archived) {
            handle_conflict(
                "Lane archive conflict", archived.error,
                [lane_id, original](const TodoWorkspace& remote) {
                    const Lane* remote_lane = remote.find_lane(lane_id);
                    return remote_lane != nullptr && *remote_lane == original;
                },
                [this, lane_id] {
                    const auto retried = controller_.archive_lane(lane_id);
                    if (!retried) {
                        show_error("Cannot retry lane archive", retried.error);
                        return;
                    }
                    refresh_board();
                    set_status("Lane archive rebased and saved.");
                });
            return;
        }
        refresh_board();
        set_status("Lane tasks archived before the lane was removed.");
    });
}

void TodoApp::set_lane_sort(SortMode sort) {
    const auto lane_id = active_lane();
    if (!lane_id) return;
    const Lane* lane = controller_.workspace() != nullptr ? controller_.workspace()->find_lane(*lane_id) : nullptr;
    if (lane == nullptr) return;
    const Lane original = *lane;
    const auto changed = controller_.set_lane_sort(*lane_id, sort);
    if (!changed) {
        handle_conflict(
            "Lane sorting conflict", changed.error,
            [captured_lane_id = *lane_id, original](const TodoWorkspace& remote) {
                const Lane* remote_lane = remote.find_lane(captured_lane_id);
                return remote_lane != nullptr && *remote_lane == original;
            },
            [this, captured_lane_id = *lane_id, sort] {
                const auto retried = controller_.set_lane_sort(captured_lane_id, sort);
                if (!retried) {
                    show_error("Cannot retry lane sorting", retried.error);
                    return;
                }
                refresh_board();
                set_status("Lane sorting rebased and saved.");
            });
        return;
    }
    refresh_board();
    set_status("Lane sorting saved.");
}

void TodoApp::present_board_manager() {
    const TodoWorkspace* workspace = controller_.workspace();
    if (workspace == nullptr) return;
    std::vector<BoardId> boards;
    std::vector<std::string> labels;
    std::vector<BoardDialogAction> actions{BoardDialogAction::Switch};
    std::vector<std::string> action_labels{"Switch selected Board"};
    int selected = 0;
    for (std::size_t index = 0; index < workspace->snapshot().boards.size(); ++index) {
        const Board& board = workspace->snapshot().boards[index];
        boards.push_back(board.id);
        labels.push_back(board.name);
        if (board.id == workspace->snapshot().last_board_id) selected = static_cast<int>(index);
    }
    if (workspace->snapshot().boards.size() < TodoLimits::max_boards &&
        lane_count(*workspace) + 3 <= TodoLimits::max_lanes) {
        actions.push_back(BoardDialogAction::Create);
        action_labels.push_back("Create new Board");
    }
    const bool has_non_main = std::any_of(boards.begin(), boards.end(), [](BoardId id) { return id != BoardId{1}; });
    if (has_non_main) {
        actions.push_back(BoardDialogAction::Rename);
        actions.push_back(BoardDialogAction::Merge);
        actions.push_back(BoardDialogAction::Archive);
        action_labels.push_back("Rename Board...");
        action_labels.push_back("Merge Board into...");
        action_labels.push_back("Archive Board...");
    }
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Board Manager";
    descriptor.help_context_key = "todo.boards";
    descriptor.fields.push_back(combo_field("&Board:", std::move(labels), selected));
    descriptor.fields.push_back(radio_field("&Action", std::move(action_labels), 0));
    descriptor.buttons.push_back({"&Continue", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    board_manager_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    board_manager_dialog_->set_completion_handler(
        [this, captured_boards = std::move(boards),
         captured_actions = std::move(actions)](ckv::widgets::DialogResult result) {
            if (!result.accepted || result.selected.size() < 2 || result.selected[0] < 0 ||
                result.selected[1] < 0 || static_cast<std::size_t>(result.selected[0]) >= captured_boards.size() ||
                static_cast<std::size_t>(result.selected[1]) >= captured_actions.size())
                return;
            const BoardId board_id = captured_boards[static_cast<std::size_t>(result.selected[0])];
            switch (captured_actions[static_cast<std::size_t>(result.selected[1])]) {
                case BoardDialogAction::Switch: switch_board(board_id); break;
                case BoardDialogAction::Create: present_new_board(); break;
                case BoardDialogAction::Rename: present_board_action_source(true, false); break;
                case BoardDialogAction::Merge: present_board_action_source(false, true); break;
                case BoardDialogAction::Archive: present_board_action_source(false, false); break;
            }
        });
}

void TodoApp::present_board_action_source(bool rename, bool merge) {
    const TodoWorkspace* workspace = controller_.workspace();
    if (workspace == nullptr) return;
    std::vector<BoardId> boards;
    std::vector<std::string> labels;
    for (const Board& board : workspace->snapshot().boards) {
        if (board.id == BoardId{1}) continue;
        boards.push_back(board.id);
        labels.push_back(board.name);
    }
    if (boards.empty()) return;

    const std::string action = rename ? "rename" : merge ? "merge" : "archive";
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Choose Board to " + action;
    descriptor.help_context_key = "todo.boards";
    descriptor.fields.push_back(combo_field("&Board:", std::move(labels), 0));
    descriptor.buttons.push_back({"&Continue", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    board_action_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    board_action_dialog_->set_completion_handler(
        [this, rename, merge, captured_boards = std::move(boards)](ckv::widgets::DialogResult result) {
            if (!result.accepted || result.selected.empty() || result.selected[0] < 0 ||
                static_cast<std::size_t>(result.selected[0]) >= captured_boards.size())
                return;
            const BoardId board_id = captured_boards[static_cast<std::size_t>(result.selected[0])];
            if (rename) present_board_rename(board_id);
            else if (merge) present_board_merge(board_id);
            else present_board_archive(board_id);
        });
}

void TodoApp::present_new_board() {
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "New Board";
    descriptor.help_context_key = "todo.boards";
    descriptor.fields.push_back(ckv::widgets::FieldDescriptor{
        "&Name:", {},
        [](const std::string& value) { return !value.empty() && value.size() <= TodoLimits::max_name_bytes; }});
    descriptor.buttons.push_back({"&Create", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    board_edit_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    board_edit_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.empty()) return;
        const std::string name = result.values[0];
        const auto added = controller_.add_board(name);
        if (!added) {
            handle_conflict(
                "Board creation conflict", added.error,
                [name](const TodoWorkspace& remote) {
                    return std::none_of(remote.snapshot().boards.begin(), remote.snapshot().boards.end(),
                                        [&name](const Board& board) { return board.name == name; });
                },
                [this, name] {
                    const auto retried = controller_.add_board(name);
                    if (!retried) {
                        show_error("Cannot retry board creation", retried.error);
                        return;
                    }
                    switch_board(*retried.value);
                    set_status("Board creation rebased and saved.");
                });
            return;
        }
        switch_board(*added.value);
        set_status("Board created, selected, and saved.");
    });
}

void TodoApp::present_board_rename(BoardId board_id) {
    const TodoWorkspace* workspace = controller_.workspace();
    const Board* board = workspace != nullptr ? workspace->find_board(board_id) : nullptr;
    if (board == nullptr) return;
    const Board original = *board;
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Rename Board";
    descriptor.help_context_key = "todo.boards";
    descriptor.fields.push_back(ckv::widgets::FieldDescriptor{
        "&Name:", board->name,
        [](const std::string& value) { return !value.empty() && value.size() <= TodoLimits::max_name_bytes; }});
    descriptor.buttons.push_back({"&Rename", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    board_edit_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    board_edit_dialog_->set_completion_handler([this, board_id, original](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.values.empty()) return;
        const std::string name = result.values[0];
        const auto renamed = controller_.rename_board(board_id, name);
        if (!renamed) {
            handle_conflict(
                "Board rename conflict", renamed.error,
                [board_id, original](const TodoWorkspace& remote) {
                    const Board* remote_board = remote.find_board(board_id);
                    return remote_board != nullptr && *remote_board == original;
                },
                [this, board_id, name] {
                    const auto retried = controller_.rename_board(board_id, name);
                    if (!retried) {
                        show_error("Cannot retry board rename", retried.error);
                        return;
                    }
                    refresh_board();
                    set_status("Board rename rebased and saved.");
                });
            return;
        }
        refresh_board();
        set_status("Board renamed and saved.");
    });
}

void TodoApp::present_board_merge(BoardId source_id) {
    const TodoWorkspace* workspace = controller_.workspace();
    if (workspace == nullptr || workspace->snapshot().boards.size() < 2) return;
    const Board* source = workspace->find_board(source_id);
    if (source == nullptr) return;
    const Board source_original = *source;
    std::vector<BoardId> targets;
    std::vector<std::string> labels;
    for (const Board& board : workspace->snapshot().boards) {
        if (board.id == source_id) continue;
        targets.push_back(board.id);
        labels.push_back(board.name);
    }
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Merge Board";
    descriptor.help_context_key = "todo.boards";
    descriptor.fields.push_back(combo_field("Merge &into:", std::move(labels), 0));
    descriptor.buttons.push_back({"&Merge", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    board_edit_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    board_edit_dialog_->set_completion_handler(
        [this, source_id, source_original, captured_targets = std::move(targets)](ckv::widgets::DialogResult result) {
            if (!result.accepted || result.selected.empty() || result.selected[0] < 0 ||
                static_cast<std::size_t>(result.selected[0]) >= captured_targets.size())
                return;
            const BoardId target_id = captured_targets[static_cast<std::size_t>(result.selected[0])];
            const Board* target = controller_.workspace()->find_board(target_id);
            if (target == nullptr) return;
            const Board target_original = *target;
            const auto merged = controller_.merge_board(source_id, target_id);
            if (!merged) {
                handle_conflict(
                    "Board merge conflict", merged.error,
                    [source_id, target_id, source_original, target_original](const TodoWorkspace& remote) {
                        const Board* remote_source = remote.find_board(source_id);
                        const Board* remote_target = remote.find_board(target_id);
                        return remote_source != nullptr && remote_target != nullptr &&
                               *remote_source == source_original && *remote_target == target_original;
                    },
                    [this, source_id, target_id] {
                        const auto retried = controller_.merge_board(source_id, target_id);
                        if (!retried) {
                            show_error("Cannot retry board merge", retried.error);
                            return;
                        }
                        refresh_board();
                        set_status("Board merge rebased and saved.");
                    });
                return;
            }
            refresh_board();
            set_status("Board merged and removed.");
        });
}

void TodoApp::present_board_archive(BoardId board_id) {
    const TodoWorkspace* workspace = controller_.workspace();
    const Board* board = workspace != nullptr ? workspace->find_board(board_id) : nullptr;
    if (board == nullptr) return;
    const Board original = *board;
    std::size_t task_count = 0;
    for (const Lane& lane : board->lanes) task_count += lane.task_ids.size();
    board_confirmation_ = ckv::widgets::present_modal_message_box(
        app_, *desktop_, roles_,
        {ckv::widgets::MessageBoxKind::Warning, "Archive Board",
         "Archive all " + std::to_string(task_count) + " task(s) in ‘" + board->name + "’ and remove the board?",
         ckv::widgets::MessageBoxButtons::YesNo});
    board_confirmation_->set_completion_handler([this, board_id, original](ckv::widgets::MessageBoxResult result) {
        if (result != ckv::widgets::MessageBoxResult::Yes) return;
        const auto archived = controller_.archive_board(board_id);
        if (!archived) {
            handle_conflict(
                "Board archive conflict", archived.error,
                [board_id, original](const TodoWorkspace& remote) {
                    const Board* remote_board = remote.find_board(board_id);
                    return remote_board != nullptr && *remote_board == original;
                },
                [this, board_id] {
                    const auto retried = controller_.archive_board(board_id);
                    if (!retried) {
                        show_error("Cannot retry board archive", retried.error);
                        return;
                    }
                    refresh_board();
                    set_status("Board archive rebased and saved.");
                });
            return;
        }
        refresh_board();
        set_status("Board tasks archived before the board was removed.");
    });
}

void TodoApp::switch_board(BoardId board_id) {
    const auto switched = controller_.switch_board(board_id);
    if (!switched) {
        handle_conflict(
            "Board switch conflict", switched.error,
            [board_id](const TodoWorkspace& remote) { return remote.find_board(board_id) != nullptr; },
            [this, board_id] {
                const auto retried = controller_.switch_board(board_id);
                if (!retried) {
                    show_error("Cannot retry board switch", retried.error);
                    return;
                }
                refresh_board();
                set_status("Board switch rebased and remembered.");
            });
        return;
    }
    refresh_board();
    set_status("Board selected and remembered.");
}

void TodoApp::toggle_move() {
    if (!move_) {
        const Task* task = selected_task();
        if (task == nullptr) return;
        const auto started = controller_.begin_task_move(task->id);
        if (!started) {
            show_error("Cannot move task", started.error);
            return;
        }
        move_ = *started.value;
        board_view_->begin_keyboard_move(task->id, move_->source_lane_id,
                                         move_->original_before_task_id);
        set_status("Move mode: arrows choose lane and position; Enter commits; Esc cancels.");
        return;
    }

    const auto lane = active_lane();
    if (!lane) return;
    const std::optional<TaskId> before = board_view_->keyboard_move_before();
    const auto staged = controller_.stage_task_move(*move_, *lane, before);
    if (!staged) {
        show_error("Cannot stage task move", staged.error);
        return;
    }
    const TaskId task_id = move_->task_id;
    const LaneId source_lane_id = move_->source_lane_id;
    const LaneId target_lane_id = *lane;
    const Task* current_task = controller_.workspace() != nullptr ? controller_.workspace()->find_task(task_id) : nullptr;
    if (current_task == nullptr) return;
    const Task original = *current_task;
    const auto committed = controller_.commit_task_move(*staged.value);
    if (!committed) {
        move_.reset();
        board_view_->end_keyboard_move();
        handle_conflict(
            "Task move conflict", committed.error,
            [task_id, source_lane_id, target_lane_id, before, original](const TodoWorkspace& remote) {
                const Task* task = remote.find_task(task_id);
                return task != nullptr && *task == original && remote.lane_of(task_id) == source_lane_id &&
                       remote.find_lane(target_lane_id) != nullptr &&
                       (!before || (remote.find_task(*before) != nullptr && remote.lane_of(*before) == target_lane_id));
            },
            [this, task_id, target_lane_id, before] {
                const auto restarted = controller_.begin_task_move(task_id);
                if (!restarted) {
                    show_error("Cannot restart task move", restarted.error);
                    return;
                }
                std::optional<TaskId> rebased_before = before;
                if (rebased_before == task_id) rebased_before = restarted.value->original_before_task_id;
                const Lane* target = controller_.workspace()->find_lane(target_lane_id);
                if (target != nullptr && target->sort != SortMode::Manual) rebased_before.reset();
                const auto restaged = controller_.stage_task_move(*restarted.value, target_lane_id, rebased_before);
                if (!restaged) {
                    show_error("Cannot restage task move", restaged.error);
                    return;
                }
                const auto retried = controller_.commit_task_move(*restaged.value);
                if (!retried) {
                    show_error("Cannot retry task move", retried.error);
                    return;
                }
                refresh_board();
                board_view_->select_task(task_id);
                set_status("Task move rebased and saved.");
            });
        return;
    }
    move_.reset();
    board_view_->end_keyboard_move();
    refresh_board();
    board_view_->select_task(task_id);
    set_status(committed.value->changed ? "Task moved and saved." : "Task stayed in its original position.");
}

void TodoApp::cancel_move() {
    if (!move_) return;
    const TaskId task_id = move_->task_id;
    move_.reset();
    board_view_->end_keyboard_move();
    board_view_->select_task(task_id);
    set_status("Move cancelled; original position restored.");
}

void TodoApp::drop_task(TaskId task_id, LaneId target_lane_id, std::optional<TaskId> before_task_id) {
    const TodoWorkspace* workspace = controller_.workspace();
    const Lane* target = workspace != nullptr ? workspace->find_lane(target_lane_id) : nullptr;
    if (target == nullptr) return;
    const auto started = controller_.begin_task_move(task_id);
    if (!started) {
        show_error("Cannot drag task", started.error);
        return;
    }
    if (target->sort != SortMode::Manual) {
        if (started.value->source_lane_id == target_lane_id) {
            set_status("This lane is sorted automatically; choose Manual before reordering it.");
            return;
        }
        before_task_id.reset();
    }
    if (before_task_id == task_id) before_task_id = started.value->original_before_task_id;
    const LaneId source_lane_id = started.value->source_lane_id;
    const Task* current_task = workspace->find_task(task_id);
    if (current_task == nullptr) return;
    const Task original = *current_task;
    const auto staged = controller_.stage_task_move(*started.value, target_lane_id, before_task_id);
    if (!staged) {
        show_error("Cannot stage dragged task", staged.error);
        return;
    }
    const auto committed = controller_.commit_task_move(*staged.value);
    if (!committed) {
        handle_conflict(
            "Task drag conflict", committed.error,
            [task_id, source_lane_id, target_lane_id, before_task_id, original](const TodoWorkspace& remote) {
                const Task* task = remote.find_task(task_id);
                return task != nullptr && *task == original && remote.lane_of(task_id) == source_lane_id &&
                       remote.find_lane(target_lane_id) != nullptr &&
                       (!before_task_id || (remote.find_task(*before_task_id) != nullptr &&
                                            remote.lane_of(*before_task_id) == target_lane_id));
            },
            [this, task_id, target_lane_id, before_task_id] {
                const auto restarted = controller_.begin_task_move(task_id);
                if (!restarted) {
                    show_error("Cannot restart dragged task", restarted.error);
                    return;
                }
                std::optional<TaskId> rebased_before = before_task_id;
                if (rebased_before == task_id) rebased_before = restarted.value->original_before_task_id;
                const Lane* rebased_target = controller_.workspace()->find_lane(target_lane_id);
                if (rebased_target != nullptr && rebased_target->sort != SortMode::Manual)
                    rebased_before.reset();
                const auto restaged = controller_.stage_task_move(*restarted.value, target_lane_id, rebased_before);
                if (!restaged) {
                    show_error("Cannot restage dragged task", restaged.error);
                    return;
                }
                const auto retried = controller_.commit_task_move(*restaged.value);
                if (!retried) {
                    show_error("Cannot retry dragged task", retried.error);
                    return;
                }
                refresh_board();
                board_view_->select_task(task_id);
                set_status("Dragged task rebased and saved.");
            });
        return;
    }
    refresh_board();
    board_view_->select_task(task_id);
    set_status(committed.value->changed ? "Task moved by drag and saved." : "Task stayed in its original position.");
}

void TodoApp::show_task_context_menu(ckv::Point screen_cell) {
    ckv::widgets::show_context_menu(
        {ckv::widgets::MenuItem::command(edit_task_command_), ckv::widgets::MenuItem::command(edit_note_command_),
         ckv::widgets::MenuItem::command(move_task_command_), ckv::widgets::MenuItem::separator(),
         ckv::widgets::MenuItem::command(archive_task_command_), ckv::widgets::MenuItem::command(delete_task_command_)},
        screen_cell, app_, *desktop_);
}

void TodoApp::show_lane_context_menu(ckv::Point screen_cell) {
    const SortMode active_sort = active_lane_record() != nullptr ? active_lane_record()->sort : SortMode::Manual;
    std::vector<ckv::widgets::MenuItem> sort_items;
    for (std::size_t index = 0; index < lane_sort_commands_.size(); ++index) {
        sort_items.push_back(ckv::widgets::MenuItem::command(lane_sort_commands_[index])
                                 .with_mark(static_cast<SortMode>(index) == active_sort
                                                ? ckv::widgets::MenuMark::RadioOn
                                                : ckv::widgets::MenuMark::RadioOff));
    }
    ckv::widgets::show_context_menu(
        {ckv::widgets::MenuItem::submenu("&Sort", std::move(sort_items)),
         ckv::widgets::MenuItem::command(lane_color_command_), ckv::widgets::MenuItem::command(lane_rename_command_),
         ckv::widgets::MenuItem::command(lane_insert_left_command_),
         ckv::widgets::MenuItem::command(lane_insert_right_command_), ckv::widgets::MenuItem::separator(),
         ckv::widgets::MenuItem::command(lane_merge_command_), ckv::widgets::MenuItem::command(lane_archive_command_)},
        screen_cell, app_, *desktop_);
}

void TodoApp::set_theme(TodoTheme theme) {
    theme_ = theme;
    switch (theme_) {
        case TodoTheme::Classic: app_.theme() = ckv::ui::make_classic_theme(app_.roles(), roles_); break;
        case TodoTheme::Dark: app_.theme() = ckv::ui::make_dark_theme(app_.roles(), roles_); break;
        case TodoTheme::Light: app_.theme() = ckv::ui::make_light_theme(app_.roles(), roles_); break;
        case TodoTheme::Mono: app_.theme() = ckv::ui::make_mono_theme(app_.roles(), roles_); break;
        case TodoTheme::HighContrast: app_.theme() = ckv::ui::make_high_contrast_theme(app_.roles(), roles_); break;
    }
    if (menu_bar_ != nullptr) menu_bar_->set_menus(build_menus());
    app_.invalidate_all();
}

void TodoApp::refresh_status_items() {
    if (status_line_ == nullptr) return;
    std::string board_label = "Board";
    if (const Board* board = active_board()) board_label = "Board: " + board->name;
    std::string calendar_label;
    if (const CalendarReadResult reading = calendar_.read(); reading) {
        calendar_label = reading.value->local_date.value;
        if (board_view_ != nullptr) board_view_->set_today(reading.value->local_date);
        if (reading.value->utc_timestamp.value.size() >= 16)
            calendar_label += " " + reading.value->utc_timestamp.value.substr(11, 5) + "Z";
    } else if (board_view_ != nullptr) {
        board_view_->set_today(std::nullopt);
    }
    std::vector<ckv::widgets::StatusLineItem> items{
        ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{app_.commands().standard().help}, 100},
        ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{add_task_command_}, 95},
        ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{edit_task_command_}, 70},
        ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{edit_note_command_}, 60},
        ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{archive_task_command_}, 50},
        ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{move_task_command_}, 90},
        ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{lane_actions_command_}, 80},
        ckv::widgets::StatusLineItem{std::move(board_label), board_manager_command_, 95},
    };
    if (pending_conflict_)
        items.emplace_back(ckv::widgets::CommandPresentation{resolve_conflict_command_}, 110);
    if (!calendar_label.empty()) items.emplace_back(std::move(calendar_label), ckv::ui::kInvalidCommand, 5);
    items.emplace_back(ckv::widgets::CommandPresentation{app_.commands().standard().quit}, 100);
    if (!same_status_items(status_line_->items(), items)) status_line_->set_items(std::move(items));
}

void TodoApp::show_error(std::string title, const ControllerError& error) {
    set_status(error.diagnostic);
    message_box_ = ckv::widgets::present_modal_message_box(
        app_, *desktop_, roles_,
        {ckv::widgets::MessageBoxKind::Error, std::move(title), error.diagnostic, ckv::widgets::MessageBoxButtons::Ok});
    message_box_->set_completion_handler([](ckv::widgets::MessageBoxResult) {});
}

bool TodoApp::handle_conflict(std::string title,
                              const ControllerError& error,
                              std::function<bool(const TodoWorkspace&)> can_rebase,
                              std::function<void()> retry,
                              std::function<void()> use_remote) {
    if (error.code != ControllerErrorCode::Conflict) {
        show_error(std::move(title), error);
        return false;
    }
    const auto reloaded = controller_.reload_if_changed();
    if (!reloaded) {
        show_error("Cannot load the external TODO change", reloaded.error);
        return false;
    }
    refresh_board();
    if (controller_.workspace() != nullptr && can_rebase(*controller_.workspace())) {
        set_status("External changes loaded; retrying your non-overlapping change once.");
        retry();
        return true;
    }
    pending_conflict_ = std::make_unique<PendingUiConflict>(
        PendingUiConflict{std::move(title), error.diagnostic, std::move(retry), std::move(use_remote)});
    if (menu_bar_ != nullptr) menu_bar_->set_menus(build_menus());
    refresh_status_items();
    present_conflict_resolution();
    return true;
}

void TodoApp::present_conflict_resolution() {
    if (!pending_conflict_) return;
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = pending_conflict_->title;
    descriptor.help_context_key = "todo.persistence";
    ckv::widgets::FieldDescriptor note;
    note.kind = ckv::widgets::FieldKind::Note;
    note.label = pending_conflict_->message;
    descriptor.fields.push_back(std::move(note));
    ckv::widgets::FieldDescriptor explanation;
    explanation.kind = ckv::widgets::FieldKind::Note;
    explanation.label = "The latest workspace is loaded. Choose exactly which version should win.";
    descriptor.fields.push_back(std::move(explanation));
    descriptor.fields.push_back(
        radio_field("&Resolution", {"Retry my change over the latest workspace", "Use the external version"}, 0));
    descriptor.buttons.push_back({"&Resolve", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Later", ckv::widgets::ButtonRole::Dismiss, nullptr});
    conflict_dialog_ = ckv::widgets::present_modal_dialog(std::move(descriptor), app_, *desktop_, roles_);
    conflict_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.selected.size() < 3 || result.selected[2] < 0) {
            set_status("Conflict kept pending; use File → Resolve conflict when ready.");
            return;
        }
        std::unique_ptr<PendingUiConflict> pending = std::move(pending_conflict_);
        if (menu_bar_ != nullptr) menu_bar_->set_menus(build_menus());
        refresh_status_items();
        if (result.selected[2] == 0) {
            pending->retry();
            return;
        }
        if (pending->use_remote) pending->use_remote();
        refresh_board();
        set_status("External version kept.");
    });
}

void TodoApp::set_status(std::string message) {
    if (status_line_ == nullptr) return;
    if (status_timer_ != 0) {
        app_.cancel_timer(status_timer_);
        status_timer_ = 0;
    }
    status_line_->set_transient_hint(std::move(message));
    status_timer_ = app_.start_timer(kStatusHintNanos, false, [this] {
        status_timer_ = 0;
        if (status_line_ != nullptr) status_line_->set_transient_hint({});
    });
}

const Task* TodoApp::selected_task() const noexcept {
    if (controller_.workspace() == nullptr || board_view_ == nullptr) return nullptr;
    const auto selected = board_view_->selected_task();
    return selected ? controller_.workspace()->find_task(*selected) : nullptr;
}

std::optional<LaneId> TodoApp::active_lane() const noexcept {
    return board_view_ != nullptr ? board_view_->active_lane() : std::nullopt;
}

const Lane* TodoApp::active_lane_record() const noexcept {
    const TodoWorkspace* workspace = controller_.workspace();
    const auto lane_id = active_lane();
    return workspace != nullptr && lane_id ? workspace->find_lane(*lane_id) : nullptr;
}

const Board* TodoApp::active_board() const noexcept {
    const TodoWorkspace* workspace = controller_.workspace();
    return workspace != nullptr && board_view_ != nullptr ? workspace->find_board(board_view_->board_id()) : nullptr;
}

std::optional<BoardId> TodoApp::board_named(std::string_view name) const noexcept {
    if (controller_.workspace() == nullptr) return std::nullopt;
    for (const Board& board : controller_.workspace()->snapshot().boards)
        if (board.name == name) return board.id;
    return std::nullopt;
}

}  // namespace ck::todo
