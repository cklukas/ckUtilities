// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "find_app.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

#include <cvision/ui/layout.hpp>
#include <cvision/widgets/button.hpp>
#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/input_line.hpp>
#include <cvision/widgets/list_view.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/option_group.hpp>
#include <cvision/widgets/splitter.hpp>
#include <cvision/widgets/table.hpp>
#include <cvision/widgets/text_layout.hpp>
#include <cvision/widgets/text_view.hpp>

#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/guided_search.hpp"
#include "ck/find/search_backend.hpp"
#include "ck/vision/keymap.hpp"
#include "native_host.hpp"

namespace ck::vision
{
namespace
{

using ckv::widgets::CommandPresentation;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;

std::string trim_string(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return std::string(value);
}

std::string ascii_lower_copy(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value)
        out.push_back(static_cast<char>(std::tolower(ch)));
    return out;
}

// Converts user-typed pattern input into comma-separated glob patterns:
// - "*.txt" -> "*.txt"
// - ".txt"  -> "*.txt"
// - "notes" -> "*notes*"
std::string normalize_quick_patterns(std::string_view raw)
{
    std::vector<std::string> normalized;
    std::string current;
    auto flush_token = [&] {
        const std::string token = trim_string(current);
        current.clear();
        if (token.empty())
            return;
        if (token.find('*') != std::string::npos || token.find('?') != std::string::npos)
        {
            normalized.push_back(token);
        }
        else if (token.front() == '.' && token.size() > 1)
        {
            normalized.push_back("*" + token);
        }
        else
        {
            normalized.push_back("*" + token + "*");
        }
    };

    for (char ch : raw)
    {
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)))
            flush_token();
        else
            current.push_back(ch);
    }
    flush_token();

    std::ostringstream joined;
    for (std::size_t i = 0; i < normalized.size(); ++i)
    {
        if (i != 0)
            joined << ", ";
        joined << normalized[i];
    }
    return joined.str();
}

std::string format_human_size(std::uint64_t bytes, bool is_directory, bool known)
{
    if (is_directory)
        return "<DIR>";
    if (!known)
        return "-";
    if (bytes < 1024ULL)
        return std::to_string(bytes) + " B";
    const char *units[] = {"KiB", "MiB", "GiB", "TiB"};
    double scaled = static_cast<double>(bytes) / 1024.0;
    std::size_t unit_idx = 0;
    while (scaled >= 1024.0 && unit_idx + 1 < 4)
    {
        scaled /= 1024.0;
        ++unit_idx;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(scaled >= 10.0 ? 0 : 1) << scaled << ' ' << units[unit_idx];
    return out.str();
}

std::string format_modified_time(std::int64_t unix_seconds, bool known)
{
    if (!known || unix_seconds <= 0)
        return "-";
    if (const char *capture_mode = std::getenv("CKTOOLS_DOCUMENTATION_CAPTURE");
        capture_mode != nullptr && std::string_view(capture_mode) == "1")
    {
        return "2026-11-16 08:00";
    }
    const std::time_t raw = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &raw) != 0)
        return "-";
#else
    if (localtime_r(&raw, &local) == nullptr)
        return "-";
#endif
    char buffer[32]{};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local) == 0)
        return "-";
    return buffer;
}

std::string shorten_middle(std::string_view text, std::size_t max_chars)
{
    if (text.size() <= max_chars || max_chars < 10)
        return std::string(text);
    const std::size_t left = (max_chars - 3) / 2;
    const std::size_t right = max_chars - 3 - left;
    return std::string(text.substr(0, left)) + "..." + std::string(text.substr(text.size() - right));
}

std::string build_filter_summary(const ck::find::SearchSpecification &spec)
{
    std::vector<std::string> parts;
    parts.push_back(spec.includeSubdirectories ? "Subfolders" : "Top-level only");
    if (spec.includeHidden)
        parts.push_back("Include hidden");
    if (spec.textOptions.matchCase)
        parts.push_back("Match case");

    const std::string excludes = trim_string(ck::find::bufferToString(spec.excludePatterns));
    if (!excludes.empty())
        parts.push_back("Exclude: " + excludes);

    if (spec.enableTypeFilters && spec.typeOptions.extensions[0] != '\0')
        parts.push_back("Ext: " + shorten_middle(ck::find::bufferToString(spec.typeOptions.extensions), 20));

    if (spec.enableTimeFilters && spec.timeOptions.preset != ck::find::TimeFilterOptions::Preset::AnyTime)
    {
        switch (spec.timeOptions.preset)
        {
        case ck::find::TimeFilterOptions::Preset::PastDay:
            parts.push_back("Past 24h");
            break;
        case ck::find::TimeFilterOptions::Preset::PastWeek:
            parts.push_back("Past 7d");
            break;
        case ck::find::TimeFilterOptions::Preset::PastMonth:
            parts.push_back("Past 30d");
            break;
        case ck::find::TimeFilterOptions::Preset::PastSixMonths:
            parts.push_back("Past 6mo");
            break;
        case ck::find::TimeFilterOptions::Preset::PastYear:
            parts.push_back("Past 1y");
            break;
        case ck::find::TimeFilterOptions::Preset::CustomRange:
            parts.push_back("Custom date");
            break;
        default:
            break;
        }
    }

    if (spec.enableSizeFilters)
    {
        if (spec.sizeOptions.emptyEnabled)
            parts.push_back("Empty (0 B)");
        else if (spec.sizeOptions.exactEnabled)
            parts.push_back("Size = " + ck::find::bufferToString(spec.sizeOptions.exactSpec));
        else if (spec.sizeOptions.minEnabled && spec.sizeOptions.maxEnabled)
            parts.push_back("Size " + ck::find::bufferToString(spec.sizeOptions.minSpec) + ".." +
                            ck::find::bufferToString(spec.sizeOptions.maxSpec));
        else if (spec.sizeOptions.minEnabled)
            parts.push_back("Size >= " + ck::find::bufferToString(spec.sizeOptions.minSpec));
        else if (spec.sizeOptions.maxEnabled)
            parts.push_back("Size <= " + ck::find::bufferToString(spec.sizeOptions.maxSpec));
    }

    if (spec.enableTraversalFilters && spec.traversalOptions.maxDepthEnabled && spec.traversalOptions.maxDepth[0] != '\0')
        parts.push_back("Max depth " + ck::find::bufferToString(spec.traversalOptions.maxDepth));

    std::ostringstream out;
    out << "Options: ";
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
            out << " | ";
        out << parts[i];
    }
    return out.str();
}

struct ResultRowRecord
{
    ckv::widgets::TableRowId id = 0;
    std::string full_path;
    std::string name;
    std::string folder;
    std::string size_display;
    std::string modified_display;
    std::uint64_t size_bytes = 0;
    std::int64_t modified_seconds = 0;
    bool is_directory = false;
};

class FoundFilesTableModel final : public ckv::widgets::TableModel
{
public:
    std::vector<ResultRowRecord> records;
    std::vector<std::size_t> order;

    void set_records(std::vector<ResultRowRecord> items)
    {
        records = std::move(items);
        order.resize(records.size());
        for (std::size_t i = 0; i < records.size(); ++i)
            order[i] = i;
    }

    std::size_t row_count() const override
    {
        return order.size();
    }

    ckv::widgets::TableRowId row_id_at(std::size_t display_index) const override
    {
        if (display_index >= order.size())
            return ckv::widgets::kInvalidTableRowId;
        return records[order[display_index]].id;
    }

    std::optional<std::size_t> index_of(ckv::widgets::TableRowId row) const override
    {
        for (std::size_t idx = 0; idx < order.size(); ++idx)
        {
            if (records[order[idx]].id == row)
                return idx;
        }
        return std::nullopt;
    }

    const ResultRowRecord *record_by_id(ckv::widgets::TableRowId row) const noexcept
    {
        for (const auto &rec : records)
        {
            if (rec.id == row)
                return &rec;
        }
        return nullptr;
    }

    bool erase_by_id(ckv::widgets::TableRowId row)
    {
        const auto rec_it = std::find_if(records.begin(), records.end(), [row](const ResultRowRecord &r) {
            return r.id == row;
        });
        if (rec_it == records.end())
            return false;
        const std::size_t erased_idx = static_cast<std::size_t>(rec_it - records.begin());
        records.erase(rec_it);
        order.erase(std::remove(order.begin(), order.end(), erased_idx), order.end());
        for (std::size_t &mapped : order)
        {
            if (mapped > erased_idx)
                --mapped;
        }
        return true;
    }

    ckv::widgets::TableCell cell(ckv::widgets::TableCellRef reference) const override
    {
        const ResultRowRecord *rec = record_by_id(reference.row);
        if (rec == nullptr)
            return {.value = std::string{}, .display = {}, .style = std::nullopt, .editable = false};

        switch (reference.column)
        {
        case 0:
            return {.value = rec->name, .display = {}, .style = std::nullopt, .editable = false};
        case 1:
            return {.value = rec->folder, .display = {}, .style = std::nullopt, .editable = false};
        case 2:
            return {.value = static_cast<std::int64_t>(rec->size_bytes),
                    .display = rec->size_display,
                    .style = std::nullopt,
                    .editable = false};
        case 3:
            return {.value = rec->modified_seconds,
                    .display = rec->modified_display,
                    .style = std::nullopt,
                    .editable = false};
        default:
            return {.value = std::string{}, .display = {}, .style = std::nullopt, .editable = false};
        }
    }

    void request_sort(std::optional<std::size_t> column, bool ascending) override
    {
        order.resize(records.size());
        for (std::size_t i = 0; i < records.size(); ++i)
            order[i] = i;
        if (!column.has_value())
            return;

        const std::size_t col = *column;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs_idx, std::size_t rhs_idx) {
            const ResultRowRecord &lhs = records[lhs_idx];
            const ResultRowRecord &rhs = records[rhs_idx];
            int cmp = 0;
            if (col == 0)
            {
                const std::string la = ascii_lower_copy(lhs.name);
                const std::string lb = ascii_lower_copy(rhs.name);
                cmp = la < lb ? -1 : (la > lb ? 1 : 0);
            }
            else if (col == 1)
            {
                const std::string la = ascii_lower_copy(lhs.folder);
                const std::string lb = ascii_lower_copy(rhs.folder);
                cmp = la < lb ? -1 : (la > lb ? 1 : 0);
            }
            else if (col == 2)
            {
                if (lhs.is_directory != rhs.is_directory)
                    cmp = lhs.is_directory ? -1 : 1;
                else
                    cmp = lhs.size_bytes < rhs.size_bytes ? -1 : (lhs.size_bytes > rhs.size_bytes ? 1 : 0);
            }
            else if (col == 3)
            {
                cmp = lhs.modified_seconds < rhs.modified_seconds
                          ? -1
                          : (lhs.modified_seconds > rhs.modified_seconds ? 1 : 0);
            }
            return ascending ? (cmp < 0) : (cmp > 0);
        });
    }
};

