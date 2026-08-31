#include "ck/vision/suite_shell.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/status_line.hpp>

namespace ck::vision
{
namespace
{

using ckv::ui::CommandDescriptor;
using ckv::ui::CommandId;
using ckv::ui::CommandVisibility;
using ckv::widgets::CommandPresentation;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;

ckv::widgets::TimeValue local_time()
{
    // Documentation capture drives real applications through HeadlessTerminal.
    // Keep its visual clock stable so a CI regeneration can verify the tracked
    // SVGs byte-for-byte without changing normal interactive behavior.
    if (const char *capture_mode = std::getenv("CKTOOLS_DOCUMENTATION_CAPTURE");
        capture_mode != nullptr && std::string_view(capture_mode) == "1")
        return {8, 0, 0};

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    return {local.tm_hour, local.tm_min, local.tm_sec};
}

} // namespace

SuiteCommandScope::SuiteCommandScope(ckv::ui::CommandRegistry &registry) noexcept : registry_(&registry) {}

SuiteCommandScope::~SuiteCommandScope()
{
    reset();
}

ckv::ui::CommandId SuiteCommandScope::own(ckv::ui::CommandId id)
{
    if (id == ckv::ui::kInvalidCommand)
        return id;

    try
    {
        commands_.push_back(id);
    }
    catch (...)
    {
        registry_->withdraw(id);
        throw;
    }
    return id;
}

void SuiteCommandScope::reset() noexcept
{
    if (registry_ == nullptr)
        return;

    for (const ckv::ui::CommandId id : commands_)
        registry_->withdraw(id);
    commands_.clear();
}

SuiteShell::SuiteShell(ckv::ui::Application &application, SuiteShellOptions options)
    : application_(application),
      options_(std::move(options)),
      roles_(ckv::ui::intern_standard_roles(application_.roles())),
      about_command_(application_.commands().declare(CommandDescriptor{
          .key = std::string(kAboutCommandKey),
          .title = "&About...",
          .category = "Application",
          .visibility = CommandVisibility::Palette,
          .handler = [this] { show_about(); },
      })),
      return_to_launcher_command_(options_.on_return_to_launcher
                                      ? application_.commands().declare(CommandDescriptor{
                                            .key = std::string(kReturnToLauncherCommandKey),
                                            .title = "Return to &Launcher",
                                            .category = "Application",
                                            .visibility = CommandVisibility::Palette,
                                            .handler = options_.on_return_to_launcher,
                                        })
                                      : ckv::ui::kInvalidCommand),
      shell_(application_, make_shell_options())
{
    ckv::widgets::install_about_help(application_, desktop(), roles_, options_.application_name,
                                     options_.about_text);
    auto clock = std::make_unique<ckv::widgets::ClockView>();
    clock->set_time_provider(options_.clock_time_provider ? options_.clock_time_provider : local_time);
    clock->set_blinking_separator(true);
    clock_view_ = shell_.menu_bar()->set_trailing_view(std::move(clock));
}

SuiteShell::~SuiteShell()
{
    clock_view_ = nullptr;
    application_.commands().withdraw(about_command_);
    if (return_to_launcher_command_ != ckv::ui::kInvalidCommand)
        application_.commands().withdraw(return_to_launcher_command_);
    // ApplicationShell attaches its desktop to Application::root().  ckVision
    // 0.1.2 intentionally leaves ownership with that root, so release the
    // child through the public view API before this helper goes away.  This
    // keeps sequential CK Utilities presentations from stacking desktops and
    // clears any focus held by the application while the desktop is detached.
    application_.root().remove_child(&shell_.desktop());
}

ckv::widgets::Desktop &SuiteShell::desktop() noexcept
{
    return shell_.desktop();
}

ckv::ui::Theme SuiteShell::make_theme(ckv::ui::RoleRegistry &registry,
                                      const ckv::ui::StandardRoles &roles,
                                      ThemeScheme scheme)
{
    switch (scheme)
    {
    case ThemeScheme::Classic:
        return ckv::ui::make_classic_theme(registry, roles);
    case ThemeScheme::Dark:
        return ckv::ui::make_dark_theme(registry, roles);
    case ThemeScheme::Light:
        return ckv::ui::make_light_theme(registry, roles);
    case ThemeScheme::Mono:
        return ckv::ui::make_mono_theme(registry, roles);
    case ThemeScheme::HighContrast:
        return ckv::ui::make_high_contrast_theme(registry, roles);
    }

    return ckv::ui::make_classic_theme(registry, roles);
}

ckv::widgets::ApplicationShellOptions SuiteShell::make_shell_options() const
{
    std::vector<MenuItem> file_items;
    if (return_to_launcher_command_ != ckv::ui::kInvalidCommand)
        file_items.push_back(MenuItem::command(
            CommandPresentation{return_to_launcher_command_, "Return to &Launcher"}));

    if (!file_items.empty())
        file_items.push_back(MenuItem::separator());

    file_items.push_back(MenuItem::command(
        CommandPresentation{application_.commands().standard().quit, "E&xit"}));

    std::vector<MenuBarItem> menus;
    menus.emplace_back(MenuBarItem{"&File", std::move(file_items)});
    for (const auto &menu : options_.application_menus)
        menus.push_back(menu);
    menus.emplace_back(MenuBarItem{
        "&Help", {MenuItem::command(CommandPresentation{about_command_, "&About..."})}});

    std::vector<StatusLineItem> status_items = options_.application_status_items;
    if (return_to_launcher_command_ != ckv::ui::kInvalidCommand)
        status_items.emplace_back(CommandPresentation{return_to_launcher_command_, "&Launcher"}, 20);

    status_items.emplace_back(CommandPresentation{application_.commands().standard().help}, 10);
    status_items.emplace_back(CommandPresentation{application_.commands().standard().menu}, 5);
    status_items.emplace_back(CommandPresentation{application_.commands().standard().quit, "&Quit"}, 1);

    return {
        .theme = make_theme(application_.roles(), roles_, options_.theme),
        .menus = std::move(menus),
        .status_items = std::move(status_items),
    };
}

void SuiteShell::show_about()
{
    auto presentation = ckv::widgets::present_message_box(
        application_, desktop(), roles_,
        {ckv::widgets::MessageBoxKind::Info, options_.application_name, options_.about_text,
         ckv::widgets::MessageBoxButtons::Ok});
    presentation.set_completion_handler([](ckv::widgets::MessageBoxResult) {});
}

} // namespace ck::vision
