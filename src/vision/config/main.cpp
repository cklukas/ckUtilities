#include <cstdio>
#include <string_view>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>

#include "ck/vision/keymap.hpp"
#include "chat_options.hpp"
#include "config_app.hpp"
#include "disk_usage_options.hpp"

int main(int argc, char **argv)
{
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        std::printf("Usage: %s\n", argc > 0 ? argv[0] : "ck-config-ckvision");
        return 0;
    }
    ck::config::OptionRegistry diskUsageRegistry("ck-du");
    ck::du::registerDiskUsageOptions(diskUsageRegistry);
    ck::config::OptionRegistry chatRegistry("ck-chat");
    ck::chat::registerChatOptions(chatRegistry);
    ck::vision::DefaultConfigPersistence persistence;
    persistence.load(diskUsageRegistry);
    persistence.load(chatRegistry);
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
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