std::string preset_extensions_for(ck::find::GuidedTypePreset preset, const std::string &custom)
{
    switch (preset)
    {
    case ck::find::GuidedTypePreset::Documents:
        return "pdf,doc,docx,txt,md,rtf";
    case ck::find::GuidedTypePreset::Images:
        return "jpg,jpeg,png,gif,svg,webp,bmp";
    case ck::find::GuidedTypePreset::Audio:
        return "mp3,wav,flac,aac,ogg";
    case ck::find::GuidedTypePreset::Archives:
        return "zip,tar,tar.gz,tgz,rar,7z";
    case ck::find::GuidedTypePreset::Code:
        return "c,cpp,h,hpp,cc,hh,py,js,ts,java,rb,rs,go,swift,cs";
    case ck::find::GuidedTypePreset::Custom:
        return custom;
    case ck::find::GuidedTypePreset::All:
    default:
        return {};
    }
}

bool valid_iso_date_or_empty(const std::string &value)
{
    const std::string trimmed = trim_string(value);
    if (trimmed.empty())
        return true;
    int y = 0;
    int m = 0;
    int d = 0;
    char extra = '\0';
    if (std::sscanf(trimmed.c_str(), "%d-%d-%d%c", &y, &m, &d, &extra) != 3)
        return false;
    return y >= 1970 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

} // namespace

struct FindApp::QuickSearchWindowState
{
    ckv::widgets::DialogDescriptor descriptor;
    ckv::widgets::Window *window = nullptr;
    ckv::widgets::InputLine *pattern_input = nullptr;
    ckv::widgets::InputLine *content_input = nullptr;
    ckv::widgets::InputLine *folder_input = nullptr;
    ckv::widgets::CheckGroup *subdirs_check = nullptr;
    ckv::widgets::CheckGroup *hidden_check = nullptr;
    ckv::widgets::CheckGroup *case_check = nullptr;
    ckv::widgets::Label *summary_label = nullptr;
};

struct FindApp::ProgressDialogState
{
    ckv::widgets::ProgressModel model;
    std::unique_ptr<ckv::widgets::ProgressController> controller;
    ckv::widgets::Window *window = nullptr;
    ckv::widgets::Progress *bar = nullptr;
    ckv::widgets::Label *headline_label = nullptr;
    ckv::widgets::Label *counts_label = nullptr;
    ckv::widgets::Label *folder_label = nullptr;

    explicit ProgressDialogState(ckv::ui::Application &app)
        : controller(std::make_unique<ckv::widgets::ProgressController>(app, model))
    {
    }
};

struct FindApp::FoundFilesSession
{
    int number = 0;
    ck::find::SearchSpecification specification;
    ck::find::SearchExecutionResult result;
    FoundFilesTableModel model;
    ckv::widgets::Window *window = nullptr;
    ckv::widgets::Table *table = nullptr;
    ckv::widgets::TextView *preview = nullptr;
    ckv::widgets::Label *header_label = nullptr;
};

FindApp::FindApp(ckv::ui::Application &application,
                 FindSpecificationStore &specification_store,
                 FindExecutionService &execution_service)
    : FindApp(application,
              specification_store,
              execution_service,
              std::make_unique<ck::host::NativeFileSystem>(),
              nullptr,
              ".",
              "")
{
}

FindApp::FindApp(ckv::ui::Application &application,
                 FindSpecificationStore &specification_store,
                 FindExecutionService &execution_service,
                 ckv::FileSystem &files,
                 std::string_view initial_directory,
                 std::string_view initial_pattern)
    : FindApp(application,
              specification_store,
              execution_service,
              nullptr,
              &files,
              initial_directory,
              initial_pattern)
{
}

FindApp::FindApp(ckv::ui::Application &application,
                 FindSpecificationStore &specification_store,
                 FindExecutionService &execution_service,
                 std::unique_ptr<ckv::FileSystem> owned_files,
                 ckv::FileSystem *external_files,
                 std::string_view initial_directory,
                 std::string_view initial_pattern)
    : application_(application),
      specification_store_(specification_store),
      execution_service_(execution_service),
      owned_files_(std::move(owned_files)),
      files_(external_files != nullptr ? *external_files : *owned_files_),
      specification_(ck::find::makeDefaultSpecification()),
      quick_pattern_text_(initial_pattern),
      command_scope_(application.commands())
{
    const std::string start_dir = initial_directory.empty() ? std::string(".") : std::string(initial_directory);
    ck::find::copyToArray(specification_.startLocation, start_dir.c_str());
    specification_.enableTextSearch = false;
    if (!quick_pattern_text_.empty())
    {
        const std::string globs = normalize_quick_patterns(quick_pattern_text_);
        ck::find::copyToArray(specification_.includePatterns, globs.c_str());
    }

    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    viewers_ = std::make_unique<FindWorkspaceViewers>(application_, shell_->desktop(), files_);
    open_quick_search_window();
}

FindApp::~FindApp()
{
    // Completion callbacks are posted from a worker. Expire their gate before
    // requesting cancellation so an already queued callback cannot access a
    // presentation tree being torn down.
    lifetime_.reset();
    execution_service_.cancel();
    for (auto &session : results_sessions_)
    {
        if (session && session->table != nullptr)
            session->table->clear_model();
    }
    progress_dialog_.reset();
    quick_search_.reset();
    viewers_.reset();
}

void FindApp::declare_commands()
{
    new_search_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.new_search", [this] { open_quick_search_window(); }));
    save_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.save_search", [this] { handle_save_command(); }));
    load_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.load_search", [this] { show_load_dialog(); }));
    export_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.export_results", [this] { show_export_dialog(); }));
    execute_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.execute_search", [this] { request_execution(); }));
    cancel_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.cancel_search", [this] { cancel_execution(); }));
    advanced_filters_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.advanced_filters", [this] { show_advanced_filters_dialog(); }));
    presets_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.presets", [this] { show_presets_dialog(); }));

    view_file_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.view_file", [this] { view_selected_result(FindPreviewMode::Auto); }));
    view_text_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.view_text", [this] { view_selected_result(FindPreviewMode::Text); }));
    view_json_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.view_json", [this] { view_selected_result(FindPreviewMode::Json); }));
    view_hex_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.view_hex", [this] { view_selected_result(FindPreviewMode::Hex); }));
    edit_text_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.edit_text", [this] { edit_selected_result_text(); }));
    edit_hex_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.edit_hex", [this] { edit_selected_result_hex(); }));
    copy_path_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.copy_path", [this] { copy_selected_result_path(); }));
    delete_selected_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.delete_selected", [this] { delete_selected_result(); }));

    preview_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.preview_command", [this] { show_posix_command_dialog(); }));
    bulk_action_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-find", "ck.find.bulk_action", [this] { show_bulk_action_dialog(); }));
}

SuiteShellOptions FindApp::make_shell_options() const
{
    const auto &std_cmds = application_.commands().standard();
    return {
        .application_name = "ck Find",
        .about_text = "Find files interactively across folders with live progress, multiple result windows, exportable file lists, and integrated Text, JSON, and Hex viewers and editors.",
        .file_items = {
            MenuItem::command(CommandPresentation{new_search_command_, "&New search..."}),
            MenuItem::command(CommandPresentation{load_command_, "&Load saved search..."}),
            MenuItem::command(CommandPresentation{save_command_, "&Save search / file..."}),
            MenuItem::separator(),
            MenuItem::command(CommandPresentation{export_command_, "&Export found files to text..."}),
        },
        .application_menus = {
            MenuBarItem{"&Search", {
                MenuItem::command(CommandPresentation{new_search_command_, "&Find files..."}),
                MenuItem::command(CommandPresentation{execute_command_, "&Run search now"}),
                MenuItem::command(CommandPresentation{cancel_command_, "&Cancel running search"}),
                MenuItem::separator(),
                MenuItem::command(CommandPresentation{advanced_filters_command_, "&Advanced filters..."}),
                MenuItem::command(CommandPresentation{presets_command_, "Search &presets && recipes..."}),
            }},
            MenuBarItem{"&Results", {
                MenuItem::command(CommandPresentation{view_file_command_, "&View selected file"}),
                MenuItem::command(CommandPresentation{view_text_command_, "View as &Text"}),
                MenuItem::command(CommandPresentation{view_json_command_, "View as &JSON Tree"}),
                MenuItem::command(CommandPresentation{view_hex_command_, "View as &Hex"}),
                MenuItem::separator(),
                MenuItem::command(CommandPresentation{edit_text_command_, "&Edit Text / JSON..."}),
                MenuItem::command(CommandPresentation{edit_hex_command_, "Edit &Hex..."}),
                MenuItem::separator(),
                MenuItem::command(CommandPresentation{copy_path_command_, "Cop&y path"}),
                MenuItem::command(CommandPresentation{export_command_, "Export &list to text file..."}),
                MenuItem::command(CommandPresentation{delete_selected_command_, "&Delete selected file..."}),
            }},
            MenuBarItem{"&Tools", {
                MenuItem::command(CommandPresentation{preview_command_, "POSIX &find command..."}),
                MenuItem::command(CommandPresentation{bulk_action_command_, "&Bulk specification actions..."}),
            }},
            MenuBarItem{"&Window", {
                MenuItem::command(CommandPresentation{std_cmds.tile, "&Tile"}),
                MenuItem::command(CommandPresentation{std_cmds.tile_horizontally, "Tile &Horizontally"}),
                MenuItem::command(CommandPresentation{std_cmds.tile_vertically, "Tile &Vertically"}),
                MenuItem::command(CommandPresentation{std_cmds.cascade, "C&ascade"}),
                MenuItem::separator(),
                MenuItem::command(CommandPresentation{std_cmds.next_window, "&Next window"}),
                MenuItem::command(CommandPresentation{std_cmds.previous_window, "&Previous window"}),
                MenuItem::command(CommandPresentation{std_cmds.zoom, "&Zoom"}),
                MenuItem::command(CommandPresentation{std_cmds.minimize, "Mi&nimize"}),
                MenuItem::command(CommandPresentation{std_cmds.close, "&Close window"}),
                MenuItem::separator(),
                MenuItem::command(CommandPresentation{std_cmds.window_list, "&Window list..."}),
            }},
        },
        .application_status_items = {
            StatusLineItem{CommandPresentation{new_search_command_, "&New"}, 30},
            StatusLineItem{CommandPresentation{view_file_command_, "&View"}, 28},
            StatusLineItem{CommandPresentation{edit_text_command_, "&Edit"}, 28},
            StatusLineItem{CommandPresentation{edit_hex_command_, "&Hex"}, 24},
            StatusLineItem{CommandPresentation{export_command_, "E&xport"}, 25},
            StatusLineItem{CommandPresentation{execute_command_, "&Run"}, 22},
            StatusLineItem{CommandPresentation{cancel_command_, "&Cancel"}, 20},
        },
    };
}

