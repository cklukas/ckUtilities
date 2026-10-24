// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_path.hpp"
#include <stop_token>
namespace ck::files {
// Worker-only requested home address; no existence/identity/permission promise.
// POSIX honors HOME when present (even an invalid/empty value is not replaced),
// otherwise queries the effective account with bounded reentrant lookup.
// Windows queries the process token's profile in native UTF-16. Neither branch
// expands variables/tilde, changes cwd, creates a folder or guesses a fallback.
// The host must keep its process environment immutable after worker startup.
// Account/profile calls can block; cancellation is cooperative between calls.
Observation<NativeAbsolutePath> native_home_address(NativePathLimits limits = {}, std::stop_token stop = {});
} // namespace ck::files
