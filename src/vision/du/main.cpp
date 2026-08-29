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
    ck::du::BuildDirectoryTreeResult snapshot = ck::du::buildDirectoryTree(path, options);
    if (snapshot.root == nullptr)
    {
        std::fprintf(stderr, "Unable to scan %s\n", path.c_str());
        return 1;
    }

    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::DiskUsageApp disk_usage(application, std::move(snapshot));
    application.run();
    return 0;
}
