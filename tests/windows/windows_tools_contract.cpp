// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Windows counterpart of the PTY terminal-profile check: every CK
// Utilities tool starts in a real ConPTY pseudo-console, draws its desktop,
// and exits cleanly through the standard Alt+X quit chord. Each runs with its
// per-user folders in an isolated scratch directory whose name is not ASCII.
#include "cvision/term/windows_clock.hpp"
#include "cvision/term/windows_terminal_subsession.hpp"
#include "cvision/term/windows_wait_set.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{

std::string utf8(const std::filesystem::path &path)
{
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

} // namespace

CK_TEST(windows_every_tool_starts_draws_and_quits_through_conpty)
{
    ck::testing::ScratchDirectory scratch("windows-tools");
    const auto home = scratch.path() / std::filesystem::path(u8"Données-日本");
    std::filesystem::create_directories(home / "AppData" / "Roaming");
    std::filesystem::create_directories(home / "AppData" / "Local");
    const std::string home_text = utf8(home);

    ckv::term::WindowsClock clock;
    ckv::term::WindowsWaitSet waits(clock);
    for (const char *name : {"ck-utilities", "ck-json-view", "ck-find", "ck-du", "ck-edit", "ck-chat", "ck-todo",
                             "ck-sysmon", "ck-config"})
    {
        std::fprintf(stderr, "Tool: %s\n", name);
        const auto image = utf8(std::filesystem::path(CKTOOLS_TOOL_BINARY_DIR) / (std::string(name) + ".exe"));
        auto spec = ckv::term::TerminalLaunchSpec::program(image);
        spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
        spec.working_directory = home_text;
        spec.environment = {{"USERPROFILE", home_text},
                            {"HOME", home_text},
                            {"APPDATA", utf8(home / "AppData" / "Roaming")},
                            {"LOCALAPPDATA", utf8(home / "AppData" / "Local")},
                            {"CK_AI_FORCE_STUB", "1"}};
        // ck-todo's first run asks a modal question, and the quit chord is
        // suppressed while a modal is open; its demo opens straight on a board.
        if (std::string_view(name) == "ck-todo")
            spec.arguments = {"--demo"};
        // Disk Usage scans the isolated folder, not the working tree.
        if (std::string_view(name) == "ck-du")
            spec.arguments = {home_text};
        spec.profile.cells = {100, 30};
        spec.profile.query_policy = ckv::term::TerminalQueryPolicy::NoResponse;
        auto child = ckv::term::WindowsTerminalSubsession::launch(std::move(spec));
        CK_CHECK(child->state() != ckv::term::TerminalSubsessionState::Failed);

        const auto pump = [&](bool until_exit) {
            const auto deadline = clock.now_nanos() + 15'000'000'000LL;
            while (clock.now_nanos() < deadline)
            {
                (void)waits.wait(clock.now_nanos() + 20'000'000LL, child->wait_handles());
                (void)child->drain(64 * 1024);
                if (child->state() == ckv::term::TerminalSubsessionState::Exited)
                    return until_exit;
                if (child->state() == ckv::term::TerminalSubsessionState::Failed)
                    return false;
                if (!until_exit)
                {
                    std::size_t visible = 0;
                    for (const auto &cell : child->snapshot().cell_buffer)
                        if (!cell.is_continuation() && !cell.grapheme().empty() && cell.grapheme() != " ")
                            ++visible;
                    if (visible > 80)
                        return true;
                }
            }
            return false;
        };

        const bool drew = pump(false);
        CK_CHECK(drew);
        if (drew)
        {
            // Let the first frame settle, then quit through the standard chord.
            const auto settled = clock.now_nanos() + 300'000'000LL;
            while (clock.now_nanos() < settled)
            {
                (void)waits.wait(std::min(settled, clock.now_nanos() + 20'000'000LL), child->wait_handles());
                (void)child->drain(64 * 1024);
            }
            child->send_input("\x1b"
                              "x");
            CK_CHECK(pump(true));
        }
        CK_CHECK(child->exit_code() == 0);
        waits.clear();
    }
}