void FindApp::open_quick_search_window()
{
    if (quick_search_ && quick_search_->window != nullptr)
    {
        sync_quick_search_from_specification();
        shell_->desktop().activate(quick_search_->window);
        if (quick_search_->pattern_input != nullptr)
            application_.set_focus(quick_search_->pattern_input);
        return;
    }

    auto state = std::make_unique<QuickSearchWindowState>();
    QuickSearchWindowState *const raw_state = state.get();
    const std::weak_ptr<void> weak_self = lifetime_;

    const std::string start_folder = ck::find::bufferToString(specification_.startLocation);
    auto &desc = raw_state->descriptor;
    desc.title = "Find Files";
    desc.resizable = true;
    desc.minimum_window_size = ckv::Size{68, 16};
    desc.fields = {
        {.label = "&File name or pattern:",
         .initial_text = quick_pattern_text_,
         .history_key = "ck.find.pattern"},
        {.label = "Containing &text:",
         .initial_text = quick_content_text_,
         .history_key = "ck.find.content"},
        {.label = "Search &in folder:",
         .initial_text = start_folder.empty() ? std::string(".") : start_folder,
         .validate = [](const std::string &value) { return !trim_string(value).empty(); },
         .history_key = "ck.find.folder"},
        {.label = "Search &subdirectories",
         .kind = ckv::widgets::FieldKind::Check,
         .initial_checked = specification_.includeSubdirectories},
        {.label = "Include &hidden files",
         .kind = ckv::widgets::FieldKind::Check,
         .initial_checked = specification_.includeHidden},
        {.label = "Match &case",
         .kind = ckv::widgets::FieldKind::Check,
         .initial_checked = specification_.textOptions.matchCase},
        {.label = build_filter_summary(specification_),
         .kind = ckv::widgets::FieldKind::Note},
    };

    desc.buttons = {
        {.label = "&Search",
         .role = ckv::widgets::ButtonRole::Accept,
         .on_press = [this, weak_self] {
             if (!weak_self.expired())
                 apply_quick_search_fields_and_run();
         }},
        {.label = "&Browse...",
         .role = ckv::widgets::ButtonRole::Neutral,
         .on_press = [this, weak_self] {
             if (!weak_self.expired())
                 browse_start_directory();
         }},
        {.label = "&Filters...",
         .role = ckv::widgets::ButtonRole::Neutral,
         .on_press = [this, weak_self] {
             if (!weak_self.expired())
                 show_advanced_filters_dialog();
         }},
        {.label = "&Presets...",
         .role = ckv::widgets::ButtonRole::Neutral,
         .on_press = [this, weak_self] {
             if (!weak_self.expired())
                 show_presets_dialog();
         }},
        {.label = "&Close",
         .role = ckv::widgets::ButtonRole::Dismiss,
         .on_press = nullptr},
    };

    ckv::widgets::MaterializedDialog mat = ckv::widgets::materialize_dialog(desc);
    raw_state->pattern_input = mat.inputs[0];
    raw_state->content_input = mat.inputs[1];
    raw_state->folder_input = mat.inputs[2];
    raw_state->subdirs_check = mat.checks[3];
    raw_state->hidden_check = mat.checks[4];
    raw_state->case_check = mat.checks[5];
    raw_state->summary_label = mat.labels[6];

    auto window = std::make_unique<ckv::widgets::Window>(desc.title);
    const auto &roles = shell_->roles();
    window->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
    window->set_content_margins(2, 1, 2, 0);
    window->set_min_size({56, 14});
    window->set_footer("Enter: Start recursive search | Ctrl+O: Saved searches");
    raw_state->window = window.get();

    const ckv::Rect area = shell_->desktop().content_area();
    if (area.width > 0 && area.height > 0)
    {
        const int width = std::min(76, area.width);
        const int height = std::min(16, area.height);
        window->set_bounds({area.x + (area.width - width) / 2,
                            area.y + (area.height - height) / 2,
                            width,
                            height});
    }

    ckv::ui::View *const initial_focus = mat.initial_focus;
    ckv::widgets::wire_dialog_window(*window, std::move(mat), desc, application_, nullptr);

    window->on_closed = [this, weak_self, raw_state] {
        if (weak_self.expired())
            return;
        ckv::widgets::Window *const closing = raw_state->window;
        raw_state->window = nullptr;
        std::unique_ptr<QuickSearchWindowState> keep_alive = std::move(quick_search_);
        if (closing != nullptr)
            shell_->desktop().remove_window(closing);
    };

    quick_search_ = std::move(state);
    shell_->desktop().present_modeless(ckv::widgets::WindowHandle{std::move(window), initial_focus}, application_);
}

void FindApp::sync_quick_search_from_specification()
{
    if (!quick_search_ || quick_search_->window == nullptr)
        return;
    if (quick_search_->pattern_input != nullptr)
        quick_search_->pattern_input->set_text(quick_pattern_text_);
    if (quick_search_->content_input != nullptr)
        quick_search_->content_input->set_text(quick_content_text_);
    if (quick_search_->folder_input != nullptr)
    {
        const std::string loc = ck::find::bufferToString(specification_.startLocation);
        quick_search_->folder_input->set_text(loc.empty() ? "." : loc);
    }
    if (quick_search_->subdirs_check != nullptr)
        quick_search_->subdirs_check->set_checked(0, specification_.includeSubdirectories);
    if (quick_search_->hidden_check != nullptr)
        quick_search_->hidden_check->set_checked(0, specification_.includeHidden);
    if (quick_search_->case_check != nullptr)
        quick_search_->case_check->set_checked(0, specification_.textOptions.matchCase);
    if (quick_search_->summary_label != nullptr)
        quick_search_->summary_label->set_text(build_filter_summary(specification_));
}

void FindApp::apply_quick_search_fields_and_run()
{
    if (quick_search_ && quick_search_->window != nullptr)
    {
        quick_pattern_text_ = quick_search_->pattern_input ? trim_string(quick_search_->pattern_input->text()) : std::string{};
        quick_content_text_ = quick_search_->content_input ? trim_string(quick_search_->content_input->text()) : std::string{};
        const std::string folder = quick_search_->folder_input ? trim_string(quick_search_->folder_input->text()) : std::string(".");
        ck::find::copyToArray(specification_.startLocation, (folder.empty() ? "." : folder).c_str());

        if (quick_search_->subdirs_check != nullptr)
            specification_.includeSubdirectories = quick_search_->subdirs_check->checked(0);
        if (quick_search_->hidden_check != nullptr)
            specification_.includeHidden = quick_search_->hidden_check->checked(0);
        if (quick_search_->case_check != nullptr)
            specification_.textOptions.matchCase = quick_search_->case_check->checked(0);

        const std::string normalized_globs = normalize_quick_patterns(quick_pattern_text_);
        ck::find::copyToArray(specification_.includePatterns, normalized_globs.c_str());
        ck::find::copyToArray(specification_.searchText, quick_content_text_.c_str());

        if (!quick_content_text_.empty())
        {
            specification_.enableTextSearch = true;
            specification_.textOptions.searchInContents = true;
            specification_.textOptions.searchInFileNames = false;
        }
        else
        {
            specification_.enableTextSearch = false;
        }

        // Standard interactive searches always list matches safely without
        // implicit deletion or custom commands.
        specification_.actionOptions.deleteMatches = false;
        specification_.actionOptions.execEnabled = false;
        specification_.actionOptions.print = true;

        if (quick_search_->window != nullptr)
            quick_search_->window->close();
    }

    start_execution(false);
}

void FindApp::browse_start_directory()
{
    std::string initial_dir = ".";
    if (quick_search_ && quick_search_->folder_input != nullptr)
    {
        const std::string typed = trim_string(quick_search_->folder_input->text());
        if (!typed.empty())
            initial_dir = typed;
    }
    if (!files_.is_directory(initial_dir))
        initial_dir = ".";

    const std::weak_ptr<void> weak_self = lifetime_;
    ckv::widgets::FileDialogOptions opts;
    opts.recent_locations_key = "ck.find.recent_dirs";
    file_dialog_.reset();
    file_dialog_.emplace(ckv::widgets::present_modal_file_dialog(
        ckv::widgets::FileDialogMode::Open, initial_dir, files_, std::move(opts), application_, shell_->desktop()));
    file_dialog_->set_completion_handler([this, weak_self](ckv::widgets::FileDialogResult res) {
        if (weak_self.expired() || !res.accepted || res.path.empty())
            return;
        std::string chosen = res.path;
        if (!files_.is_directory(chosen))
            chosen = files_.parent(chosen);
        ck::find::copyToArray(specification_.startLocation, chosen.c_str());
        if (quick_search_ && quick_search_->folder_input != nullptr)
            quick_search_->folder_input->set_text(chosen);
    });
}

