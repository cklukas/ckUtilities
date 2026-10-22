#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/ui/application.hpp>

#include "ck/vision/keymap.hpp"
#include "native_host.hpp"
#include "find_app.hpp"

namespace
{

int run(const std::vector<std::string> &args)
{
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h"))
    {
        std::printf("Usage: %s\n", args.empty() ? "ck-find" : args[0].c_str());
        return 0;
    }
    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-find", application.commands(), keymap_persistence);
    ck::vision::CoreFindSpecificationStore specifications;
    ck::vision::ThreadedFindExecutionService execution;
    ck::vision::FindApp find(application, specifications, execution);
    keymap.load();
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
