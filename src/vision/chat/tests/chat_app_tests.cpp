#include "chat_app.hpp"

#include <cstdlib>
#include <iostream>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

namespace
{
void require(bool value, const char *message)
{
    if (value)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}
}

int main()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ck::vision::ChatApp chat(application, [](const std::string &prompt) { return "Echo: " + prompt; });
    require(chat.submit_prompt("Hello"), "The native chat app must accept a non-empty prompt.");
    require(chat.messages().size() == 2 && chat.messages()[1].content == "Echo: Hello",
            "The native chat app must retain user and injected assistant messages.");
    require(application.execute_command(chat.copy_command()), "Copy must dispatch through the command registry.");
    require(application.clipboard_text().find("Echo: Hello") != std::string::npos,
            "Copy must export the native transcript through the application clipboard.");
    require(application.execute_command(chat.new_chat_command()), "New conversation must dispatch through the command registry.");
    require(chat.messages().empty(), "New conversation must clear the application-owned conversation state.");
    require(application.execute_command(chat.send_command()), "Send must dispatch through the command registry.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "The native chat app must render headlessly.");
}
