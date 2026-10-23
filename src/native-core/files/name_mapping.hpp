// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_name.hpp"
#include "observation.hpp"
#include <cstddef>

namespace ck::files {
// Byte budget applies independently to input/output native storage. UTF-16 is
// counted in bytes here, not scalars or display columns. This is a resource
// bound, not a filesystem name/path limit. No returned prefix on any failure.
struct NameMappingLimits {
    std::size_t max_native_bytes = 64 * 1024;
    static constexpr std::size_t hard_native_bytes = 1024 * 1024;
};
// Same encoding preserves arbitrary valid native components, including invalid
// Unicode. Cross encoding requires strict UTF-8/UTF-16 scalar representation.
// Windows -> POSIX uses UTF-8 bytes explicitly, never the process locale.
// This does not sanitize, normalize, case-fold, validate destination filesystem
// rules, prove collision freedom or authorize creation. Display text is unused.
Observation<NativeName> map_native_name(const NativeName& source, NameEncoding destination, NameMappingLimits limits = {});
} // namespace ck::files