void FindApp::show_advanced_filters_dialog()
{
    const ck::find::GuidedSearchState guided = ck::find::guidedStateFromSpecification(specification_);
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Advanced Search Filters";
    dialog.resizable = true;
    dialog.minimum_window_size = ckv::Size{72, 20};

    dialog.fields.push_back({"E&xclude patterns (e.g. *.o, *.tmp):",
                             ck::find::bufferToString(guided.excludePatterns),
                             nullptr});
    dialog.fields.push_back({.label = "File &type:",
                             .kind = ckv::widgets::FieldKind::Combo,
                             .options = {"All files", "Documents", "Images", "Audio", "Archives", "Source code", "Custom extensions"},
                             .initial_selection = static_cast<int>(guided.typePreset)});
    dialog.fields.push_back({"Custom e&xtensions (comma-separated):",
                             ck::find::bufferToString(guided.typeCustomExtensions),
                             nullptr});
    dialog.fields.push_back({.label = "Modified &date:",
                             .kind = ckv::widgets::FieldKind::Combo,
                             .options = {"Any time", "Past 24 hours", "Past 7 days", "Past 30 days", "Past 6 months", "Past year", "Custom YYYY-MM-DD range"},
                             .initial_selection = static_cast<int>(guided.datePreset)});
    dialog.fields.push_back({"Date &from (YYYY-MM-DD):",
                             ck::find::bufferToString(guided.dateFrom),
                             valid_iso_date_or_empty});
    dialog.fields.push_back({"Date &to (YYYY-MM-DD):",
                             ck::find::bufferToString(guided.dateTo),
                             valid_iso_date_or_empty});
    dialog.fields.push_back({.label = "File &size:",
                             .kind = ckv::widgets::FieldKind::Combo,
                             .options = {"Any size", "Larger than", "Smaller than", "Between", "Exactly", "Empty files (0 B)"},
                             .initial_selection = static_cast<int>(guided.sizePreset)});
    dialog.fields.push_back({"&Primary size (e.g. 10K, 5M):",
                             ck::find::bufferToString(guided.sizePrimary),
                             nullptr});
    dialog.fields.push_back({"&Secondary size (for Between):",
                             ck::find::bufferToString(guided.sizeSecondary),
                             nullptr});
    dialog.fields.push_back({"&Max folder depth (blank = unlimited):",
                             specification_.traversalOptions.maxDepthEnabled
                                 ? ck::find::bufferToString(specification_.traversalOptions.maxDepth)
                                 : std::string{},
                             [](const std::string &v) {
                                 const std::string t = trim_string(v);
                                 return t.empty() || std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                             }});
    dialog.fields.push_back({.label = "Content matching &mode:",
                             .kind = ckv::widgets::FieldKind::Combo,
                             .options = {"Contains substring", "Whole word", "Regular expression"},
                             .initial_selection = static_cast<int>(specification_.textOptions.mode)});
    dialog.fields.push_back({.label = "Follow symbolic &links",
                             .kind = ckv::widgets::FieldKind::Check,
                             .initial_checked = specification_.followSymlinks});
    dialog.fields.push_back({.label = "Stay on the same &filesystem",
                             .kind = ckv::widgets::FieldKind::Check,
                             .initial_checked = specification_.stayOnSameFilesystem});

    dialog.buttons.push_back({"&Apply Filters", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Reset Filters", ckv::widgets::ButtonRole::Neutral, [this] {
        const std::string loc = ck::find::bufferToString(specification_.startLocation);
        specification_ = ck::find::makeDefaultSpecification();
        ck::find::copyToArray(specification_.startLocation, loc.c_str());
        specification_.enableTextSearch = false;
        sync_quick_search_from_specification();
    }});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});

    const std::weak_ptr<void> weak_self = lifetime_;
    modal_dialog_.reset();
    modal_dialog_.emplace(ckv::widgets::present_modal_dialog(std::move(dialog), application_, shell_->desktop()));
    modal_dialog_->set_completion_handler([this, weak_self](ckv::widgets::DialogResult res) {
        if (weak_self.expired() || !res.accepted || res.values.size() < 13)
            return;

        ck::find::GuidedSearchState next = ck::find::guidedStateFromSpecification(specification_);
        ck::find::copyToArray(next.excludePatterns, trim_string(res.values[0]).c_str());
        const int type_sel = std::clamp(res.selected[1], 0, 6);
        next.typePreset = static_cast<ck::find::GuidedTypePreset>(type_sel);
        const std::string exts = preset_extensions_for(next.typePreset, trim_string(res.values[2]));
        ck::find::copyToArray(next.typeCustomExtensions, exts.c_str());

        const int date_sel = std::clamp(res.selected[3], 0, 6);
        next.datePreset = static_cast<ck::find::GuidedDatePreset>(date_sel);
        ck::find::copyToArray(next.dateFrom, trim_string(res.values[4]).c_str());
        ck::find::copyToArray(next.dateTo, trim_string(res.values[5]).c_str());

        const int size_sel = std::clamp(res.selected[6], 0, 5);
        next.sizePreset = static_cast<ck::find::GuidedSizePreset>(size_sel);
        ck::find::copyToArray(next.sizePrimary, trim_string(res.values[7]).c_str());
        ck::find::copyToArray(next.sizeSecondary, trim_string(res.values[8]).c_str());

        next.followSymlinks = res.checked[11];
        next.stayOnSameFilesystem = res.checked[12];
        next.textMode = static_cast<ck::find::TextSearchOptions::Mode>(std::clamp(res.selected[10], 0, 2));

        const std::string saved_include = ck::find::bufferToString(specification_.includePatterns);
        const std::string saved_content = ck::find::bufferToString(specification_.searchText);
        const bool had_content = !trim_string(saved_content).empty();

        ck::find::applyGuidedStateToSpecification(next, specification_);
        ck::find::copyToArray(specification_.includePatterns, saved_include.c_str());
        ck::find::copyToArray(specification_.searchText, saved_content.c_str());
        specification_.enableTextSearch = had_content;
        specification_.textOptions.searchInContents = had_content;
        specification_.textOptions.searchInFileNames = false;

        const std::string max_depth = trim_string(res.values[9]);
        specification_.enableTraversalFilters = !max_depth.empty();
        specification_.traversalOptions.maxDepthEnabled = !max_depth.empty();
        ck::find::copyToArray(specification_.traversalOptions.maxDepth, max_depth.c_str());

        sync_quick_search_from_specification();
    });
}

void FindApp::show_presets_dialog()
{
    struct PresetChoice
    {
        std::string label;
        void (*apply)(ck::find::GuidedSearchState &) = nullptr;
    };

    auto choices = std::make_shared<std::vector<PresetChoice>>();
    std::vector<std::string> items;
    for (const auto &p : ck::find::popularSearchPresets())
    {
        choices->push_back({std::string(p.title) + " — " + std::string(p.subtitle), p.apply});
        items.push_back(choices->back().label);
    }
    for (const auto &r : ck::find::expertSearchRecipes())
    {
        choices->push_back({"[Recipe] " + std::string(r.title) + " — " + std::string(r.description), r.apply});
        items.push_back(choices->back().label);
    }

    auto window = std::make_unique<ckv::widgets::Window>("Search Presets & Recipes");
    auto *const win_ptr = window.get();
    const auto &roles = shell_->roles();
    window->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
    window->set_content_margins(2, 1, 2, 0);
    const ckv::Rect area = shell_->desktop().content_area();
    const int width = std::min(74, area.width);
    const int height = std::min(16, area.height);
    window->set_bounds({area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height});
    window->cancel_request = [win_ptr] { win_ptr->close(); };

    auto col = std::make_unique<ckv::ui::Column>();
    col->set_spacing(1);
    auto list = std::make_unique<ckv::widgets::ListView>();
    auto *const list_ptr = list.get();
    list->set_items(std::move(items));
    col->add_item(std::move(list), {.policy = ckv::ui::SizePolicy::Expanding});

    const std::weak_ptr<void> weak_self = lifetime_;
    auto apply_selected = [this, weak_self, win_ptr, list_ptr, choices](bool run_immediately) {
        if (weak_self.expired())
            return;
        const int idx = list_ptr->cursor();
        if (idx < 0 || static_cast<std::size_t>(idx) >= choices->size())
            return;
        ck::find::GuidedSearchState state = ck::find::guidedStateFromSpecification(specification_);
        if ((*choices)[static_cast<std::size_t>(idx)].apply != nullptr)
            (*choices)[static_cast<std::size_t>(idx)].apply(state);
        const std::string keep_folder = ck::find::bufferToString(specification_.startLocation);
        ck::find::applyGuidedStateToSpecification(state, specification_);
        ck::find::copyToArray(specification_.startLocation, (keep_folder.empty() ? "." : keep_folder).c_str());
        specification_.enableTextSearch = false;
        sync_quick_search_from_specification();
        win_ptr->close();
        if (run_immediately)
            start_execution(false);
        else
            open_quick_search_window();
    };

    window->accept_request = [apply_selected] { apply_selected(true); };

    auto bar = std::make_unique<ckv::ui::Row>();
    bar->set_spacing(1);
    auto run_btn = std::make_unique<ckv::widgets::Button>("&Search Now");
    run_btn->on_press = [apply_selected] { apply_selected(true); };
    bar->add_item(std::move(run_btn));

    auto load_btn = std::make_unique<ckv::widgets::Button>("&Load into Form");
    load_btn->on_press = [apply_selected] { apply_selected(false); };
    bar->add_item(std::move(load_btn));

    auto cancel_btn = std::make_unique<ckv::widgets::Button>("&Cancel");
    cancel_btn->on_press = [win_ptr] { win_ptr->close(); };
    bar->add_item(std::move(cancel_btn));

    col->add_item(std::move(bar));
    window->set_content(std::move(col));
    shell_->desktop().present_modal(ckv::widgets::WindowHandle{std::move(window), list_ptr}, application_);
}

void FindApp::handle_save_command()
{
    if (viewers_ && viewers_->save_active_document())
        return;
    show_save_dialog();
}

