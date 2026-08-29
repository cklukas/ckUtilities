#include <cstdio>
#include <filesystem>
#include <string_view>
#include <utility>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>
#include <cvision/ui/application.hpp>

#include "disk_usage_app.hpp"
#include "disk_usage_core.hpp"

int main(int argc, char **argv)
{
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        std::printf("Usage: %s [DIRECTORY]\n", argc > 0 ? argv[0] : "ck-du-ckvision");
        return 0;
    }

    const std::filesystem::path path = argc > 1 ? argv[1] : ".";
    ck::du::BuildDirectoryTreeOptions options;
    options.reportErrors = false;
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::ThreadedDiskUsageScanService scan_service;
    ck::vision::DiskUsageApp disk_usage(application, scan_service, path, std::move(options));
    application.run();
    return 0;
}
