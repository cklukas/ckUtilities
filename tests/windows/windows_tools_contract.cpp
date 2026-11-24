// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Windows counterpart of the PTY terminal-profile check: every CK
// Utilities tool starts in a real ConPTY pseudo-console, draws its desktop,
// and exits cleanly through the standard Alt+X quit chord. Each runs with its
// per-user folders in an isolated scratch directory whose name is not ASCII.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "cvision/term/process_runner.hpp"
#include "cvision/term/windows_clock.hpp"
#include "cvision/term/windows_terminal_subsession.hpp"
#include "cvision/term/windows_text.hpp"
#include "cvision/term/windows_wait_set.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include "windows_name.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

std::string utf8(const std::filesystem::path &path)
{
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::optional<std::string> tool_image(std::string_view tool)
{
    const std::string executable = std::string(tool) + ".exe";
    wchar_t package_mode[2]{};
    const DWORD mode_length = GetEnvironmentVariableW(L"CKTOOLS_TEST_PACKAGE_IMAGE", package_mode, 2);
    if (mode_length == 0)
        return utf8(std::filesystem::path(CKTOOLS_TOOL_BINARY_DIR) / executable);
    if (mode_length != 1 || package_mode[0] != L'1')
        return std::nullopt;

    constexpr wchar_t variable[] = L"CKTOOLS_EXTRACTED_PACKAGE_BIN";
    const DWORD needed = GetEnvironmentVariableW(variable, nullptr, 0);
    if (needed <= 1 || needed > 32768)
        return std::nullopt;
    std::wstring directory(needed, L'\0');
    const DWORD copied = GetEnvironmentVariableW(variable, directory.data(), needed);
    if (copied != needed - 1)
        return std::nullopt;
    directory.resize(copied);
    if (!directory.starts_with(L"\\\\?\\"))
        return std::nullopt;

    // Apply the same address and encoding checks as ckVision's wide launch.
    const auto image = (std::filesystem::path(directory) / executable).native();
    const auto native = ck::files::windows_extended_path(image);
    if (!native || *native != image)
        return std::nullopt;
    return ckv::term::windows_utf8(*native);
}

ckv::core::ProcessRunResult run_tool_help(ckv::core::ProcessRunner &runner, ckv::core::ProcessLaunchSpec launch)
{
    launch.arguments = {"--help"};
    ckv::core::ProcessRunRequest request;
    request.launch = std::move(launch);
    request.descendants = ckv::core::ProcessDescendantPolicy::TerminateOnCompletion;
    request.max_stdout_bytes = 0;
    request.max_stderr_bytes = 0;
    return runner.run(request);
}

} // namespace

