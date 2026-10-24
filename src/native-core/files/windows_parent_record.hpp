// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "file_metadata.hpp"
#include <span>
namespace ck::files::detail {
struct WindowsParentIds {
    IdentityScheme scheme;
    NativeIdentifier child, parent;
    std::uint32_t attributes;
    friend bool operator==(const WindowsParentIds&, const WindowsParentIds&) = default;
};
// FSCTL_READ_FILE_USN_DATA only, requested major version exactly 2 or 3.
// No journal history, time/reason interpretation, guessed ID conversion or name
// copying. Native owner/scope/volume and before/after qualification are separate.
Observation<WindowsParentIds> decode_windows_parent_record(std::span<const std::byte> bytes, unsigned requested_major);
} // namespace ck::files::detail
