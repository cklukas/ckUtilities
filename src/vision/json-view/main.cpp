#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/term/terminal_clipboard.hpp>
#include <cvision/ui/application.hpp>

#include "ck/vision/keymap.hpp"
#include "native_host.hpp"
#include "json_view_app.hpp"

namespace
{

int run(const std::vector<std::string> &args)
{
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h"))
    {
        std::printf("Usage: %s [JSON_FILE ...]\n", args.empty() ? "ck-json-view" : args[0].c_str());
        return 0;
    }
    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
    ckv::term::TerminalClipboardWriter clipboard(terminal);
    ckv::ui::Application application(terminal, clock, clipboard);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-json-view", application.commands(), keymap_persistence);
    ck::host::NativeFileSystem files;
    ck::vision::JsonViewApp json_view(application, files);
    keymap.load();

    for (std::size_t index = 1; index < args.size(); ++index)
        json_view.load_file(args[index]);

    application.run();
    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    return run(ck::host::native_arguments(argc, argv));
}
