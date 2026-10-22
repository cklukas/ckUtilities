#include <cstdio>
#include <string>
#include <string_view>
#include <vector>


#include "ck/vision/keymap.hpp"
#include "native_host.hpp"
#include "chat_options.hpp"
#include "config_app.hpp"
#include "disk_usage_options.hpp"

namespace
{

int run(const std::vector<std::string> &args)
{
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h"))
    {
        std::printf("Usage: %s\n", args.empty() ? "ck-config" : args[0].c_str());
        return 0;
    }
    ck::config::OptionRegistry diskUsageRegistry("ck-du");
    ck::du::registerDiskUsageOptions(diskUsageRegistry);
    ck::config::OptionRegistry chatRegistry("ck-chat");
    ck::chat::registerChatOptions(chatRegistry);
    ck::vision::DefaultConfigPersistence persistence;
    persistence.load(diskUsageRegistry);
    persistence.load(chatRegistry);
    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-config", application.commands(), keymap_persistence);
    ck::vision::ConfigApp config(application,
                                 {{"ck-du", "Disk Usage", &diskUsageRegistry, &persistence},
                                  {"ck-chat", "Chat", &chatRegistry, &persistence}},
                                 &keymap);
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
