// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>
#if defined(_WIN32)
#include "cvision/term/windows_clock.hpp"
#include "cvision/term/windows_filesystem.hpp"
#include "cvision/term/windows_terminal.hpp"
#else
#include "cvision/term/posix_clock.hpp"
#include "cvision/term/posix_filesystem.hpp"
#include "cvision/term/posix_terminal.hpp"
#include <pwd.h>
#include <unistd.h>
#endif

namespace ck::host {
#if defined(_WIN32)
using NativeClock = ckv::term::WindowsClock;
using NativeTerminal = ckv::term::WindowsTerminal;
using NativeFileSystem = ckv::term::WindowsFileSystem;
#else
using NativeClock = ckv::term::PosixClock;
using NativeTerminal = ckv::term::PosixTerminal;
using NativeFileSystem = ckv::term::PosixFileSystem;
#endif
#if defined(_WIN32)
inline std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const auto count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Invalid Unicode host text");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("Cannot convert Unicode host text");
    return result;
}
inline std::vector<std::string> native_arguments(int argc, wchar_t** argv) {
    std::vector<std::string> result;
    for (int i = 0; i < argc; ++i) result.push_back(utf8(argv[i]));
    return result;
}
#else
inline std::vector<std::string> native_arguments(int argc, char** argv) {
    return {argv, argv + argc};
}
#endif
// A UTF-8 command-line or configuration string as a filesystem path. A path
// built from a plain std::string is read in the ANSI code page on Windows.
inline std::filesystem::path utf8_path(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

struct UserIdentity { std::string home, name; };
inline UserIdentity current_user() {
    UserIdentity result;
#if defined(_WIN32)
    if (const auto* home = ::_wgetenv(L"USERPROFILE")) result.home = utf8(home);
    if (const auto* user = ::_wgetenv(L"USERNAME")) result.name = utf8(user);
#else
    if (const passwd* record = ::getpwuid(::getuid())) {
        if (record->pw_dir) result.home = record->pw_dir;
        if (record->pw_name) result.name = record->pw_name;
    }
    if (const auto* home = std::getenv("HOME"); result.home.empty() && home) result.home = home;
    if (const auto* user = std::getenv("USER"); result.name.empty() && user) result.name = user;
#endif
    if (result.name.empty()) result.name = "local-user";
    return result;
}

// The suite's per-user folder, where every CK Utilities tool keeps its
// settings and data -- the same rules as the options store: %APPDATA% on
// Windows, else $XDG_CONFIG_HOME, else ~/.config, each with "ck-utilities"
// appended. Returned as UTF-8; empty when no root can be resolved.
inline std::string suite_config_root() {
#if defined(_WIN32)
    if (const auto* appdata = ::_wgetenv(L"APPDATA"); appdata && *appdata)
        return utf8(appdata) + "\\ck-utilities";
#endif
    if (const auto* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
        return std::string(config) + "/ck-utilities";
    const auto home = current_user().home;
    return home.empty() ? std::string{} : home + "/.config/ck-utilities";
}
} // namespace ck::host