void FindApp::show_save_dialog()
{
    if (quick_search_ && quick_search_->window != nullptr)
    {
        quick_pattern_text_ = quick_search_->pattern_input ? trim_string(quick_search_->pattern_input->text()) : quick_pattern_text_;
        quick_content_text_ = quick_search_->content_input ? trim_string(quick_search_->content_input->text()) : quick_content_text_;
        if (quick_search_->folder_input != nullptr)
            ck::find::copyToArray(specification_.startLocation, trim_string(quick_search_->folder_input->text()).c_str());
        ck::find::copyToArray(specification_.includePatterns, normalize_quick_patterns(quick_pattern_text_).c_str());
        ck::find::copyToArray(specification_.searchText, quick_content_text_.c_str());
    }

    const std::string current_name = ck::find::normaliseSpecificationName(ck::find::bufferToString(specification_.specName));
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Save Search Specification";
    dialog.minimum_window_size = {52, 8};
    dialog.fields.push_back({"&Search name:", current_name.empty() ? "My Search" : current_name,
                             [](const std::string &value) {
                                 return !ck::find::normaliseSpecificationName(value).empty();
                             }});
    dialog.buttons.push_back({"&Save", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});

    const std::weak_ptr<void> weak_self = lifetime_;
    modal_dialog_.reset();
    modal_dialog_.emplace(ckv::widgets::present_modal_dialog(std::move(dialog), application_, shell_->desktop()));
    modal_dialog_->set_completion_handler([this, weak_self](ckv::widgets::DialogResult result) {
        if (weak_self.expired() || !result.accepted || result.values.empty())
            return;
        const std::string name = ck::find::normaliseSpecificationName(result.values.front());
        if (name.empty())
            return;

        ck::find::SearchSpecification saved = specification_;
        ck::find::copyToArray(saved.specName, name.c_str());
        if (!specification_store_.save(saved, name))
        {
            show_message(ckv::widgets::MessageBoxKind::Error, "Save search", "Could not save specification '" + name + "'.");
            return;
        }
        specification_ = std::move(saved);
        show_message(ckv::widgets::MessageBoxKind::Info, "Save search", "Saved search specification '" + name + "'.");
    });
}

void FindApp::show_load_dialog()
{
    auto saved_list = std::make_shared<std::vector<ck::find::SavedSpecification>>(specification_store_.list());
    if (saved_list->empty())
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Load saved search", "No saved search specifications are available yet.");
        return;
    }

    std::vector<std::string> captions;
    captions.reserve(saved_list->size());
    for (const auto &item : *saved_list)
        captions.push_back(item.name);

    auto window = std::make_unique<ckv::widgets::Window>("Load Saved Search");
    auto *const win_ptr = window.get();
    const auto &roles = shell_->roles();
    window->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
    window->set_content_margins(2, 1, 2, 0);
    const ckv::Rect area = shell_->desktop().content_area();
    const int width = std::min(62, area.width);
    const int height = std::min(15, area.height);
    window->set_bounds({area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height});
    window->cancel_request = [win_ptr] { win_ptr->close(); };

    auto col = std::make_unique<ckv::ui::Column>();
    col->set_spacing(1);
    auto list = std::make_unique<ckv::widgets::ListView>();
    auto *const list_ptr = list.get();
    list->set_items(std::move(captions));
    col->add_item(std::move(list), {.policy = ckv::ui::SizePolicy::Expanding});

    const std::weak_ptr<void> weak_self = lifetime_;
    auto load_selected = [this, weak_self, win_ptr, list_ptr, saved_list](bool run_now) {
        if (weak_self.expired())
            return;
        const int idx = list_ptr->cursor();
        if (idx < 0 || static_cast<std::size_t>(idx) >= saved_list->size())
            return;
        const std::string &name = (*saved_list)[static_cast<std::size_t>(idx)].name;
        const auto loaded = specification_store_.load(name);
        if (!loaded)
        {
            win_ptr->close();
            show_message(ckv::widgets::MessageBoxKind::Error, "Load saved search", "Could not load '" + name + "'.");
            return;
        }
        set_specification(*loaded);
        win_ptr->close();
        if (run_now)
            request_execution();
        else
            open_quick_search_window();
    };

    window->accept_request = [load_selected] { load_selected(true); };

    auto bar = std::make_unique<ckv::ui::Row>();
    bar->set_spacing(1);
    auto run_btn = std::make_unique<ckv::widgets::Button>("&Load && Search");
    run_btn->on_press = [load_selected] { load_selected(true); };
    bar->add_item(std::move(run_btn));

    auto edit_btn = std::make_unique<ckv::widgets::Button>("Load &Form");
    edit_btn->on_press = [load_selected] { load_selected(false); };
    bar->add_item(std::move(edit_btn));

    auto delete_btn = std::make_unique<ckv::widgets::Button>("&Delete");
    delete_btn->on_press = [this, weak_self, list_ptr, saved_list] {
        if (weak_self.expired())
            return;
        const int idx = list_ptr->cursor();
        if (idx < 0 || static_cast<std::size_t>(idx) >= saved_list->size())
            return;
        const std::string target = (*saved_list)[static_cast<std::size_t>(idx)].name;
        if (specification_store_.remove(target))
        {
            saved_list->erase(saved_list->begin() + idx);
            std::vector<std::string> remaining;
            for (const auto &it : *saved_list)
                remaining.push_back(it.name);
            list_ptr->set_items(std::move(remaining));
        }
    };
    bar->add_item(std::move(delete_btn));

    auto cancel_btn = std::make_unique<ckv::widgets::Button>("&Cancel");
    cancel_btn->on_press = [win_ptr] { win_ptr->close(); };
    bar->add_item(std::move(cancel_btn));

    col->add_item(std::move(bar));
    window->set_content(std::move(col));
    shell_->desktop().present_modal(ckv::widgets::WindowHandle{std::move(window), list_ptr}, application_);
}

std::string FindApp::command_preview() const
{
    const bool custom_command = specification_.enableActionOptions && specification_.actionOptions.execEnabled;
    const auto command = ck::find::buildFindCommand(specification_, !custom_command);
    std::ostringstream output;
    for (std::size_t index = 0; index < command.size(); ++index)
    {
        if (index != 0)
            output << ' ';
        output << command[index];
    }
    if (custom_command)
    {
        const FindCustomCommandCapability capability = execution_service_.custom_command_capability(specification_);
        output << "\n\nSandboxed custom-command argv:\n";
        if (!capability.available)
        {
            output << "Unavailable: " << capability.reason;
        }
        else
        {
            for (const std::string &argument : capability.argv_preview)
                output << '[' << argument << "] ";
        }
    }
    return output.str();
}

void FindApp::set_specification(ck::find::SearchSpecification specification)
{
    specification_ = std::move(specification);
    quick_pattern_text_ = ck::find::bufferToString(specification_.includePatterns);
    quick_content_text_ = ck::find::bufferToString(specification_.searchText);
    sync_quick_search_from_specification();
}

void FindApp::show_posix_command_dialog()
{
    const std::string cmd = command_preview();
    auto window = std::make_unique<ckv::widgets::Window>("POSIX find(1) Command Preview");
    auto *const win_ptr = window.get();
    const auto &roles = shell_->roles();
    window->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
    window->set_content_margins(1, 1, 1, 0);
    const ckv::Rect area = shell_->desktop().content_area();
    const int width = std::min(82, area.width);
    const int height = std::min(14, area.height);
    window->set_bounds({area.x + (area.width - width) / 2, area.y + (area.height - height) / 2, width, height});
    window->cancel_request = [win_ptr] { win_ptr->close(); };

    auto col = std::make_unique<ckv::ui::Column>();
    col->set_spacing(1);
    auto tv = std::make_unique<ckv::widgets::TextView>();
    auto *const tv_ptr = tv.get();
    tv->set_wrap_mode(ckv::widgets::WrapMode::Word);
    tv->set_text(cmd);
    col->add_item(std::move(tv), {.policy = ckv::ui::SizePolicy::Expanding});

    auto bar = std::make_unique<ckv::ui::Row>();
    bar->set_spacing(1);
    const std::weak_ptr<void> weak_self = lifetime_;
    auto copy_btn = std::make_unique<ckv::widgets::Button>("Cop&y Command");
    copy_btn->on_press = [this, weak_self, cmd, win_ptr] {
        if (!weak_self.expired())
        {
            application_.set_clipboard_text(cmd);
            win_ptr->set_footer("Copied POSIX find command to clipboard.");
        }
    };
    bar->add_item(std::move(copy_btn));

    auto close_btn = std::make_unique<ckv::widgets::Button>("&Close");
    close_btn->on_press = [win_ptr] { win_ptr->close(); };
    bar->add_item(std::move(close_btn));

    col->add_item(std::move(bar));
    window->set_content(std::move(col));
    shell_->desktop().present_modeless(ckv::widgets::WindowHandle{std::move(window), tv_ptr}, application_);
}

