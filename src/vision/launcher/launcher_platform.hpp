// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the launcher needs from the operating system: where its sibling tools
// are installed, and how to run one of them in this terminal and wait for it.
// launcher_platform_posix.cpp and launcher_platform_windows.cpp implement it.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ck::vision
{

// The folder holding the launcher's executable, and so its sibling tools.
// `argv0` is the launcher's first argument (UTF-8), used where the operating
// system cannot report the executable's own path.
std::filesystem::path launcher_tool_directory(const std::string &argv0);

// The result of one tool run in the foreground.
struct ForegroundRun
{
    bool started = false; // the program could be started at all
    int exit_code = 0;    // its exit code; 128 + signal number when a signal ended it
};

// Runs `program` with `arguments` (UTF-8) in this terminal, inheriting its
// console and environment, and waits for it to end. With
// `launched_from_launcher`, the child sees CK_LAUNCHED_FROM=ck-utilities, so
// its "return to launcher" command exits with kReturnToLauncherExitCode.
ForegroundRun run_foreground(const std::filesystem::path &program, const std::vector<std::string> &arguments,
                             bool launched_from_launcher);

} // namespace ck::vision
