#include "json_view_app.hpp"

#include <iostream>
#include <string>

#include <cvision/core/clock.hpp>
#include <cvision/core/filesystem.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

namespace
{

bool expect(bool condition, const char *message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ckv::MemoryFileSystem files;
    files.add_file("/fixture.json",
                   R"({"name":"ckVision","nested":{"name":"ckUtilities"},"enabled":true})");
    ck::vision::JsonViewApp json_view(application, files);

    if (!expect(json_view.load_file("/fixture.json"),
                "JSON document did not load"))
        return 1;
    if (!expect(json_view.tree() != nullptr, "JSON tree was not materialized"))
        return 1;
    if (!expect(json_view.find("ckutilities", false, true), "value search did not find a result"))
        return 1;
    if (!expect(json_view.selected_json_node() != nullptr &&
                    json_view.selected_json_node()->key == "name",
                "search did not reveal and select its matching node"))
        return 1;
    if (!expect(application.execute_command(json_view.level_command(0)), "level command was unavailable"))
        return 1;
    if (!expect(json_view.tree() != nullptr, "tree was lost after an expansion-level change"))
        return 1;
    if (!expect(json_view.find("ckutilities", false, true), "search did not recover after an expansion-level change"))
        return 1;
    if (!expect(application.execute_command(json_view.copy_command()), "copy command was unavailable"))
        return 1;
    if (!expect(application.clipboard_text() == "\"ckUtilities\"", "copy command did not export selected JSON"))
        return 1;

    application.step(0);
    return expect(application.current_frame().size() == ckv::Size{100, 30},
                  "native JSON viewer did not render a full frame")
               ? 0
               : 1;
}
