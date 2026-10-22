// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "user_directories.hpp"

#include <cstdlib>
#include <system_error>

namespace ck::ai::detail
{

namespace
{

std::filesystem::path current_directory()
{
    std::error_code error;
    return std::filesystem::current_path(error);
}

#if defined(_WIN32)
// A Windows environment value as a path, read as UTF-16 so any user name works.
std::filesystem::path environment_path(const wchar_t *name)
{
    const wchar_t *value = _wgetenv(name);
    return value != nullptr && *value != L'\0' ? std::filesystem::path(value) : std::filesystem::path();
}
#endif

} // namespace

std::filesystem::path config_directory()
{
#if defined(_WIN32)
    if (auto root = environment_path(L"APPDATA"); !root.empty())
        return root / "cktools";
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "cktools";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "cktools";
#endif
    return current_directory() / "cktools";
}

std::filesystem::path data_directory()
{
#if defined(_WIN32)
    if (auto root = environment_path(L"LOCALAPPDATA"); !root.empty())
        return root / "cktools";
#else
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "share" / "cktools";
#endif
    return current_directory();
}

} // namespace ck::ai::detail
