#include "launcher_app.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cvision/ui/application.hpp>

#include "ck/launcher.hpp"
#include "ck/launcher/cli_utils.hpp"
#include "ck/vision/keymap.hpp"
#include "launcher_platform.hpp"
#include "native_host.hpp"

namespace
{

struct LaunchRequest
{
    const ck::appinfo::ToolInfo *tool = nullptr;
    std::vector<std::string> arguments;
};

std::optional<LaunchRequest> show_launcher()
{
    std::optional<LaunchRequest> request;
    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-utilities", application.commands(), keymap_persistence);
    ck::vision::UtilitiesLauncherApp launcher(application, [&request](const ck::appinfo::ToolInfo &tool) {
        request = LaunchRequest{&tool, {}};
    });
    keymap.load();
    application.run();
    return request;
}

const ck::appinfo::ToolInfo *find_tool(std::string_view name)
{
    if (const auto *tool = ck::appinfo::findTool(name))
        return tool;
    return ck::appinfo::findToolByExecutable(name);
}

// The tool's exit code, or nothing when it could not be found or started.
std::optional<int> launch(const LaunchRequest &request, const std::filesystem::path &tool_directory,
                          bool return_to_launcher)
{
    if (request.tool == nullptr)
        return std::nullopt;
    const auto program = ck::launcher::locateProgramPath(tool_directory, *request.tool);
    if (!program)
    {
        std::fprintf(stderr, "Unable to locate %s\n", request.tool->executable.data());
        return std::nullopt;
    }
    const ck::vision::ForegroundRun run = ck::vision::run_foreground(*program, request.arguments, return_to_launcher);
    if (!run.started)
    {
        std::fprintf(stderr, "Unable to start %s\n", request.tool->executable.data());
        return std::nullopt;
    }
    return run.exit_code;
}

int run(const std::vector<std::string> &args)
{
    const std::filesystem::path tool_directory = ck::vision::launcher_tool_directory(args.empty() ? std::string() : args[0]);
    const char *self = args.empty() ? "ck-utilities" : args[0].c_str();
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h"))
    {
        std::printf("Usage: %s [--launch TOOL [ARGS...]]\n", self);
        return EXIT_SUCCESS;
    }

    if (args.size() > 2 && args[1] == "--launch")
    {
        const ck::appinfo::ToolInfo *tool = find_tool(args[2]);
        if (tool == nullptr)
        {
            std::fprintf(stderr, "Unknown tool '%s'.\n", args[2].c_str());
            return EXIT_FAILURE;
        }
        LaunchRequest request{tool, std::vector<std::string>(args.begin() + 3, args.end())};
        return launch(request, tool_directory, false).value_or(EXIT_FAILURE);
    }
    if (args.size() > 1 && args[1] == "--launch")
    {
        std::fputs("--launch requires a tool identifier.\n", stderr);
        return EXIT_FAILURE;
    }

    while (true)
    {
        const std::optional<LaunchRequest> request = show_launcher();
        if (!request)
            return EXIT_SUCCESS;

        const std::optional<int> status = launch(*request, tool_directory, true);
        if (status == ck::launcher::kReturnToLauncherExitCode)
            continue;
        return status.value_or(EXIT_FAILURE);
    }
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
