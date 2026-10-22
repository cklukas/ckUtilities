// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher's operating-system services on POSIX: fork, execve, waitpid.
#include "launcher_platform.hpp"

#include <cerrno>
#include <cstdlib>
#include <string_view>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ck/launcher.hpp"

extern char **environ;

namespace ck::vision
{

std::filesystem::path launcher_tool_directory(const std::string &argv0)
{
    std::error_code error;
    const std::filesystem::path base = std::filesystem::current_path(error);
    if (argv0.empty())
        return base;
    std::filesystem::path candidate = argv0;
    if (!candidate.is_absolute())
        candidate = base / candidate;
    candidate = candidate.lexically_normal();
    return candidate.has_parent_path() ? candidate.parent_path() : base;
}

ForegroundRun run_foreground(const std::filesystem::path &program, const std::vector<std::string> &arguments,
                             bool launched_from_launcher)
{
    // Everything the child needs is prepared before fork: after it, only
    // async-signal-safe calls are allowed.
    std::vector<std::string> values;
    values.reserve(arguments.size() + 1);
    values.push_back(program.string());
    values.insert(values.end(), arguments.begin(), arguments.end());
    std::vector<char *> argv;
    argv.reserve(values.size() + 1);
    for (std::string &value : values)
        argv.push_back(value.data());
    argv.push_back(nullptr);

    // The environment passes through; a launcher child also gets the marker.
    const std::string marker = std::string(ck::launcher::kLauncherEnvVar) + "=";
    std::vector<std::string> environment;
    for (char **entry = environ; *entry != nullptr; ++entry)
        if (!launched_from_launcher || std::string_view(*entry).rfind(marker, 0) != 0)
            environment.emplace_back(*entry);
    if (launched_from_launcher)
        environment.push_back(marker + ck::launcher::kLauncherEnvValue);
    std::vector<char *> envp;
    envp.reserve(environment.size() + 1);
    for (std::string &entry : environment)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0)
        return {};
    if (pid == 0)
    {
        execve(argv[0], argv.data(), envp.data());
        _exit(127);
    }

    int status = 0;
    pid_t result = 0;
    do
    {
        result = waitpid(pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        return {};
    if (WIFSIGNALED(status))
        return {true, 128 + WTERMSIG(status)};
    return {true, WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE};
}

} // namespace ck::vision