void FindApp::show_bulk_action_dialog()
{
    const std::string existing_cmd = ck::find::bufferToString(specification_.actionOptions.execCommand);
    int selected_mode = 0;
    if (specification_.enableActionOptions && specification_.actionOptions.deleteMatches)
        selected_mode = 3;
    else if (specification_.enableActionOptions && specification_.actionOptions.execEnabled)
        selected_mode = (existing_cmd == "ckvision.file-info") ? 1 : (existing_cmd == "ckvision.sha256" ? 2 : 0);

    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Power-User Bulk Specification Action";
    dialog.minimum_window_size = {68, 15};
    dialog.fields = {
        {.label = "Action on matched entries:",
         .kind = ckv::widgets::FieldKind::Radio,
         .options = {
             "List matches in Found Files window (safe default)",
             "Sandboxed macOS File Metadata (stat)",
             "Sandboxed macOS SHA-256 Digest (shasum -a 256)",
             "Delete matched regular files / symlinks (requires confirmation)",
         },
         .initial_selection = selected_mode},
        {.label = "Sandboxed actions run via /usr/bin/sandbox-exec with network and writes denied (max 64 files).",
         .kind = ckv::widgets::FieldKind::Note},
    };
    dialog.buttons = {
        {"&Execute", ckv::widgets::ButtonRole::Accept, nullptr},
        {"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr},
    };

    const std::weak_ptr<void> weak_self = lifetime_;
    modal_dialog_.reset();
    modal_dialog_.emplace(ckv::widgets::present_modal_dialog(std::move(dialog), application_, shell_->desktop()));
    modal_dialog_->set_completion_handler([this, weak_self](ckv::widgets::DialogResult res) {
        if (weak_self.expired() || !res.accepted || res.selected.empty())
            return;
        const int choice = res.selected[0];
        specification_.enableActionOptions = true;
        specification_.actionOptions.deleteMatches = (choice == 3);
        specification_.actionOptions.execEnabled = (choice == 1 || choice == 2);
        specification_.actionOptions.execUsePlus = false;
        specification_.actionOptions.execVariant = ck::find::ActionOptions::ExecVariant::Exec;
        if (choice == 1)
            ck::find::copyToArray(specification_.actionOptions.execCommand, "ckvision.file-info");
        else if (choice == 2)
            ck::find::copyToArray(specification_.actionOptions.execCommand, "ckvision.sha256");
        request_execution();
    });
}

void FindApp::request_execution()
{
    if (execution_service_.running())
    {
        if (progress_dialog_ && progress_dialog_->window != nullptr)
            shell_->desktop().activate(progress_dialog_->window);
        else
            show_message(ckv::widgets::MessageBoxKind::Info,
                         "Search in progress",
                         "A search is already running. Cancel it first to start a different search.");
        return;
    }

    if (specification_.enableActionOptions && specification_.actionOptions.execEnabled)
    {
        if (specification_.actionOptions.deleteMatches)
        {
            show_message(ckv::widgets::MessageBoxKind::Warning,
                         "Conflicting actions",
                         "Custom commands cannot be combined with deletion. Run one confirmed action at a time.");
            return;
        }
        const FindCustomCommandCapability capability = execution_service_.custom_command_capability(specification_);
        if (!capability.available)
        {
            show_message(ckv::widgets::MessageBoxKind::Warning,
                         "Custom command unavailable",
                         "Custom command unavailable: " + capability.reason);
            return;
        }

        std::ostringstream preview;
        for (const std::string &argument : capability.argv_preview)
            preview << '[' << argument << "] ";
        custom_command_confirmation_.reset();
        custom_command_confirmation_.emplace(ckv::widgets::present_modal_message_box(
            application_, shell_->desktop(),
            {ckv::widgets::MessageBoxKind::Warning,
             "Run sandboxed custom command",
             "Run the fixed, non-interactive command for matching paths? It has a 5-second per-path limit, 16 KiB output limit, and stops after 64 paths.\n\nDirect argv:\n" +
                 preview.str(),
             ckv::widgets::MessageBoxButtons::YesNoCancel}));
        const std::weak_ptr<void> weak_self = lifetime_;
        custom_command_confirmation_->set_completion_handler([this, weak_self](ckv::widgets::MessageBoxResult result) {
            if (!weak_self.expired() && result == ckv::widgets::MessageBoxResult::Yes)
                start_execution(false);
        });
        return;
    }

    if (specification_.enableActionOptions && specification_.actionOptions.deleteMatches)
    {
        destructive_confirmation_.reset();
        destructive_confirmation_.emplace(ckv::widgets::present_modal_message_box(
            application_, shell_->desktop(),
            {ckv::widgets::MessageBoxKind::Warning,
             "Delete matched files",
             "Delete matching regular files and symbolic links under '" +
                 ck::find::bufferToString(specification_.startLocation) +
                 "'? Directories will not be deleted. This cannot be undone.",
             ckv::widgets::MessageBoxButtons::YesNoCancel}));
        const std::weak_ptr<void> weak_self = lifetime_;
        destructive_confirmation_->set_completion_handler([this, weak_self](ckv::widgets::MessageBoxResult result) {
            if (!weak_self.expired() && result == ckv::widgets::MessageBoxResult::Yes)
                start_execution(true);
        });
        return;
    }

    start_execution(false);
}

void FindApp::open_progress_dialog(const ck::find::SearchSpecification &spec, bool delete_matched_files)
{
    close_progress_dialog();

    auto state = std::make_unique<ProgressDialogState>(application_);
    ProgressDialogState *const raw_state = state.get();

    const std::string root = ck::find::bufferToString(spec.startLocation);
    const std::string pattern = ck::find::bufferToString(spec.includePatterns);
    const bool custom_cmd = spec.enableActionOptions && spec.actionOptions.execEnabled;

    std::string headline = delete_matched_files
                               ? "Deleting matched files in '" + root + "'..."
                               : custom_cmd ? "Running sandboxed command in '" + root + "'..."
                                            : "Searching '" + (root.empty() ? std::string(".") : root) + "'";
    if (!pattern.empty())
        headline += "  (" + pattern + ")";

    auto window = std::make_unique<ckv::widgets::Window>("Searching Files...");
    raw_state->window = window.get();
    const auto &roles = shell_->roles();
    window->set_role_override(roles.dialog_frame, roles.dialog_background, roles.dialog_frame, roles.dialog_background);
    window->set_content_margins(2, 1, 2, 0);
    window->set_resizable(false);
    window->set_minimizable(false);

    const ckv::Rect area = shell_->desktop().content_area();
    const int width = std::min(68, std::max(44, area.width));
    const int height = std::min(11, std::max(8, area.height));
    if (area.width > 0 && area.height > 0)
    {
        window->set_bounds({area.x + (area.width - width) / 2,
                            area.y + (area.height - height) / 2,
                            width,
                            height});
    }

    auto col = std::make_unique<ckv::ui::Column>();
    col->set_spacing(1);

    auto headline_lbl = std::make_unique<ckv::widgets::Label>(shorten_middle(headline, 60));
    raw_state->headline_label = headline_lbl.get();
    col->add_item(std::move(headline_lbl));

    auto bar = std::make_unique<ckv::widgets::Progress>();
    bar->set_indeterminate(true);
    bar->set_presentation(ckv::widgets::ProgressPresentation::Smooth);
    bar->set_activity_style(ckv::widgets::ProgressActivityStyle::Sweep);
    bar->set_label("Scanning...");
    raw_state->bar = bar.get();
    raw_state->controller->bind(*bar);
    col->add_item(std::move(bar));

    auto counts_lbl = std::make_unique<ckv::widgets::Label>("Scanned: 0 entries (0 folders)  |  Matches: 0");
    raw_state->counts_label = counts_lbl.get();
    col->add_item(std::move(counts_lbl));

    auto folder_lbl = std::make_unique<ckv::widgets::Label>("Folder: " + shorten_middle(root.empty() ? "." : root, 52));
    raw_state->folder_label = folder_lbl.get();
    col->add_item(std::move(folder_lbl));

    const std::weak_ptr<void> weak_self = lifetime_;
    auto btn_row = std::make_unique<ckv::ui::Row>();
    auto cancel_btn = std::make_unique<ckv::widgets::Button>("&Cancel Search");
    auto *const cancel_btn_ptr = cancel_btn.get();
    cancel_btn->on_press = [this, weak_self] {
        if (!weak_self.expired())
            cancel_execution();
    };
    btn_row->add_item(std::move(cancel_btn));
    col->add_item(std::move(btn_row));

    window->cancel_request = [this, weak_self] {
        if (!weak_self.expired())
            cancel_execution();
    };
    window->on_closed = [this, weak_self, raw_state] {
        if (weak_self.expired())
            return;
        if (execution_service_.running())
            execution_service_.cancel();
        ckv::widgets::Window *const closing = raw_state->window;
        raw_state->window = nullptr;
        std::unique_ptr<ProgressDialogState> keep_alive = std::move(progress_dialog_);
        if (closing != nullptr)
            shell_->desktop().remove_window(closing);
    };

    window->set_content(std::move(col));
    progress_dialog_ = std::move(state);
    shell_->desktop().present_modeless(ckv::widgets::WindowHandle{std::move(window), cancel_btn_ptr}, application_);
}

void FindApp::close_progress_dialog()
{
    if (!progress_dialog_)
        return;
    std::unique_ptr<ProgressDialogState> closing_state = std::move(progress_dialog_);
    if (closing_state->window != nullptr)
    {
        ckv::widgets::Window *const win = closing_state->window;
        closing_state->window = nullptr;
        win->on_closed = nullptr;
        shell_->desktop().remove_window(win);
    }
}

void FindApp::update_progress_dialog(const ck::find::SearchProgressSnapshot &snapshot)
{
    if (!progress_dialog_ || progress_dialog_->window == nullptr)
        return;
    if (progress_dialog_->bar != nullptr)
        progress_dialog_->bar->set_label(std::to_string(snapshot.matchCount) + " found");
    if (progress_dialog_->counts_label != nullptr)
    {
        progress_dialog_->counts_label->set_text(
            "Scanned: " + std::to_string(snapshot.scannedEntries) + " entries (" +
            std::to_string(snapshot.scannedDirectories) + " folders)  |  Matches: " +
            std::to_string(snapshot.matchCount));
    }
    if (progress_dialog_->folder_label != nullptr)
    {
        const std::string cur = snapshot.currentDirectory.string();
        progress_dialog_->folder_label->set_text("Folder: " + shorten_middle(cur.empty() ? "." : cur, 52));
    }
}

void FindApp::start_execution(bool delete_matched_files)
{
    last_execution_result_.reset();
    const std::uint64_t generation = ++active_search_generation_;
    const ck::find::SearchSpecification executed_spec = specification_;

    open_progress_dialog(executed_spec, delete_matched_files);

    const std::weak_ptr<void> lifetime = lifetime_;
    auto *const application = &application_;
    auto *const self = this;

    execution_service_.start_with_progress(
        executed_spec,
        delete_matched_files,
        [application, lifetime, self, generation](ck::find::SearchProgressSnapshot snapshot) {
            if (lifetime.expired())
                return;
            application->post([self, lifetime, generation, snap = std::move(snapshot)] {
                if (lifetime.expired() || self->active_search_generation_ != generation)
                    return;
                self->update_progress_dialog(snap);
            });
        },
        [application, lifetime, self, generation, spec_for_cb = executed_spec](ck::find::SearchExecutionResult result) mutable {
            if (lifetime.expired())
                return;
            application->post([self, lifetime, generation, spec_copy = std::move(spec_for_cb), res = std::move(result)]() mutable {
                if (lifetime.expired() || self->active_search_generation_ != generation)
                    return;
                self->complete_execution(std::move(spec_copy), std::move(res));
            });
        });
}

void FindApp::cancel_execution()
{
    if (!execution_service_.running())
        return;
    execution_service_.cancel();
    if (progress_dialog_ && progress_dialog_->bar != nullptr)
        progress_dialog_->bar->set_label("Cancelling...");
}

void FindApp::complete_execution(ck::find::SearchSpecification executed_spec,
                                 ck::find::SearchExecutionResult result)
{
    close_progress_dialog();
    last_execution_result_ = result;

    const bool bulk_action_ran = result.deletedCount != 0 ||
                                 result.failedDeletionCount != 0 ||
                                 result.customCommandInvocationCount != 0 ||
                                 !result.customCommandAudit.empty();
    if (bulk_action_ran)
    {
        create_bulk_action_report_window(executed_spec, result);
        return;
    }

    create_found_files_window(executed_spec, std::move(result));
}

void FindApp::create_found_files_window(const ck::find::SearchSpecification &executed_spec,
                                        ck::find::SearchExecutionResult result)
{
    auto session = std::make_unique<FoundFilesSession>();
    session->number = next_results_number_++;
    session->specification = executed_spec;

    const std::string root_str = ck::find::bufferToString(executed_spec.startLocation);
    const std::filesystem::path root_path = ck::host::utf8_path(root_str.empty() ? "." : root_str);

    std::vector<ResultRowRecord> rows;
    const std::size_t entry_count = !result.matchEntries.empty() ? result.matchEntries.size() : result.matches.size();
    rows.reserve(entry_count);

    for (std::size_t i = 0; i < entry_count; ++i)
    {
        ck::find::SearchMatchEntry meta;
        if (i < result.matchEntries.size())
            meta = result.matchEntries[i];
        else
            meta.path = result.matches[i];

        ResultRowRecord rec;
        rec.id = static_cast<ckv::widgets::TableRowId>(i + 1U);
        rec.full_path = meta.path.string();
        rec.name = meta.path.filename().string();
        if (rec.name.empty())
            rec.name = rec.full_path;
        if (meta.isDirectory && !rec.name.empty() && rec.name.back() != '/')
            rec.name.push_back('/');

        std::error_code rel_ec;
        const std::filesystem::path parent_dir = meta.path.parent_path();
        const std::filesystem::path rel_parent = std::filesystem::relative(parent_dir, root_path, rel_ec);
        if (!rel_ec && !rel_parent.empty() && rel_parent.string().substr(0, 2) != "..")
            rec.folder = rel_parent.generic_string();
        else
            rec.folder = parent_dir.empty() ? "." : parent_dir.string();

        rec.size_bytes = meta.sizeBytes;
        rec.modified_seconds = meta.modifiedUnixSeconds;
        rec.is_directory = meta.isDirectory;
        rec.size_display = format_human_size(meta.sizeBytes, meta.isDirectory, meta.sizeKnown);
        rec.modified_display = format_modified_time(meta.modifiedUnixSeconds, meta.modifiedKnown);
        rows.push_back(std::move(rec));
    }

    session->result = std::move(result);
    session->model.set_records(std::move(rows));

    const std::string pattern = trim_string(ck::find::bufferToString(executed_spec.includePatterns));
    const std::string content_term = trim_string(ck::find::bufferToString(executed_spec.searchText));
    std::string query_desc = !pattern.empty() ? pattern : (!content_term.empty() ? ("\"" + content_term + "\"") : "All files");

    std::ostringstream title;
    title << "Found #" << session->number << ": " << query_desc << " (" << session->result.matchCount
          << (session->result.matchCount == 1 ? " file" : " files")
          << (session->result.cancelled ? ", cancelled" : "") << ")";

    auto window = std::make_unique<ckv::widgets::Window>(title.str());
    FoundFilesSession *const raw_session = session.get();
    raw_session->window = window.get();
    window->set_min_size({64, 14});

    const ckv::Rect area = shell_->desktop().content_area();
    if (area.width > 0 && area.height > 0)
    {
        if (results_sessions_.empty())
        {
            window->set_bounds(area);
        }
        else
        {
            const int offset = static_cast<int>(results_sessions_.size() % 5U) * 2;
            const int w = std::max(64, area.width - offset);
            const int h = std::max(14, area.height - (offset / 2));
            window->set_bounds({area.x + std::min(offset, std::max(0, area.width - w)),
                                area.y + std::min(offset / 2, std::max(0, area.height - h)),
                                std::min(w, area.width),
                                std::min(h, area.height)});
        }
    }

    auto root_col = std::make_unique<ckv::ui::Column>();
    root_col->set_spacing(0);

    std::ostringstream summary;
    summary << "Folder: " << (root_str.empty() ? "." : root_str) << "  |  Query: " << query_desc;
    if (!content_term.empty() && !pattern.empty())
        summary << " containing \"" << content_term << "\"";
    summary << "  |  " << session->result.matchCount << " match(es)";
    if (session->result.scannedEntries > 0)
        summary << " of " << session->result.scannedEntries << " scanned";
    if (session->result.matchCount > session->model.records.size())
        summary << " (showing first " << session->model.records.size() << ")";

    auto hdr = std::make_unique<ckv::widgets::Label>(summary.str());
    raw_session->header_label = hdr.get();
    root_col->add_item(std::move(hdr));

    auto table = std::make_unique<ckv::widgets::Table>();
    raw_session->table = table.get();
    table->set_banded_rows(true);
    table->set_column_dividers(true);
    table->set_columns({
        {"Name", 28, 10, ckv::widgets::TableCellType::Text, false},
        {"Folder", 36, 10, ckv::widgets::TableCellType::Text, false},
        {"Size", 10, 6, ckv::widgets::TableCellType::Integer, false},
        {"Modified", 16, 10, ckv::widgets::TableCellType::Integer, false},
    });
    table->set_model(raw_session->model);

    auto preview = std::make_unique<ckv::widgets::TextView>();
    raw_session->preview = preview.get();
    preview->set_wrap_mode(ckv::widgets::WrapMode::None);

    auto splitter = std::make_unique<ckv::widgets::Splitter>(
        std::move(table), std::move(preview), ckv::widgets::Orientation::Vertical);
    splitter->set_resize_anchor(ckv::widgets::SplitterPane::Second);
    splitter->set_anchored_extent(6);
    root_col->add_item(std::move(splitter), {.policy = ckv::ui::SizePolicy::Expanding});

    const std::weak_ptr<void> weak_self = lifetime_;

    auto selected_path_of = [raw_session]() -> std::string {
        if (raw_session->table == nullptr)
            return {};
        const auto cell = raw_session->table->selected_cell();
        if (!cell)
            return {};
        const ResultRowRecord *rec = raw_session->model.record_by_id(cell->row);
        return rec != nullptr ? rec->full_path : std::string{};
    };

    auto refresh_selection = [this, weak_self, raw_session, selected_path_of] {
        if (weak_self.expired() || raw_session->window == nullptr)
            return;
        last_active_results_ = raw_session;
        const std::string path = selected_path_of();
        if (raw_session->preview != nullptr)
        {
            if (raw_session->model.row_count() == 0)
            {
                raw_session->preview->set_text("No files matched this search. Press Ctrl+N (or click New Search) to adjust criteria.");
            }
            else
            {
                raw_session->preview->set_text(viewers_->inline_preview_for(path));
                raw_session->preview->set_top_line(0);
            }
        }
        raw_session->window->set_footer(
            path.empty() ? "0 matches | Ctrl+N New Search"
                         : shorten_middle(path, 56) + " | Enter/F3 View | F4 Edit | Ctrl+E Export");
    };

    raw_session->table->on_selection_changed = [refresh_selection](ckv::widgets::TableCellRef) {
        refresh_selection();
    };
    raw_session->table->on_sort_requested = [raw_session, refresh_selection](std::optional<ckv::widgets::TableCellRef>,
                                                                             std::optional<std::size_t>,
                                                                             bool) {
        if (raw_session->table != nullptr)
            raw_session->table->model_changed();
        refresh_selection();
    };

    window->accept_request = [this, weak_self, raw_session, selected_path_of] {
        if (weak_self.expired())
            return;
        last_active_results_ = raw_session;
        const std::string path = selected_path_of();
        if (!path.empty())
            viewers_->open_preview(path, FindPreviewMode::Auto);
    };

    auto bar = std::make_unique<ckv::ui::Row>();
    bar->set_spacing(1);

    auto make_btn = [&](std::string caption, std::function<void()> fn) {
        auto btn = std::make_unique<ckv::widgets::Button>(std::move(caption));
        btn->on_press = [this, weak_self, raw_session, action_fn = std::move(fn)] {
            if (weak_self.expired())
                return;
            last_active_results_ = raw_session;
            action_fn();
        };
        bar->add_item(std::move(btn));
    };

    make_btn("&View (F3)", [this, selected_path_of] { viewers_->open_preview(selected_path_of(), FindPreviewMode::Auto); });
    make_btn("&Edit Text (F4)", [this, selected_path_of] { viewers_->open_text_editor(selected_path_of()); });
    make_btn("&JSON Tree", [this, selected_path_of] { viewers_->open_preview(selected_path_of(), FindPreviewMode::Json); });
    make_btn("&Hex Edit (F8)", [this, selected_path_of] { viewers_->open_hex_editor(selected_path_of()); });
    make_btn("Export &List...", [this, raw_session] { show_export_dialog(raw_session); });
    make_btn("&New Search", [this] { open_quick_search_window(); });

    root_col->add_item(std::move(bar));
    window->set_content(std::move(root_col));
    window->set_content_margins(1, 0, 1, 0);

    window->on_closed = [this, weak_self, raw_session] {
        if (weak_self.expired())
            return;
        if (raw_session->table != nullptr)
        {
            raw_session->table->clear_model();
            raw_session->table = nullptr;
        }
        ckv::widgets::Window *const closing = raw_session->window;
        raw_session->window = nullptr;
        if (last_active_results_ == raw_session)
            last_active_results_ = nullptr;
        auto it = std::find_if(results_sessions_.begin(), results_sessions_.end(),
                               [raw_session](const auto &item) { return item.get() == raw_session; });
        std::unique_ptr<FoundFilesSession> removed;
        if (it != results_sessions_.end())
        {
            removed = std::move(*it);
            results_sessions_.erase(it);
        }
        if (last_active_results_ == nullptr && !results_sessions_.empty())
            last_active_results_ = results_sessions_.back().get();
        if (closing != nullptr)
            shell_->desktop().remove_window(closing);
    };

    auto *const focus_target = raw_session->table;
    last_active_results_ = raw_session;
    results_sessions_.push_back(std::move(session));
    shell_->desktop().present_modeless(ckv::widgets::WindowHandle{std::move(window), focus_target}, application_);
    refresh_selection();
}

void FindApp::create_bulk_action_report_window(const ck::find::SearchSpecification &executed_spec,
                                               const ck::find::SearchExecutionResult &result)
{
    std::ostringstream output;
    if (result.cancelled)
        output << "Action cancelled after " << result.matchCount << " match(es).\n";
    else if (result.exitCode == 0)
        output << "Action completed across " << result.matchCount << " match(es).\n";
    else
        output << "Action completed with exit code " << result.exitCode << " across " << result.matchCount << " match(es).\n";

    if (result.deletedCount != 0 || result.failedDeletionCount != 0)
    {
        output << "Deleted " << result.deletedCount << " matching file(s) or symbolic link(s).\n";
        if (result.failedDeletionCount != 0)
            output << result.failedDeletionCount << " matching file(s) could not be deleted.\n";
    }

    if (result.customCommandInvocationCount != 0 || !result.customCommandAudit.empty())
    {
        output << result.customCommandInvocationCount << " sandboxed custom command invocation(s).\n";
        if (result.failedCustomCommandInvocationCount != 0)
            output << result.failedCustomCommandInvocationCount << " invocation(s) failed or hit limits.\n";
        if (result.customCommandCancelled)
            output << "Custom command cancellation was requested.\n";
        if (result.customCommandTimedOut)
            output << "At least one custom command exceeded its 5-second limit.\n";
        if (result.customCommandOutputTruncated)
            output << "Output exceeded bounded capture limits and was truncated.\n";
        if (!result.customCommandAudit.empty())
            output << "Audit: " << result.customCommandAudit << '\n';
        if (!result.customCommandOutput.empty())
            output << "\nCommand output:\n" << result.customCommandOutput << '\n';
    }

    if (!result.matches.empty())
    {
        output << "\nMatched paths:\n";
        for (const auto &match : result.matches)
            output << "  " << match.string() << '\n';
    }

    const std::string root = ck::find::bufferToString(executed_spec.startLocation);
    auto window = std::make_unique<ckv::widgets::Window>("Bulk Action Report (" + (root.empty() ? std::string(".") : root) + ")");
    auto *const win_ptr = window.get();
    const ckv::Rect area = shell_->desktop().content_area();
    const int w = std::min(88, area.width);
    const int h = std::min(22, area.height);
    window->set_bounds({area.x + (area.width - w) / 2, area.y + (area.height - h) / 2, w, h});
    window->set_min_size({48, 10});
    window->cancel_request = [win_ptr] { win_ptr->close(); };

    auto col = std::make_unique<ckv::ui::Column>();
    col->set_spacing(1);
    auto tv = std::make_unique<ckv::widgets::TextView>();
    auto *const tv_ptr = tv.get();
    tv->set_wrap_mode(ckv::widgets::WrapMode::None);
    tv->set_text(output.str());
    col->add_item(std::move(tv), {.policy = ckv::ui::SizePolicy::Expanding});

    auto bar = std::make_unique<ckv::ui::Row>();
    auto close_btn = std::make_unique<ckv::widgets::Button>("&Close");
    close_btn->on_press = [win_ptr] { win_ptr->close(); };
    bar->add_item(std::move(close_btn));
    col->add_item(std::move(bar));

    window->set_content(std::move(col));
    window->set_content_margins(1, 0, 1, 0);
    shell_->desktop().present_modeless(ckv::widgets::WindowHandle{std::move(window), tv_ptr}, application_);
}

FindApp::FoundFilesSession *FindApp::active_results_session() const noexcept
{
    if (ckv::widgets::Window *const active = shell_->desktop().active_window())
    {
        for (const auto &session : results_sessions_)
        {
            if (session->window == active)
            {
                last_active_results_ = session.get();
                return session.get();
            }
        }
    }
    if (last_active_results_ != nullptr)
    {
        for (const auto &session : results_sessions_)
        {
            if (session.get() == last_active_results_)
                return last_active_results_;
        }
        last_active_results_ = nullptr;
    }
    return results_sessions_.empty() ? nullptr : results_sessions_.back().get();
}

bool FindApp::write_results_export(const FoundFilesSession &session, const std::string &target_path)
{
    if (target_path.empty())
        return false;
    std::ostringstream lines;
    for (std::size_t idx : session.model.order)
    {
        if (idx < session.model.records.size())
            lines << session.model.records[idx].full_path << '\n';
    }
    const auto written = files_.write_file_atomic(target_path, lines.str(), ckv::FileWriteExpectation::any());
    if (written.status != ckv::FileWriteStatus::Ok)
        return false;
    if (session.window != nullptr)
    {
        session.window->set_footer("Exported " + std::to_string(session.model.order.size()) +
                                   " path(s) to '" + target_path + "'");
    }
    return true;
}

bool FindApp::export_active_results(const std::string &target_path)
{
    const FoundFilesSession *session = active_results_session();
    if (session == nullptr)
        return false;
    return write_results_export(*session, target_path);
}

void FindApp::show_export_dialog(FoundFilesSession *target_session)
{
    FoundFilesSession *const session = target_session != nullptr ? target_session : active_results_session();
    if (session == nullptr)
    {
        show_message(ckv::widgets::MessageBoxKind::Info,
                     "Export found files",
                     "Run a search first so there is a Found Files window to export.");
        return;
    }

    std::string start_dir = ck::find::bufferToString(session->specification.startLocation);
    if (start_dir.empty() || !files_.is_directory(start_dir))
        start_dir = ".";

    ckv::widgets::FileDialogOptions opts;
    opts.filters = {{"Text files", {"txt"}}, {"All files", {}}};
    opts.suggested_name = "found-files-" + std::to_string(session->number) + ".txt";
    opts.recent_locations_key = "ck.find.export_dirs";

    const int session_number = session->number;
    const std::weak_ptr<void> weak_self = lifetime_;
    file_dialog_.reset();
    file_dialog_.emplace(ckv::widgets::present_modal_file_dialog(
        ckv::widgets::FileDialogMode::Save, start_dir, files_, std::move(opts), application_, shell_->desktop()));
    file_dialog_->set_completion_handler([this, weak_self, session_number](ckv::widgets::FileDialogResult res) {
        if (weak_self.expired() || !res.accepted || res.path.empty())
            return;
        FoundFilesSession *found_session = nullptr;
        for (const auto &candidate : results_sessions_)
        {
            if (candidate->number == session_number)
            {
                found_session = candidate.get();
                break;
            }
        }
        if (found_session == nullptr)
            return;
        if (!write_results_export(*found_session, res.path))
        {
            show_message(ckv::widgets::MessageBoxKind::Error,
                         "Export failed",
                         "Could not write found-files list to '" + res.path + "'.");
            return;
        }
        show_message(ckv::widgets::MessageBoxKind::Info,
                     "Export complete",
                     "Exported " + std::to_string(found_session->model.order.size()) +
                         " file path(s) to '" + res.path + "'.");
    });
}

std::optional<std::string> FindApp::selected_result_path() const
{
    const FoundFilesSession *session = active_results_session();
    if (session == nullptr || session->table == nullptr)
        return std::nullopt;
    const auto cell = session->table->selected_cell();
    if (!cell)
        return std::nullopt;
    const ResultRowRecord *rec = session->model.record_by_id(cell->row);
    if (rec == nullptr)
        return std::nullopt;
    return rec->full_path;
}

void FindApp::view_selected_result(FindPreviewMode mode)
{
    const auto path = selected_result_path();
    if (!path)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "View file", "Select a file in a Found Files window first.");
        return;
    }
    viewers_->open_preview(*path, mode);
}

