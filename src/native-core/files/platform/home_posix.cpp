// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_home.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <unistd.h>
#include <vector>
namespace ck::files {
namespace {
using Result = Observation<NativeAbsolutePath>;
Result adapter(ObservationState state, AdapterError code, const char *operation) {
    return Result::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
Result cancelled() { return adapter(ObservationState::Failed, AdapterError::InterruptedRead, "home.cancelled"); }
Result address(const char *text, NativePathLimits limits) {
    if (!text)
        return adapter(ObservationState::Unknown, AdapterError::InvalidNativeResult, "home.account_directory");
    const auto size = ::strnlen(text, limits.max_native_bytes + 1);
    if (size > limits.max_native_bytes)
        return adapter(ObservationState::Unknown, AdapterError::InventoryLimit, "home.path_bytes");
    return NativeAbsolutePath::from_posix(std::string_view(text, size), limits);
}
} // namespace
Result native_home_address(NativePathLimits limits, std::stop_token stop) {
    if (!limits.max_native_bytes || limits.max_native_bytes > NativePathLimits::hard_native_bytes)
        return adapter(ObservationState::Failed, AdapterError::InvalidLimit, "home.limits");
    if (stop.stop_requested())
        return cancelled();
    if (const auto *home = std::getenv("HOME"))
        return address(home, limits);
    constexpr std::size_t max_record_bytes = 1024 * 1024;
    const auto suggested = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    auto size = suggested > 0 ? std::min(static_cast<std::size_t>(suggested), max_record_bytes) : std::size_t{16384};
    for (;;) {
        if (stop.stop_requested())
            return cancelled();
        std::vector<char> buffer(size);
        passwd account{};
        passwd *found = nullptr;
        const auto error = ::getpwuid_r(::geteuid(), &account, buffer.data(), buffer.size(), &found);
        if (error == ERANGE) {
            if (size == max_record_bytes)
                return adapter(ObservationState::Unknown, AdapterError::InventoryLimit, "home.account_record_bytes");
            size = std::min(size * 2, max_record_bytes);
            continue;
        }
        if (error)
            return Result::unavailable(error == EACCES || error == EPERM ? ObservationState::Denied
                                                                         : ObservationState::Failed,
                                       {ErrorDomain::Posix, error, "home.getpwuid_r"});
        if (stop.stop_requested())
            return cancelled();
        if (!found)
            return adapter(ObservationState::Unknown, AdapterError::NotApplicable, "home.account_missing");
        return address(found->pw_dir, limits);
    }
}
} // namespace ck::files
