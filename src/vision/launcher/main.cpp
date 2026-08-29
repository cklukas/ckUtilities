#include "launcher_app.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>
#include <cvision/ui/application.hpp>

#include "ck/launcher.hpp"
#include "ck/launcher/cli_utils.hpp"

extern char **environ;

namespace
{

struct LaunchRequest
{
    const ck::appinfo::ToolInfo *tool = nullptr;
    std::vector<std::string> arguments;
};

int run_program(const std::filesystem::path &program, const std::vector<std::string> &arguments,
                bool launched_from_launcher)
{
    const pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0)
    {
        if (launched_from_launcher)
            setenv(ck::launcher::kLauncherEnvVar, ck::launcher::kLauncherEnvValue, 1);

        std::vector<std::string> values;
        values.reserve(arguments.size() + 1);
        values.push_back(program.string());
        values.insert(values.end(), arguments.begin(), arguments.end());
        std::vector<char *> argv;
        argv.reserve(values.size() + 1);
        for (std::string &value : values)
            argv.push_back(value.data());
        argv.push_back(nullptr);
        execve(program.c_str(), argv.data(), environ);
        _exit(127);
    }

    int status = 0;
    pid_t result = 0;
    do
    {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    return result < 0 ? -1 : status;
}

std::optional<LaunchRequest> show_launcher()
{
    std::optional<LaunchRequest> request;
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::UtilitiesLauncherApp launcher(application, [&request](const ck::appinfo::ToolInfo &tool) {
        request = LaunchRequest{&tool, {}};
    });
    application.run();
    return request;
}

const ck::appinfo::ToolInfo *find_tool(std::string_view name)
{
    if (const auto *tool = ck::appinfo::findTool(name))
        return tool;
    return ck::appinfo::findToolByExecutable(name);
}

int status_to_exit_code(int status)
{
    if (status < 0)
        return EXIT_FAILURE;
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : status;
}

int launch(const LaunchRequest &request, const std::filesystem::path &tool_directory, bool return_to_launcher)
{
    if (request.tool == nullptr)
        return EXIT_FAILURE;
    const auto program = ck::launcher::locateProgramPath(tool_directory, *request.tool);
    if (!program)
    {
        std::fprintf(stderr, "Unable to locate %s\n", request.tool->executable.data());
        return EXIT_FAILURE;
    }
    return run_program(*program, request.arguments, return_to_launcher);
}

} // namespace

int main(int argc, char **argv)
{
    const std::filesystem::path tool_directory = ck::launcher::resolveToolDirectory(argc > 0 ? argv[0] : nullptr);
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        std::printf("Usage: %s [--launch TOOL [ARGS...]]\n", argc > 0 ? argv[0] : "ck-utilities");
        return EXIT_SUCCESS;
    }

    if (argc > 2 && std::string_view(argv[1]) == "--launch")
    {
        const ck::appinfo::ToolInfo *tool = find_tool(argv[2]);
        if (tool == nullptr)
        {
            std::fprintf(stderr, "Unknown tool '%s'.\n", argv[2]);
            return EXIT_FAILURE;
        }
        LaunchRequest request{tool, std::vector<std::string>(argv + 3, argv + argc)};
        return status_to_exit_code(launch(request, tool_directory, false));
    }
    if (argc > 1 && std::string_view(argv[1]) == "--launch")
    {
        std::fputs("--launch requires a tool identifier.\n", stderr);
        return EXIT_FAILURE;
    }

    while (true)
    {
        const std::optional<LaunchRequest> request = show_launcher();
        if (!request)
            return EXIT_SUCCESS;

        const int status = launch(*request, tool_directory, true);
        if (status >= 0 && WIFEXITED(status) &&
            WEXITSTATUS(status) == ck::launcher::kReturnToLauncherExitCode)
            continue;
        return status_to_exit_code(status);
    }
}
