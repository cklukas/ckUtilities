#include "launcher_app.hpp"
#include "calculator_model.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

namespace
{

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

} // namespace

int main()
{
    require(ck::vision::CalculatorModel::evaluate("2 + 3 * 4").text == "14",
            "The calculator must respect arithmetic precedence.");
    require(ck::vision::CalculatorModel::evaluate("(12 + 8) / 4").text == "5",
            "The calculator must support grouped arithmetic.");
    require(!ck::vision::CalculatorModel::evaluate("1 / 0").accepted,
            "The calculator must reject division by zero.");

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    std::string launched_id;
    ck::vision::UtilitiesLauncherApp launcher(application, [&launched_id](const ck::appinfo::ToolInfo &tool) {
        launched_id = tool.id;
    });

    require(launcher.diagnostics_entry_count() > 0,
            "The launcher must retain an application-owned bounded diagnostics snapshot.");
    require(launcher.launcher_window_count() == 1, "The launcher must open one initial window.");
    require(launcher.selected_tool() != nullptr, "The launcher must select an installed tool.");
    const std::string expected_tool = std::string(launcher.selected_tool()->id);
    require(application.execute_command(launcher.new_launcher_command()),
            "The native New Launcher command must be available.");
    require(launcher.launcher_window_count() == 2, "The native launcher must support multiple windows.");
    require(application.execute_command(launcher.calendar_command()),
            "The native Calendar command must dispatch through the registry.");
    require(launcher.desktop_window_count() == 3,
            "The native calendar must be presented as a regular Desktop window.");
    require(application.execute_command(launcher.ascii_table_command()),
            "The native ASCII table command must dispatch through the registry.");
    require(application.execute_command(launcher.calculator_command()),
            "The native calculator command must dispatch through the registry.");
    require(launcher.desktop_window_count() == 5,
            "The native launcher must present each built-in tool as a regular Desktop window.");
    require(application.execute_command(launcher.diagnostics_command()),
            "The native diagnostics command must dispatch through the registry.");
    require(launcher.desktop_window_count() == 6,
            "The diagnostics snapshot must be presented as a regular Desktop window.");
    require(application.execute_command(launcher.launch_command()),
            "The native Launch command must dispatch through the registry.");
    require(launched_id == expected_tool,
            "The launch request must retain the selected launcher tool behind a utility window.");
    require(application.quit_requested(), "Launching a child tool must close the terminal session first.");

    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30},
            "The native launcher must compose a full headless frame.");
}
