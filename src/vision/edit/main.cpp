#include <cstdio>
#include <string>
#include <string_view>
#include <vector>


#include "ck/vision/keymap.hpp"
#include "native_host.hpp"
#include "edit_app.hpp"

namespace
{

int run(const std::vector<std::string> &args)
{
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h"))
    {
        std::printf("Usage: %s [MARKDOWN_FILE]\n", args.empty() ? "ck-edit" : args[0].c_str());
        return 0;
    }
    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
    ck::host::NativeFileSystem files;
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-edit", application.commands(), keymap_persistence);
    ck::vision::EditApp editor(application, files);
    keymap.load();
    if (args.size() > 1)
        editor.open_file(args[1]);
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
