#include <string>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>

#include "chat_app.hpp"

int main()
{
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::ChatApp chat(application, [](const std::string &prompt) {
        return "No model service is configured yet. Received: " + prompt;
    });
    application.run();
    return 0;
}
