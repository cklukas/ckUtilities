#include "launcher_app.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include <cvision/widgets/common_components.hpp>
#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/splitter.hpp>
#include <cvision/widgets/text_layout.hpp>

namespace ck::vision
{
namespace
{

using ckv::ui::CommandDescriptor;
using ckv::ui::CommandVisibility;
using ckv::widgets::CommandPresentation;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;

} // namespace

UtilitiesLauncherApp::UtilitiesLauncherApp(ckv::ui::Application &application, LaunchHandler on_launch)
    : application_(application), on_launch_(std::move(on_launch))
{
    for (const ck::appinfo::ToolInfo &tool : ck::appinfo::tools())
    {
        if (tool.id != "ck-utilities")
            tools_.push_back(&tool);
    }
    std::sort(tools_.begin(), tools_.end(), [](const auto *left, const auto *right) {
        return left->displayName < right->displayName;
    });

    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    open_launcher_window();
}

void UtilitiesLauncherApp::declare_commands()
{
    launch_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.utilities.launch_tool",
        .title = "&Launch selected tool",
        .category = "CK Utilities",
        .chord = "Enter",
        .visibility = CommandVisibility::Palette,
        .handler = [this] { launch_active_tool(); },
    });
    new_launcher_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.utilities.new_launcher",
        .title = "&New launcher window",
        .category = "CK Utilities",
        .chord = "Ctrl+N",
        .visibility = CommandVisibility::Palette,
        .handler = [this] { open_launcher_window(); },
    });
    calendar_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.utilities.show_calendar",
        .title = "Show &Calendar",
        .category = "CK Utilities",
        .visibility = CommandVisibility::Palette,
        .handler = [this] { open_calendar_window(); },
    });
}

SuiteShellOptions UtilitiesLauncherApp::make_shell_options() const
{
    return {
        .application_name = "CK Utilities",
        .about_text = "The native ckVision suite launcher. Browse installed CK tools, inspect their descriptions, and launch the selected tool in a fresh terminal session.",
        .application_menus = {
            MenuBarItem{"&Tools", {
                MenuItem::command(CommandPresentation{launch_command_, "&Launch selected tool"}),
                MenuItem::command(CommandPresentation{calendar_command_, "Show &Calendar"}),
            }},
            MenuBarItem{"&Window", {MenuItem::command(CommandPresentation{new_launcher_command_, "&New launcher window"})}},
        },
        .application_status_items = {
            StatusLineItem{CommandPresentation{launch_command_, "&Launch"}, 30},
            StatusLineItem{CommandPresentation{calendar_command_, "&Calendar"}, 25},
            StatusLineItem{CommandPresentation{new_launcher_command_, "&New"}, 20},
        },
    };
}

void UtilitiesLauncherApp::open_launcher_window()
{
    auto state = std::make_unique<LauncherWindow>();
    LauncherWindow *const raw_state = state.get();

    auto window = std::make_unique<ckv::widgets::Window>("CK Utilities");
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{60, 16});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    window->on_closed = [this, raw_state] { close_launcher_window(raw_state); };

    auto tool_list = std::make_unique<ckv::widgets::ListView>();
    state->tool_list = tool_list.get();
    std::vector<std::string> tool_names;
    tool_names.reserve(tools_.size());
    for (const ck::appinfo::ToolInfo *tool : tools_)
        tool_names.emplace_back(tool->displayName);
    state->tool_list->set_items(std::move(tool_names));
    state->tool_list->on_cursor_changed = [this, raw_state](std::size_t index) {
        update_detail(*raw_state, index);
    };
    state->tool_list->on_activate = [this](std::size_t) { launch_active_tool(); };

    auto detail = std::make_unique<ckv::widgets::TextView>();
    state->detail = detail.get();
    state->detail->set_wrap_mode(ckv::widgets::WrapMode::Word);

    auto splitter = std::make_unique<ckv::widgets::Splitter>(window->content_rect(),
                                                               std::move(tool_list), std::move(detail));
    window->set_content(std::move(splitter));
    state->window = shell_->desktop().add_window(std::move(window));
    windows_.push_back(std::move(state));

    if (!tools_.empty())
    {
        raw_state->tool_list->set_cursor(0);
        application_.set_focus(raw_state->tool_list);
    }
}

void UtilitiesLauncherApp::update_detail(LauncherWindow &window, std::size_t tool_index)
{
    if (window.detail == nullptr || tool_index >= tools_.size())
        return;

    const ck::appinfo::ToolInfo &tool = *tools_[tool_index];
    window.detail->set_text(std::string(tool.displayName) + "\n\n" +
                            std::string(tool.shortDescription) + "\n\n" +
                            std::string(tool.longDescription));
    if (window.window != nullptr)
        window.window->set_footer(std::string(tool.executable));
}

void UtilitiesLauncherApp::launch_active_tool()
{
    const ck::appinfo::ToolInfo *tool = selected_tool();
    if (tool == nullptr)
        return;
    if (on_launch_)
        on_launch_(*tool);
    application_.request_quit();
}

void UtilitiesLauncherApp::open_calendar_window()
{
    const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
    const std::chrono::year_month_day date{today};
    const ckv::widgets::DateValue selected{
        static_cast<int>(date.year()), static_cast<int>(static_cast<unsigned>(date.month())),
        static_cast<int>(static_cast<unsigned>(date.day()))};

    auto window = std::make_unique<ckv::widgets::Window>("Calendar");
    window->set_min_size(ckv::Size{32, 12});
    auto calendar = std::make_unique<ckv::widgets::CalendarView>();
    calendar->set_selected(selected);
    calendar->set_today(selected);
    window->set_content(std::move(calendar));
    shell_->desktop().add_window(std::move(window));
}

void UtilitiesLauncherApp::close_launcher_window(LauncherWindow *window)
{
    const auto found = std::find_if(windows_.begin(), windows_.end(), [window](const auto &candidate) {
        return candidate.get() == window;
    });
    if (found == windows_.end())
        return;

    ckv::widgets::Window *const owned_window = (*found)->window;
    windows_.erase(found);
    if (owned_window != nullptr)
        shell_->desktop().remove_window(owned_window);
    if (windows_.empty())
        application_.request_quit();
}

UtilitiesLauncherApp::LauncherWindow *UtilitiesLauncherApp::active_launcher_window() const noexcept
{
    const ckv::widgets::Window *const active = shell_->desktop().active_window();
    const auto found = std::find_if(windows_.begin(), windows_.end(), [active](const auto &candidate) {
        return candidate->window == active;
    });
    if (found != windows_.end())
        return found->get();

    // A utility window may temporarily be active while a menu command still
    // refers to the selected launcher tool. Retain the most recently opened
    // launcher as that command's stable context rather than making Launch a
    // silent no-op solely because the reader checked the calendar first.
    return windows_.empty() ? nullptr : windows_.back().get();
}

const ck::appinfo::ToolInfo *UtilitiesLauncherApp::selected_tool() const noexcept
{
    const LauncherWindow *const active = active_launcher_window();
    if (active == nullptr || active->tool_list == nullptr)
        return nullptr;
    const int cursor = active->tool_list->cursor();
    if (cursor < 0 || cursor >= static_cast<int>(tools_.size()))
        return nullptr;
    return tools_[static_cast<std::size_t>(cursor)];
}

} // namespace ck::vision
