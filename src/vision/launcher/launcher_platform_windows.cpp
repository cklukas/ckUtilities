// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The launcher's operating-system services on Windows, from Microsoft's
// documentation of GetModuleFileNameW, CreateProcessW, WaitForSingleObject
// and GetExitCodeProcess, and of how the C runtime splits a command line
// into arguments ("Parsing C++ command-line arguments").
#include "launcher_platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>

#include "ck/launcher.hpp"

namespace ck::vision
{

namespace
{

std::wstring widen(const std::string &text)
{
    if (text.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(count > 0 ? count : 0), L'\0');
    if (count > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
    return wide;
}

// Quotes one argument so the child's C runtime reads it back unchanged:
// backslashes are literal except before a quote, where they are doubled.
void append_argument(std::wstring &command_line, const std::wstring &argument)
{
    if (!command_line.empty())
        command_line.push_back(L' ');
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
    {
        command_line += argument;
        return;
    }
    command_line.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (ch == L'"')
            command_line.append(backslashes * 2 + 1, L'\\');
        else
            command_line.append(backslashes, L'\\');
        backslashes = 0;
        command_line.push_back(ch);
    }
    command_line.append(backslashes * 2, L'\\');
    command_line.push_back(L'"');
}

struct HandleCloser
{
    void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};

} // namespace

std::filesystem::path launcher_tool_directory(const std::string &argv0)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            break;
        if (length < buffer.size())
        {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
    std::error_code error;
    const std::filesystem::path base = std::filesystem::current_path(error);
    const std::filesystem::path candidate(widen(argv0));
    return candidate.has_parent_path() ? (base / candidate).lexically_normal().parent_path() : base;
}

ForegroundRun run_foreground(const std::filesystem::path &program, const std::vector<std::string> &arguments,
                             bool launched_from_launcher)
{
    std::wstring command_line;
    append_argument(command_line, program.wstring());
    for (const std::string &argument : arguments)
        append_argument(command_line, widen(argument));

    // The child inherits this process's environment. A launcher child also
    // gets the marker, set for the duration of the launch only.
    const std::wstring name = widen(ck::launcher::kLauncherEnvVar);
    if (launched_from_launcher)
        SetEnvironmentVariableW(name.c_str(), widen(ck::launcher::kLauncherEnvValue).c_str());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // No new console: the tool shares this one, the way a shell runs it.
    const BOOL created = CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                        nullptr, &startup, &process);
    if (launched_from_launcher)
        SetEnvironmentVariableW(name.c_str(), nullptr);
    if (!created)
        return {};
    const std::unique_ptr<void, HandleCloser> thread(process.hThread);
    const std::unique_ptr<void, HandleCloser> child(process.hProcess);

    if (WaitForSingleObject(child.get(), INFINITE) != WAIT_OBJECT_0)
        return {};
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(child.get(), &exit_code))
        return {};
    return {true, static_cast<int>(exit_code)};
}

} // namespace ck::vision
