#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

#include <cvision/ui/application.hpp>

#include "ck/vision/keymap.hpp"
#include "native_host.hpp"
#include "disk_usage_app.hpp"
#include "disk_usage_options.hpp"

namespace
{

int run(const std::vector<std::string> &args)
{
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h"))
    {
        std::printf("Usage: %s [DIRECTORY]\n", args.empty() ? "ck-du" : args[0].c_str());
        return 0;
    }

    const std::filesystem::path path = args.size() > 1 ? ck::host::utf8_path(args[1]) : std::filesystem::path(".");
    ck::config::OptionRegistry optionsRegistry("ck-du");
    ck::du::registerDiskUsageOptions(optionsRegistry);
    optionsRegistry.loadDefaults();
    ck::du::BuildDirectoryTreeOptions options = ck::du::buildDirectoryTreeOptionsFromRegistry(optionsRegistry);
    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
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

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    return run(ck::host::native_arguments(argc, argv));
}
