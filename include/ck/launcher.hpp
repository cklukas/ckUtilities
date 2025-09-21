#pragma once

#include <cstdlib>
#include <cstring>

namespace ck::launcher
{

inline constexpr const char kLauncherEnvVar[] = "CK_LAUNCHED_FROM";
inline constexpr const char kLauncherEnvValue[] = "ck-utilities";
inline constexpr int kReturnToLauncherExitCode = 247; // Arbitrary non-zero code reserved for returning to the launcher.

inline bool launchedFromCkLauncher()
{
    const char *value = std::getenv(kLauncherEnvVar);
    return value && std::strcmp(value, kLauncherEnvValue) == 0;
}

} // namespace ck::launcher
