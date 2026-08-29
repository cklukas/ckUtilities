#include <cvision/core/clock.hpp>
#include <cvision/core/version.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

int main()
{
    const ckv::Version version = ckv::version();
    if (version.major < 0 || version.minor < 0 || version.patch < 0)
        return 1;

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{80, 24});
    ckv::ui::Application application(terminal, clock);

    // A minimal UI loop step proves that the package's UI, terminal, clock,
    // and presenter surfaces link and execute together in an external client.
    application.step(clock.now_nanos());
    return application.terminal_too_small() ? 1 : 0;
}
