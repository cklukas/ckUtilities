#include "config_app.hpp"

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
    ck::config::OptionRegistry registry("test");
    registry.registerOption({"enabled", ck::config::OptionKind::Boolean, ck::config::OptionValue(false), "Enabled", "Enables the test feature."});
    registry.registerOption({"limit", ck::config::OptionKind::Integer, ck::config::OptionValue(std::int64_t{7}), "Limit", "Sets the test limit."});
    registry.set("enabled", ck::config::OptionValue(true));

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ck::vision::ConfigApp config(application, registry);
    require(config.option_count() == 2 && config.table() != nullptr,
            "The native config app must present injected registry definitions in a table.");
    require(config.select_option("enabled"), "The native config app must select options by stable string key.");
    require(application.execute_command(config.reset_command()), "Reset must dispatch through the command registry.");
    require(!registry.getBool("enabled"), "Reset must restore the selected option's registered default.");
    require(application.execute_command(config.edit_command()), "Edit must dispatch through the command registry.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "The native config view must render headlessly.");
}
