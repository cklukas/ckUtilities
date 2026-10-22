// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_name.hpp"
#include "observation.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace ck::files::detail {
// Documented FILE_ID_BOTH_DIR_INFO layout. The native adapter statically checks
// these offsets against its Windows SDK. Parsing remains testable on all hosts.
inline constexpr std::size_t windows_directory_name_offset = 104;
inline constexpr std::size_t windows_directory_name_length_offset = 60;
struct WindowsDirectoryRecord {
    std::optional<NativeName> name; // navigation records are deliberately omitted
    std::optional<std::size_t> next_offset; // absolute offset within this buffer
};
Observation<WindowsDirectoryRecord> decode_windows_directory_record(
    std::span<const std::byte> bytes, std::size_t position);
} // namespace ck::files::detail
