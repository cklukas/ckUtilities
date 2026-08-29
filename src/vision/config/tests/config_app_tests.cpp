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

class MemoryPersistence final : public ck::vision::ConfigPersistence
{
public:
    bool load(ck::config::OptionRegistry &registry) override
    {
        ++load_count;
        registry.set("enabled", ck::config::OptionValue(true));
        return true;
    }

    bool save(const ck::config::OptionRegistry &registry) override
    {
        ++save_count;
        saved_enabled = registry.getBool("enabled");
        return true;
    }

    int load_count = 0;
    int save_count = 0;
    bool saved_enabled = false;
};
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
    MemoryPersistence persistence;
    ck::vision::ConfigApp config(application, registry, persistence);
    require(config.option_count() == 2 && config.table() != nullptr,
            "The native config app must present injected registry definitions in a table.");
    require(config.select_option("enabled"), "The native config app must select options by stable string key.");
    require(application.execute_command(config.reset_command()), "Reset must dispatch through the command registry.");
    require(!registry.getBool("enabled"), "Reset must restore the selected option's registered default.");
    registry.set("enabled", ck::config::OptionValue(true));
    require(application.execute_command(config.save_command()), "Save must dispatch through the command registry.");
    require(persistence.save_count == 1 && persistence.saved_enabled,
            "Save must delegate the current registry state to the injected persistence policy.");
    registry.set("enabled", ck::config::OptionValue(false));
    require(application.execute_command(config.reload_command()), "Reload must dispatch through the command registry.");
    require(persistence.load_count == 1 && registry.getBool("enabled"),
            "Reload must delegate registry updates to the injected persistence policy.");
    require(application.execute_command(config.edit_command()), "Edit must dispatch through the command registry.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "The native config view must render headlessly.");
}
