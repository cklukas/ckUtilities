#include "find_app.hpp"

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
    ck::vision::FindApp find(application);

    require(find.command_preview().find("find") == 0, "The preview must be built by the search backend.");
    require(application.execute_command(find.preview_command()), "Preview must be a registry command.");
    require(application.execute_command(find.new_search_command()), "New search must be a registry command.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "Find must render headlessly.");
}