void FindApp::edit_selected_result_text()
{
    const auto path = selected_result_path();
    if (!path)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Edit file", "Select a file in a Found Files window first.");
        return;
    }
    viewers_->open_text_editor(*path);
}

void FindApp::edit_selected_result_hex()
{
    const auto path = selected_result_path();
    if (!path)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Hex editor", "Select a file in a Found Files window first.");
        return;
    }
    viewers_->open_hex_editor(*path);
}

void FindApp::copy_selected_result_path()
{
    const auto path = selected_result_path();
    if (!path)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Copy path", "Select a file in a Found Files window first.");
        return;
    }
    application_.set_clipboard_text(*path);
    if (FoundFilesSession *session = active_results_session(); session != nullptr && session->window != nullptr)
        session->window->set_footer("Copied path to clipboard: " + *path);
}

void FindApp::delete_selected_result()
{
    FoundFilesSession *const session = active_results_session();
    if (session == nullptr || session->table == nullptr)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Delete file", "Select a file in a Found Files window first.");
        return;
    }
    const auto cell = session->table->selected_cell();
    if (!cell)
        return;
    const ResultRowRecord *rec = session->model.record_by_id(cell->row);
    if (rec == nullptr)
        return;
    if (rec->is_directory)
    {
        show_message(ckv::widgets::MessageBoxKind::Warning,
                     "Delete file",
                     "Directories are never deleted from Found Files results.");
        return;
    }

    const int session_number = session->number;
    const ckv::widgets::TableRowId row_id = rec->id;
    const std::string path = rec->full_path;
    const std::weak_ptr<void> weak_self = lifetime_;

    destructive_confirmation_.reset();
    destructive_confirmation_.emplace(ckv::widgets::present_modal_message_box(
        application_, shell_->desktop(),
        {ckv::widgets::MessageBoxKind::Warning,
         "Delete file",
         "Permanently delete '" + path + "'? This cannot be undone.",
         ckv::widgets::MessageBoxButtons::YesNoCancel}));
    destructive_confirmation_->set_completion_handler([this, weak_self, session_number, row_id, path](ckv::widgets::MessageBoxResult choice) {
        if (weak_self.expired() || choice != ckv::widgets::MessageBoxResult::Yes)
            return;
        if (!execution_service_.delete_single_file(path))
        {
            show_message(ckv::widgets::MessageBoxKind::Error, "Delete failed", "Could not delete '" + path + "'.");
            return;
        }
        for (const auto &candidate : results_sessions_)
        {
            if (candidate->number == session_number && candidate->table != nullptr)
            {
                candidate->model.erase_by_id(row_id);
                candidate->table->model_changed();
                if (candidate->window != nullptr)
                    candidate->window->set_footer("Deleted '" + path + "'");
                break;
            }
        }
    });
}

void FindApp::show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message)
{
    message_box_.reset();
    message_box_.emplace(ckv::widgets::present_modal_message_box(
        application_, shell_->desktop(),
        {kind, std::move(title), std::move(message), ckv::widgets::MessageBoxButtons::Ok}));
    message_box_->set_completion_handler([](ckv::widgets::MessageBoxResult) {});
}

bool FindApp::execution_running() const noexcept
{
    return execution_service_.running();
}

bool FindApp::progress_dialog_open() const noexcept
{
    return progress_dialog_ != nullptr && progress_dialog_->window != nullptr;
}

std::size_t FindApp::results_window_count() const noexcept
{
    return results_sessions_.size();
}

} // namespace ck::vision
