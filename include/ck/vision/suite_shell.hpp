#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/ui/standard_roles.hpp>
#include <cvision/widgets/application_shell.hpp>
#include <cvision/widgets/common_components.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/status_line.hpp>

namespace ck::vision
{

inline constexpr std::string_view kAboutCommandKey = "ck.utilities.about";
inline constexpr std::string_view kReturnToLauncherCommandKey = "ck.utilities.return_to_launcher";

enum class ThemeScheme
{
    Classic,
    Dark,
    Light,
    Mono,
    HighContrast,
};

struct SuiteShellOptions
{
    std::string application_name;
    std::string about_text;
    ThemeScheme theme = ThemeScheme::Classic;
    // An application host may inject local wall time for the native menu
    // clock. Tests supply a deterministic value; a production shell uses the
    // platform-local default when this is empty.
    std::function<ckv::widgets::TimeValue()> clock_time_provider;
    std::function<void()> on_return_to_launcher;
    std::vector<ckv::widgets::MenuBarItem> application_menus;
    std::vector<ckv::widgets::StatusLineItem> application_status_items;
};

// Owns command registrations whose handlers close over a native controller.
// The application outlives controllers, so leaving a registration behind
// would leave the command registry with a callable dangling `this` pointer.
// Keep this scope as the final member of a controller so it withdraws those
// handlers before the controller's shell and presentation state are torn down.
class SuiteCommandScope
{
public:
    explicit SuiteCommandScope(ckv::ui::CommandRegistry &registry) noexcept;
    ~SuiteCommandScope();

    SuiteCommandScope(const SuiteCommandScope &) = delete;
    SuiteCommandScope &operator=(const SuiteCommandScope &) = delete;

    ckv::ui::CommandId own(ckv::ui::CommandId id);
    void reset() noexcept;

private:
    ckv::ui::CommandRegistry *registry_ = nullptr;
    std::vector<ckv::ui::CommandId> commands_;
};

class SuiteShell
{
public:
    SuiteShell(ckv::ui::Application &application, SuiteShellOptions options);
    ~SuiteShell();

    ckv::widgets::Desktop &desktop() noexcept;
    const ckv::ui::StandardRoles &roles() const noexcept { return roles_; }
    const ckv::widgets::ClockView *clock_view() const noexcept { return clock_view_; }

    ckv::ui::CommandId about_command() const noexcept { return about_command_; }
    ckv::ui::CommandId return_to_launcher_command() const noexcept
    {
        return return_to_launcher_command_;
    }

private:
    static ckv::ui::Theme make_theme(ckv::ui::RoleRegistry &registry,
                                     const ckv::ui::StandardRoles &roles,
                                     ThemeScheme scheme);

    ckv::widgets::ApplicationShellOptions make_shell_options() const;
    void show_about();

    ckv::ui::Application &application_;
    SuiteShellOptions options_;
    ckv::ui::StandardRoles roles_;
    ckv::ui::CommandId about_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId return_to_launcher_command_ = ckv::ui::kInvalidCommand;
    ckv::widgets::ApplicationShell shell_;
    ckv::widgets::ClockView *clock_view_ = nullptr;
};

} // namespace ck::vision
