// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Setting and clearing environment variables in tests, on POSIX and Windows.
#pragma once

#include <cstdlib>
#include <string>

namespace ck::testing
{

// The variable the suite's configuration folder follows first:
// %APPDATA% on Windows, $XDG_CONFIG_HOME elsewhere.
#if defined(_WIN32)
inline constexpr const char kConfigRootVariable[] = "APPDATA";
#else
inline constexpr const char kConfigRootVariable[] = "XDG_CONFIG_HOME";
#endif

inline bool set_environment(const char *name, const std::string &value)
{
#if defined(_WIN32)
    return _putenv_s(name, value.c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

inline bool unset_environment(const char *name)
{
#if defined(_WIN32)
    // An empty value removes the variable from the process environment.
    return _putenv_s(name, "") == 0;
#else
    return unsetenv(name) == 0;
#endif
}

} // namespace ck::testing
