#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/list_view.hpp>
#include <cvision/widgets/text_view.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/app_info.hpp"
#include "ck/vision/suite_shell.hpp"

namespace ck::vision
{

class BoundedDiagnostics;

// Native ckVision launcher presentation. It publishes the selected tool to
// the composition root, which owns the platform-specific child-process
// lifecycle after the terminal UI has closed.
class UtilitiesLauncherApp
{
public:
    using LaunchHandler = std::function<void(const ck::appinfo::ToolInfo &)>;

    UtilitiesLauncherApp(ckv::ui::Application &application, LaunchHandler on_launch);

    const ck::appinfo::ToolInfo *selected_tool() const noexcept;
    std::size_t launcher_window_count() const noexcept { return windows_.size(); }

    ckv::ui::CommandId launch_command() const noexcept { return launch_command_; }
    ckv::ui::CommandId new_launcher_command() const noexcept { return new_launcher_command_; }
    ckv::ui::CommandId calendar_command() const noexcept { return calendar_command_; }
    ckv::ui::CommandId ascii_table_command() const noexcept { return ascii_table_command_; }
    ckv::ui::CommandId calculator_command() const noexcept { return calculator_command_; }
    ckv::ui::CommandId diagnostics_command() const noexcept { return diagnostics_command_; }
    std::size_t desktop_window_count() const noexcept { return shell_->desktop().windows().size(); }
    std::size_t diagnostics_entry_count() const noexcept;

private:
    struct LauncherWindow
    {
        ckv::widgets::Window *window = nullptr;
        ckv::widgets::ListView *tool_list = nullptr;
        ckv::widgets::TextView *detail = nullptr;
    };

    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void open_launcher_window();
    void update_detail(LauncherWindow &window, std::size_t tool_index);
    void launch_active_tool();
    void open_calendar_window();
    void open_ascii_table_window();
    void open_calculator_window();
    void open_diagnostics_window();
    void log_diagnostic(ckv::LogLevel level, std::string_view message) noexcept;
    void close_launcher_window(LauncherWindow *window);
    LauncherWindow *active_launcher_window() const noexcept;

    ckv::ui::Application &application_;
    LaunchHandler on_launch_;
    std::vector<const ck::appinfo::ToolInfo *> tools_;
    std::vector<std::unique_ptr<LauncherWindow>> windows_;
    ckv::ui::CommandId launch_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId new_launcher_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId calendar_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId ascii_table_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId calculator_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId diagnostics_command_ = ckv::ui::kInvalidCommand;
    std::shared_ptr<BoundedDiagnostics> diagnostics_;
    std::unique_ptr<SuiteShell> shell_;
};

} // namespace ck::vision