CK_TEST(windows_every_tool_starts_draws_and_quits_through_conpty)
{
    ck::testing::ScratchDirectory scratch("windows-tools");
    const auto home = scratch.path() / std::filesystem::path(u8"Données-日本");
    std::filesystem::create_directories(home / "AppData" / "Roaming");
    std::filesystem::create_directories(home / "AppData" / "Local");
    const auto files_left = home / "FilesLeft";
    const auto files_right = home / "FilesRight";
    std::filesystem::create_directories(files_left);
    std::filesystem::create_directories(files_right);
    {
        std::ofstream(files_left / "a-mouse-target.txt", std::ios::binary) << "a\n";
        std::ofstream(files_left / "b-mouse-target.txt", std::ios::binary) << "b\n";
    }
    const std::string home_text = utf8(home);

    ckv::term::WindowsClock clock;
    ckv::term::NativeProcessRunner runner(clock);
    ckv::term::WindowsWaitSet waits(clock);
    for (const char *name : {"ck-utilities", "ck-json-view", "ck-find", "ck-du", "ck-edit", "ck-chat", "ck-todo",
                             "ck-sysmon", "ck-config", "ck-files"})
    {
        std::fprintf(stderr, "Tool: %s\n", name);
        const auto image = tool_image(name);
        CK_CHECK(image.has_value());
        if (!image)
            return;
        auto spec = ckv::term::TerminalLaunchSpec::program(*image);
        spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
        spec.working_directory = home_text;
        spec.environment = {{"USERPROFILE", home_text},
                            {"HOME", home_text},
                            {"APPDATA", utf8(home / "AppData" / "Roaming")},
                            {"LOCALAPPDATA", utf8(home / "AppData" / "Local")},
                            {"CK_AI_FORCE_STUB", "1"}};
        const auto help = run_tool_help(runner, spec);
        CK_CHECK(help.successful());
        if (!help.successful()) {
            std::fprintf(stderr, "Native --help failed for %s (state %d, native error %llu): %s\n",
                         name, static_cast<int>(help.state),
                         static_cast<unsigned long long>(help.error.code), help.diagnostic.c_str());
            return;
        }
        // ck-todo's first run asks a modal question, and the quit chord is
        // suppressed while a modal is open; its demo opens straight on a board.
        if (std::string_view(name) == "ck-todo")
            spec.arguments = {"--demo"};
        // Disk Usage scans the isolated folder, not the working tree.
        if (std::string_view(name) == "ck-du")
            spec.arguments = {home_text};
        if (std::string_view(name) == "ck-files")
            spec.arguments = {utf8(files_left), utf8(files_right)};
        spec.profile.cells = {100, 30};
        spec.profile.query_policy = ckv::term::TerminalQueryPolicy::NoResponse;
        auto child = ckv::term::WindowsTerminalSubsession::launch(std::move(spec));
        if (child->state() == ckv::term::TerminalSubsessionState::Failed) {
            for (const auto &diagnostic : child->diagnostics())
                std::fprintf(stderr, "ConPTY diagnostic: %s\n", diagnostic.message.c_str());
        }
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
        const auto snapshot_text = [&] {
            std::string text;
            for (const auto &cell : child->snapshot().cell_buffer)
                if (!cell.is_continuation())
                    text += cell.grapheme();
            return text;
        };
        const auto wait_for_text = [&](std::string_view needle) {
            const auto deadline = clock.now_nanos() + 5'000'000'000LL;
            while (clock.now_nanos() < deadline) {
                (void)waits.wait(clock.now_nanos() + 20'000'000LL, child->wait_handles());
                (void)child->drain(64 * 1024);
                if (child->state() == ckv::term::TerminalSubsessionState::Failed ||
                    child->state() == ckv::term::TerminalSubsessionState::Exited)
                    return false;
                if (snapshot_text().find(needle) != std::string::npos)
                    return true;
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
            // Exercise the actual ConPTY input path with an unbound modified
            // key and a harmless SGR mouse press/release on the footer cell.
            // The click is outside file rows; Alt+X remains the only exit
            // command and proves that input handling leaves the tool usable.
            child->send_input("\x1b[1;2A"
                              "\x1b[999~"
                              "\x1b[<0;99;23M"
                              "\x1b[<0;99;23m");
            if (std::string_view(name) == "ck-files") {
                child->send_input("\x1b[<0;10;4M"
                                  "\x1b[<0;10;4m"
                                  "\x1b[2~");
                CK_CHECK(wait_for_text("Marked: 1"));
            }
            child->send_input("\x1b"
                              "x");
            CK_CHECK(pump(true));
        }
        CK_CHECK(child->exit_code() == 0);
        waits.clear();
    }
}

CK_TEST(windows_launcher_starts_files_and_returns_through_conpty)
{
    ck::testing::ScratchDirectory scratch("windows-launcher");
    const auto home = scratch.path() / std::filesystem::path(u8"Données-日本");
    std::filesystem::create_directories(home / "AppData" / "Roaming");
    std::filesystem::create_directories(home / "AppData" / "Local");
    const auto files_left = home / "FilesLeft";
    const auto files_right = home / "FilesRight";
    std::filesystem::create_directories(files_left);
    std::filesystem::create_directories(files_right);

    const std::string home_text = utf8(home);
    const auto image = tool_image("ck-utilities");
    CK_CHECK(image.has_value());
    if (!image)
        return;
    auto spec = ckv::term::TerminalLaunchSpec::program(*image);
    spec.arguments = {"--launch", "ck-files", utf8(files_left), utf8(files_right)};
    spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    spec.working_directory = home_text;
    spec.environment = {{"USERPROFILE", home_text},
                        {"HOME", home_text},
                        {"APPDATA", utf8(home / "AppData" / "Roaming")},
                        {"LOCALAPPDATA", utf8(home / "AppData" / "Local")},
                        {"CK_AI_FORCE_STUB", "1"}};
    spec.profile.cells = {100, 30};
    spec.profile.query_policy = ckv::term::TerminalQueryPolicy::NoResponse;

    ckv::term::WindowsClock clock;
    ckv::term::WindowsWaitSet waits(clock);
    auto child = ckv::term::WindowsTerminalSubsession::launch(std::move(spec));
    if (child->state() == ckv::term::TerminalSubsessionState::Failed) {
        for (const auto &diagnostic : child->diagnostics())
            std::fprintf(stderr, "ConPTY launcher diagnostic: %s\n", diagnostic.message.c_str());
    }
    CK_CHECK(child->state() != ckv::term::TerminalSubsessionState::Failed);

    const auto pump = [&](bool until_exit) {
        const auto deadline = clock.now_nanos() + 15'000'000'000LL;
        while (clock.now_nanos() < deadline) {
            (void)waits.wait(clock.now_nanos() + 20'000'000LL, child->wait_handles());
            (void)child->drain(64 * 1024);
            if (child->state() == ckv::term::TerminalSubsessionState::Exited)
                return until_exit;
            if (child->state() == ckv::term::TerminalSubsessionState::Failed)
                return false;
            if (!until_exit) {
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
    if (drew) {
        child->send_input("\x1b"
                          "x");
        CK_CHECK(pump(true));
    }
    CK_CHECK(child->exit_code() == 0);
    waits.clear();
}

CK_TEST(windows_files_conpty_wp02_selection_settings_and_volumes)
{
    ck::testing::ScratchDirectory scratch("windows-files-wp02");
    const auto home = scratch.path() / std::filesystem::path(u8"Données-日本");
    const auto roaming = home / "AppData" / "Roaming";
    const auto local = home / "AppData" / "Local";
    const auto files_left = home / "FilesLeft";
    const auto files_right = home / "FilesRight";
    std::filesystem::create_directories(roaming);
    std::filesystem::create_directories(local);
    std::filesystem::create_directories(files_left);
    std::filesystem::create_directories(files_right);
    {
        std::ofstream(files_left / "b-stay.txt", std::ios::binary) << "stay\n";
        std::ofstream(files_left / "c-other.txt", std::ios::binary) << "other\n";
        std::ofstream(files_right / "r-file.txt", std::ios::binary) << "right\n";
    }

    const auto image = tool_image("ck-files");
    CK_CHECK(image.has_value());
    if (!image)
        return;

    ckv::term::WindowsClock clock;
    ckv::term::WindowsWaitSet waits(clock);
    const std::string home_text = utf8(home);
    const auto make_spec = [&](const std::filesystem::path &left, const std::filesystem::path &right) {
        auto spec = ckv::term::TerminalLaunchSpec::program(*image);
        spec.arguments = {utf8(left), utf8(right)};
        spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
        spec.working_directory = home_text;
        spec.environment = {{"USERPROFILE", home_text},
                            {"HOME", home_text},
                            {"APPDATA", utf8(roaming)},
                            {"LOCALAPPDATA", utf8(local)},
                            {"CK_AI_FORCE_STUB", "1"}};
        spec.profile.cells = {120, 34};
        spec.profile.query_policy = ckv::term::TerminalQueryPolicy::NoResponse;
        return spec;
    };

    const auto snapshot_text = [](const ckv::term::WindowsTerminalSubsession &child) {
        std::string text;
        for (const auto &cell : child.snapshot().cell_buffer)
            if (!cell.is_continuation())
                text += cell.grapheme();
        return text;
    };
    const auto wait_for = [&](ckv::term::WindowsTerminalSubsession &child, auto &&predicate, long long timeout_ns) {
        const auto deadline = clock.now_nanos() + timeout_ns;
        while (clock.now_nanos() < deadline) {
            (void)waits.wait(clock.now_nanos() + 20'000'000LL, child.wait_handles());
            (void)child.drain(64 * 1024);
            if (child.state() == ckv::term::TerminalSubsessionState::Failed)
                return false;
            if (predicate())
                return true;
            if (child.state() == ckv::term::TerminalSubsessionState::Exited)
                return false;
        }
        return false;
    };
    const auto settle = [&](ckv::term::WindowsTerminalSubsession &child, long long duration_ns) {
        const auto until = clock.now_nanos() + duration_ns;
        while (clock.now_nanos() < until) {
            (void)waits.wait(std::min(until, clock.now_nanos() + 20'000'000LL), child.wait_handles());
            (void)child.drain(64 * 1024);
        }
    };

    // Session 1: selection continuity across Ctrl+R refresh + bookmark persistence on exit.
    {
        auto child = ckv::term::WindowsTerminalSubsession::launch(make_spec(files_left, files_right));
        CK_CHECK(child->state() != ckv::term::TerminalSubsessionState::Failed);
        CK_CHECK(wait_for(*child, [&] { return snapshot_text(*child).find("b-stay.txt") != std::string::npos; },
                          15'000'000'000LL));
        settle(*child, 250'000'000LL);

        // Mark b-stay.txt (row 0) via Insert.
        child->send_input("\x1b[2~");
        CK_CHECK(wait_for(*child, [&] { return snapshot_text(*child).find("Marked: 1") != std::string::npos; },
                          5'000'000'000LL));

        // Insert a-inserted.txt externally so b-stay.txt shifts down, then refresh (Ctrl+R).
        {
            std::ofstream(files_left / "a-inserted.txt", std::ios::binary) << "inserted\n";
        }
        child->send_input("\x12");
        CK_CHECK(wait_for(*child,
                          [&] {
                              const auto text = snapshot_text(*child);
                              return text.find("a-inserted.txt") != std::string::npos &&
                                     text.find("Marked: 1") != std::string::npos;
                          },
                          10'000'000'000LL));

        // Add bookmark for FilesLeft via Location -> Add bookmark (Alt+L, d), then quit cleanly.
        child->send_input("\x1bl");
        settle(*child, 200'000'000LL);
        child->send_input("d");
        settle(*child, 400'000'000LL);
        child->send_input("\x1bx");
        CK_CHECK(wait_for(*child, [&] { return child->state() == ckv::term::TerminalSubsessionState::Exited; },
                          15'000'000'000LL));
        CK_CHECK(child->exit_code() == 0);
        waits.clear();
    }

    // Verify persisted settings file on disk after clean shutdown.
    const auto settings_dir = roaming / "ck-utilities" / "files";
    bool found_bookmark = false;
    if (std::filesystem::is_directory(settings_dir)) {
        for (const auto &entry : std::filesystem::directory_iterator(settings_dir)) {
            if (!entry.is_regular_file())
                continue;
            const auto name = entry.path().filename().string();
            if (name.find("settings") == std::string::npos)
                continue;
            std::ifstream in(entry.path(), std::ios::binary);
            std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (body.find("CKFILES_SETTINGS 1") != std::string::npos &&
                body.find("\nBOOKMARK 0 W ") != std::string::npos)
                found_bookmark = true;
        }
    }
    CK_CHECK(found_bookmark);

    // Session 2: reopen persisted bookmark across restart and inspect Windows NTFS volumes view.
    {
        auto child = ckv::term::WindowsTerminalSubsession::launch(make_spec(files_right, files_right));
        CK_CHECK(child->state() != ckv::term::TerminalSubsessionState::Failed);
        CK_CHECK(wait_for(*child, [&] { return snapshot_text(*child).find("r-file.txt") != std::string::npos; },
                          15'000'000'000LL));
        settle(*child, 250'000'000LL);

        // Open bookmark via Location -> Open bookmark... (Alt+L, k) and press Enter.
        child->send_input("\x1bl");
        settle(*child, 200'000'000LL);
        child->send_input("k");
        CK_CHECK(wait_for(*child, [&] { return snapshot_text(*child).find("Left bookmarks") != std::string::npos; },
                          5'000'000'000LL));
        child->send_input("\r");
        CK_CHECK(wait_for(*child, [&] { return snapshot_text(*child).find("a-inserted.txt") != std::string::npos; },
                          10'000'000'000LL));

        // Navigate to Location root (Alt+L, r), then Parent (Backspace) to inspect pane-local Windows volumes.
        child->send_input("\x1bl");
        settle(*child, 200'000'000LL);
        child->send_input("r");
        settle(*child, 500'000'000LL);
        child->send_input("\x7f");
        CK_CHECK(wait_for(*child,
                          [&] {
                              const auto text = snapshot_text(*child);
                              return text.find("Volumes") != std::string::npos &&
                                     (text.find("NTFS") != std::string::npos || text.find("V:\\") != std::string::npos);
                          },
                          10'000'000'000LL));
        // Volume Copy stays gated on Windows; pressing F5 must leave the destination unchanged.
        child->send_input("\x1b[15~");
        settle(*child, 400'000'000LL);
        // Dismiss any archive review/refusal modal before quitting cleanly via Alt+X.
        child->send_input("\x1b");
        settle(*child, 300'000'000LL);

        // Quit cleanly via Alt+X.
        child->send_input("\x1bx");
        CK_CHECK(wait_for(*child, [&] { return child->state() == ckv::term::TerminalSubsessionState::Exited; },
                          15'000'000'000LL));
        CK_CHECK(child->exit_code() == 0);
        waits.clear();
    }
    std::size_t right_count = 0;
    for (const auto &entry : std::filesystem::directory_iterator(files_right)) {
        (void)entry;
        ++right_count;
    }
    CK_CHECK(right_count == 1 && std::filesystem::exists(files_right / "r-file.txt"));
}
