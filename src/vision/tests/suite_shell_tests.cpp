#include "ck/vision/suite_shell.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

#include <cvision/core/clock.hpp>
#include <cvision/term/capabilities.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

namespace
{

void require(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

ckv::Style render_shell(
    ck::vision::ThemeScheme scheme,
    ckv::term::Capabilities capabilities = ckv::term::baseline_capabilities())
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{80, 24}, capabilities);
    ckv::ui::Application application(terminal, clock);
    ck::vision::SuiteShell shell(
        application,
        {
            .application_name = "ckUtilities shell theme test",
            .about_text = "A native ckVision shell for the ckUtilities migration.",
            .theme = scheme,
        });

    application.step(clock.now_nanos());
    require(!application.terminal_too_small(),
            "A themed 80x24 shell must remain above the hard floor.");
    require(application.current_frame().size() == ckv::Size{80, 24},
            "Every suite theme must compose a full 80x24 frame.");
    require(!terminal.written_bytes().empty(),
            "Every suite theme must produce terminal output.");
    return application.theme().resolve(shell.roles().desktop_background);
}

void verify_theme_and_degraded_color_fixtures()
{
    constexpr std::array schemes{
        ck::vision::ThemeScheme::Classic,
        ck::vision::ThemeScheme::Dark,
        ck::vision::ThemeScheme::Light,
        ck::vision::ThemeScheme::Mono,
        ck::vision::ThemeScheme::HighContrast,
    };

    std::array<ckv::Style, schemes.size()> desktop_styles{};
    for (std::size_t index = 0; index < schemes.size(); ++index)
        desktop_styles[index] = render_shell(schemes[index]);

    require(desktop_styles[1] != desktop_styles[2],
            "Dark and light suite themes must resolve different desktop styles.");
    require(desktop_styles[3] != desktop_styles[4],
            "Mono and high-contrast suite themes must resolve different desktop styles.");

    ckv::term::Capabilities degraded = ckv::term::baseline_capabilities();
    degraded.color_depth = ckv::term::ColorDepth::Mono16;
    const ckv::Style degraded_style =
        render_shell(ck::vision::ThemeScheme::Dark, degraded);
    require(degraded_style == desktop_styles[1],
            "Terminal color capability must not mutate the shell's semantic dark theme.");

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{80, 24}, degraded);
    ckv::ui::Application application(terminal, clock);
    ck::vision::SuiteShell shell(
        application,
        {
            .application_name = "ckUtilities degraded color test",
            .about_text = "A native ckVision shell for the ckUtilities migration.",
            .theme = ck::vision::ThemeScheme::Dark,
        });
    application.step(clock.now_nanos());
    require(terminal.written_bytes().find("38;2;") == std::string::npos &&
                terminal.written_bytes().find("48;2;") == std::string::npos,
            "A Mono16 terminal must not receive truecolor shell escape sequences.");
}

void verify_resize_and_recovery_fixture()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{80, 24});
    ckv::ui::Application application(terminal, clock);
    ck::vision::SuiteShell shell(
        application,
        {
            .application_name = "ckUtilities shell resize test",
            .about_text = "A native ckVision shell for the ckUtilities migration.",
            .theme = ck::vision::ThemeScheme::Dark,
        });

    application.step(clock.now_nanos());
    terminal.resize(ckv::Size{120, 40});
    application.step(clock.now_nanos());
    require(!application.terminal_too_small() &&
                application.current_frame().size() == ckv::Size{120, 40},
            "The shell must recompose at a wide terminal size.");

    terminal.resize(ckv::Size{42, 12});
    application.step(clock.now_nanos());
    require(!application.terminal_too_small() &&
                application.current_frame().size() == ckv::Size{42, 12},
            "The shell must recompose at a narrow usable terminal size.");

    terminal.clear_written();
    terminal.resize(ckv::Size{19, 5});
    application.step(clock.now_nanos());
    require(application.terminal_too_small(),
            "The shell must enter the standard too-small state below the hard floor.");
    require(terminal.written_bytes().find("Terminal too small") != std::string::npos,
            "The shell must present the standard too-small state rather than a stale frame.");

    terminal.clear_written();
    terminal.resize(ckv::Size{80, 24});
    application.step(clock.now_nanos());
    require(!application.terminal_too_small() &&
                application.current_frame().size() == ckv::Size{80, 24},
            "The shell must recover after growing back above the hard floor.");
    require(!terminal.written_bytes().empty(),
            "The recovered shell must present a normal frame.");
}

void verify_controller_command_scope_withdraws_handlers()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{80, 24});
    ckv::ui::Application application(terminal, clock);
    bool invoked = false;
    ckv::ui::CommandId command = ckv::ui::kInvalidCommand;
    {
        ck::vision::SuiteCommandScope scope(application.commands());
        command = scope.own(application.commands().declare({
            .key = "ck.vision.test.controller_command",
            .title = "Controller command",
            .handler = [&invoked] { invoked = true; },
        }));
        require(application.execute_command(command),
                "An owned suite command must remain dispatchable while its controller is alive.");
        require(invoked, "The owned suite command handler must run before its scope is destroyed.");
    }

    require(!application.execute_command(command),
            "Destroying a controller command scope must withdraw its callback from the application registry.");
    require(application.commands().key_for(command).empty(),
            "A withdrawn controller command must no longer expose a live command key.");
}

} // namespace

int main()
{
    verify_theme_and_degraded_color_fixtures();
    verify_resize_and_recovery_fixture();
    verify_controller_command_scope_withdraws_handlers();

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{80, 24});
    ckv::ui::Application application(terminal, clock);

    bool returned_to_launcher = false;
    ck::vision::SuiteShell shell(
        application,
        {
            .application_name = "ckUtilities shell test",
            .about_text = "A native ckVision shell for the ckUtilities migration.",
            .theme = ck::vision::ThemeScheme::Dark,
            .on_return_to_launcher = [&returned_to_launcher] { returned_to_launcher = true; },
        });

    require(application.commands().key_for(shell.about_command()) == ck::vision::kAboutCommandKey,
            "The shell must register its stable About command key.");
    require(application.commands().key_for(shell.return_to_launcher_command()) ==
                ck::vision::kReturnToLauncherCommandKey,
            "The shell must register its stable launcher command key.");
    require(application.execute_command(shell.return_to_launcher_command()),
            "The launcher command should dispatch through the command registry.");
    require(returned_to_launcher, "The launcher command handler was not invoked.");

    application.step(clock.now_nanos());
    require(!application.terminal_too_small(), "An 80x24 headless terminal must render the shell.");
    require(application.current_frame().size() == ckv::Size{80, 24},
            "The shell must compose a full terminal frame.");

    require(application.execute_command(application.commands().standard().quit),
            "Desktop should supply the standard quit command handler.");
    require(application.quit_requested(), "The standard quit command must request application shutdown.");
}
