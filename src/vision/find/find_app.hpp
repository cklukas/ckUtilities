// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/core/filesystem.hpp>
#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/file_dialog.hpp>
#include <cvision/widgets/label.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/progress.hpp>
#include <cvision/widgets/progress_tasks.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/find/search_model.hpp"
#include "ck/vision/suite_shell.hpp"
#include "find_services.hpp"
#include "find_viewers.hpp"

namespace ck::vision
{

class FindApp
{
public:
    FindApp(ckv::ui::Application &application,
            FindSpecificationStore &specification_store,
            FindExecutionService &execution_service);

    FindApp(ckv::ui::Application &application,
            FindSpecificationStore &specification_store,
            FindExecutionService &execution_service,
            ckv::FileSystem &files,
            std::string_view initial_directory = ".",
            std::string_view initial_pattern = "");

    ~FindApp();

    const ck::find::SearchSpecification &specification() const noexcept { return specification_; }
    void set_specification(ck::find::SearchSpecification specification);
    std::string command_preview() const;

    ckv::ui::CommandId new_search_command() const noexcept { return new_search_command_; }
    ckv::ui::CommandId preview_command() const noexcept { return preview_command_; }
    ckv::ui::CommandId save_command() const noexcept { return save_command_; }
    ckv::ui::CommandId load_command() const noexcept { return load_command_; }
    ckv::ui::CommandId export_command() const noexcept { return export_command_; }
    ckv::ui::CommandId execute_command() const noexcept { return execute_command_; }
    ckv::ui::CommandId cancel_command() const noexcept { return cancel_command_; }
    ckv::ui::CommandId advanced_filters_command() const noexcept { return advanced_filters_command_; }
    ckv::ui::CommandId presets_command() const noexcept { return presets_command_; }
    ckv::ui::CommandId view_file_command() const noexcept { return view_file_command_; }
    ckv::ui::CommandId view_text_command() const noexcept { return view_text_command_; }
    ckv::ui::CommandId view_json_command() const noexcept { return view_json_command_; }
    ckv::ui::CommandId view_hex_command() const noexcept { return view_hex_command_; }
    ckv::ui::CommandId edit_text_command() const noexcept { return edit_text_command_; }
    ckv::ui::CommandId edit_hex_command() const noexcept { return edit_hex_command_; }
    ckv::ui::CommandId copy_path_command() const noexcept { return copy_path_command_; }
    ckv::ui::CommandId delete_selected_command() const noexcept { return delete_selected_command_; }
    ckv::ui::CommandId bulk_action_command() const noexcept { return bulk_action_command_; }

    bool execution_running() const noexcept;
    bool progress_dialog_open() const noexcept;
    std::size_t results_window_count() const noexcept;
    std::optional<std::string> selected_result_path() const;
    bool export_active_results(const std::string &target_path);
    FindWorkspaceViewers &viewers() noexcept { return *viewers_; }
    const FindWorkspaceViewers &viewers() const noexcept { return *viewers_; }

    const std::optional<ck::find::SearchExecutionResult> &last_execution_result() const noexcept
    {
        return last_execution_result_;
    }

private:
    FindApp(ckv::ui::Application &application,
            FindSpecificationStore &specification_store,
            FindExecutionService &execution_service,
            std::unique_ptr<ckv::FileSystem> owned_files,
            ckv::FileSystem *external_files,
            std::string_view initial_directory,
            std::string_view initial_pattern);

    struct QuickSearchWindowState;
    struct ProgressDialogState;
    struct FoundFilesSession;

    void declare_commands();
    SuiteShellOptions make_shell_options() const;

    void open_quick_search_window();
    void sync_quick_search_from_specification();
    void apply_quick_search_fields_and_run();
    void browse_start_directory();

    void show_advanced_filters_dialog();
    void show_presets_dialog();
    void show_save_dialog();
    void show_load_dialog();
    void show_export_dialog(FoundFilesSession *target_session = nullptr);
    void show_posix_command_dialog();
    void show_bulk_action_dialog();

    void handle_save_command();
    void request_execution();
    void start_execution(bool delete_matched_files);
    void cancel_execution();
    void open_progress_dialog(const ck::find::SearchSpecification &spec, bool delete_matched_files);
    void close_progress_dialog();
    void update_progress_dialog(const ck::find::SearchProgressSnapshot &snapshot);
    void complete_execution(ck::find::SearchSpecification executed_spec,
                            ck::find::SearchExecutionResult result);

    void create_found_files_window(const ck::find::SearchSpecification &executed_spec,
                                   ck::find::SearchExecutionResult result);
    void create_bulk_action_report_window(const ck::find::SearchSpecification &executed_spec,
                                          const ck::find::SearchExecutionResult &result);
    FoundFilesSession *active_results_session() const noexcept;
    bool write_results_export(const FoundFilesSession &session, const std::string &target_path);

    void view_selected_result(FindPreviewMode mode);
    void edit_selected_result_text();
    void edit_selected_result_hex();
    void copy_selected_result_path();
    void delete_selected_result();

    void show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message);

    ckv::ui::Application &application_;
    FindSpecificationStore &specification_store_;
    FindExecutionService &execution_service_;
    std::unique_ptr<ckv::FileSystem> owned_files_;
    ckv::FileSystem &files_;
    ck::find::SearchSpecification specification_;
    std::string quick_pattern_text_;
    std::string quick_content_text_;

    ckv::ui::CommandId new_search_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId preview_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId save_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId load_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId export_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId execute_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId cancel_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId advanced_filters_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId presets_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId view_file_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId view_text_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId view_json_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId view_hex_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId edit_text_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId edit_hex_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId copy_path_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId delete_selected_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId bulk_action_command_ = ckv::ui::kInvalidCommand;

    std::unique_ptr<SuiteShell> shell_;
    std::unique_ptr<FindWorkspaceViewers> viewers_;
    std::unique_ptr<QuickSearchWindowState> quick_search_;
    std::unique_ptr<ProgressDialogState> progress_dialog_;
    std::vector<std::unique_ptr<FoundFilesSession>> results_sessions_;
    mutable FoundFilesSession *last_active_results_ = nullptr;
    int next_results_number_ = 1;

    std::optional<ckv::widgets::DescriptorDialogPresentation> modal_dialog_;
    std::optional<ckv::widgets::FileDialogPresentation> file_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> destructive_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> custom_command_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> message_box_;

    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::uint64_t active_search_generation_ = 0;
    std::optional<ck::find::SearchExecutionResult> last_execution_result_;
    SuiteCommandScope command_scope_;
};

} // namespace ck::vision
