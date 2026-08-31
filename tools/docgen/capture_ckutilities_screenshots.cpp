// Produces the screenshots embedded by the user-facing README.  The capture
// fixtures exercise the actual ckUtilities views rather than illustrative
// mockups, following ckVision's own HeadlessTerminal-to-SVG documentation
// pipeline.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <cvision/core/clock.hpp>
#include <cvision/core/event.hpp>
#include <cvision/core/filesystem.hpp>
#include <cvision/term/headless_terminal.hpp>
#include <cvision/ui/application.hpp>

#include "edit_app.hpp"
#include "frame_svg.hpp"
#include "json_view_app.hpp"
#include "launcher_app.hpp"

namespace
{

void write_svg(const std::filesystem::path &directory, const std::string &name,
               const ckv::term::VirtualDisplay &display)
{
    const std::filesystem::path path = directory / (name + ".svg");
    std::ofstream output(path, std::ios::binary);
    output << ckv::docgen::render_virtual_display_svg(display);
    if (!output)
        throw std::runtime_error("could not write " + path.string());
    std::cerr << "wrote " << path << " (" << display.size().width << "x" << display.size().height
              << " cells)\n";
}

void capture_launcher(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ck::vision::UtilitiesLauncherApp launcher(application, {});
    application.step(0);
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Down, ckv::Modifier::None, ""}});
    application.step(0);
    write_svg(directory, "ck-utilities-launcher", terminal.display());
}

void capture_json_view(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ckv::MemoryFileSystem files;
    files.add_file("/workspace/release-notes.json",
                   R"({"product":"CK Utilities","framework":"ckVision 0.1.3","tools":["JSON View","Find","Disk Usage","Markdown Editor","Chat"],"release":{"platforms":["macOS","Linux"],"packages":["archive","deb","rpm"],"status":"ready"}})");
    ck::vision::JsonViewApp json_view(application, files);
    if (!json_view.load_file("/workspace/release-notes.json"))
        throw std::runtime_error("could not load JSON capture fixture");
    application.execute_command(json_view.level_command(2));
    application.step(0);
    write_svg(directory, "ck-json-view", terminal.display());
}

void capture_markdown_editor(const std::filesystem::path &directory)
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{120, 34}, ckv::term::headless_no_graphics_profile());
    ckv::ui::Application application(terminal, clock);
    ckv::MemoryFileSystem files;
    files.add_file("/workspace/notes.md",
                   "# CK Utilities\n\n"
                   "A collection of focused terminal tools built with ckVision.\n\n"
                   "## Release checklist\n\n"
                   "- [x] Browse JSON\n"
                   "- [x] Search files\n"
                   "- [x] Inspect disk usage\n"
                   "- [x] Edit Markdown\n");
    ck::vision::EditApp editor(application, files);
    if (!editor.open_file("/workspace/notes.md"))
        throw std::runtime_error("could not load Markdown capture fixture");
    application.step(0);
    write_svg(directory, "ck-edit", terminal.display());
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "capture_ckutilities_screenshots")
                  << " <output-directory>\n";
        return 1;
    }

#if defined(_WIN32)
    _putenv_s("CKTOOLS_DOCUMENTATION_CAPTURE", "1");
#else
    setenv("CKTOOLS_DOCUMENTATION_CAPTURE", "1", 1);
#endif
    const std::filesystem::path output_directory = argv[1];
    std::filesystem::create_directories(output_directory);
    try
    {
        capture_launcher(output_directory);
        capture_json_view(output_directory);
        capture_markdown_editor(output_directory);
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
