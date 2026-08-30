#include <cstdio>
#include <filesystem>
#include <string_view>
#include <utility>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>
#include <cvision/ui/application.hpp>

#include "ck/vision/keymap.hpp"
#include "disk_usage_app.hpp"
#include "disk_usage_options.hpp"

int main(int argc, char **argv)
{
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        std::printf("Usage: %s [DIRECTORY]\n", argc > 0 ? argv[0] : "ck-du-ckvision");
        return 0;
    }

    const std::filesystem::path path = argc > 1 ? argv[1] : ".";
    ck::config::OptionRegistry optionsRegistry("ck-du");
    ck::du::registerDiskUsageOptions(optionsRegistry);
    optionsRegistry.loadDefaults();
    ck::du::BuildDirectoryTreeOptions options = ck::du::buildDirectoryTreeOptionsFromRegistry(optionsRegistry);
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-du", application.commands(), keymap_persistence);
    ck::vision::ThreadedDiskUsageScanService scan_service;
    ck::vision::ThreadedDiskUsageFileListService file_list_service;
#if defined(__APPLE__)
    ck::vision::MacDiskUsageCloudService cloud_service;
#else
    ck::vision::UnsupportedDiskUsageCloudService cloud_service;
#endif
    ck::vision::DiskUsageApp disk_usage(application, scan_service, file_list_service, cloud_service, path, std::move(options));
    keymap.load();
    application.run();
    return 0;
}
