#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/ui/standard_roles.hpp>
#include <cvision/widgets/application_shell.hpp>

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
    std::function<void()> on_return_to_launcher;
};

class SuiteShell
{
public:
    SuiteShell(ckv::ui::Application &application, SuiteShellOptions options);

    ckv::widgets::Desktop &desktop() noexcept;

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
};

} // namespace ck::vision
