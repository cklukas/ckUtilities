#include "ck/vision/suite_shell.hpp"

#include <cstdlib>
#include <iostream>

#include <cvision/core/clock.hpp>
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

} // namespace

int main()
{
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
