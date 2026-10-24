// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_home.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <userenv.h>
#include <memory>
#include <vector>
namespace ck::files {
namespace {
using Result = Observation<NativeAbsolutePath>;
Result adapter(ObservationState state, AdapterError code, const char *operation) {
    return Result::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
Result native(DWORD code, const char *operation) {
    return Result::unavailable(code == ERROR_ACCESS_DENIED ? ObservationState::Denied : ObservationState::Failed,
                               {ErrorDomain::Win32, code, operation});
}
Result cancelled() { return adapter(ObservationState::Failed, AdapterError::InterruptedRead, "home.cancelled"); }
struct CloseToken {
    void operator()(void *token) const noexcept { ::CloseHandle(token); }
};
} // namespace
Result native_home_address(NativePathLimits limits, std::stop_token stop) {
    if (!limits.max_native_bytes || limits.max_native_bytes > NativePathLimits::hard_native_bytes)
        return adapter(ObservationState::Failed, AdapterError::InvalidLimit, "home.limits");
    if (stop.stop_requested())
        return cancelled();
    HANDLE raw = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw))
        return native(::GetLastError(), "home.process_token");
    const std::unique_ptr<void, CloseToken> token(raw);
    if (stop.stop_requested())
        return cancelled();
    DWORD needed = 0;
    const auto probed = ::GetUserProfileDirectoryW(token.get(), nullptr, &needed);
    const auto probe_error = probed ? ERROR_SUCCESS : ::GetLastError();
    if (probed || probe_error != ERROR_INSUFFICIENT_BUFFER)
        return probed ? adapter(ObservationState::Failed, AdapterError::InvalidNativeResult, "home.profile_probe")
                      : native(probe_error, "home.profile_probe");
    if (needed < 2)
        return adapter(ObservationState::Unknown, AdapterError::InvalidNativeResult, "home.profile_size");
    if (needed - 1 > limits.max_native_bytes / sizeof(char16_t))
        return adapter(ObservationState::Unknown, AdapterError::InventoryLimit, "home.path_bytes");
    if (stop.stop_requested())
        return cancelled();
    std::vector<wchar_t> buffer(needed);
    auto written = needed;
    if (!::GetUserProfileDirectoryW(token.get(), buffer.data(), &written))
        return native(::GetLastError(), "home.profile_path");
    if (stop.stop_requested())
        return cancelled();
    if (written < 2 || written > needed || buffer[written - 1] != L'\0')
        return adapter(ObservationState::Unknown, AdapterError::InvalidNativeResult, "home.profile_result");
    std::u16string units;
    units.reserve(written - 1);
    for (DWORD i = 0; i + 1 < written; ++i)
        units.push_back(static_cast<char16_t>(buffer[i]));
    return NativeAbsolutePath::from_windows(units, limits);
}
} // namespace ck::files
