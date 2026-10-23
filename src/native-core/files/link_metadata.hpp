// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "attributes.hpp"

namespace ck::files {
struct PosixSymlinkData { std::string target; }; // raw bytes, never a display/path conversion
enum class WindowsReparseKind { OpaqueMicrosoft, OpaqueGuid, SymbolicLink, MountPoint };
struct WindowsLinkNames {
    std::u16string substitute;
    std::u16string print;
    std::uint32_t flags = 0; // preserve unknown flags; decoding does not authorize replay
};
struct WindowsReparseData {
    std::uint32_t tag = 0;
    WindowsReparseKind kind = WindowsReparseKind::OpaqueMicrosoft;
    NativeBytes raw;
    std::optional<NativeIdentifier> owner_guid;
    std::optional<WindowsLinkNames> names;
};
struct LinkMetadata {
    Observation<ObjectIdentity> owner;
    std::variant<std::monostate, PosixSymlinkData, WindowsReparseData> payload;
};
namespace detail {
using LinkReadReply = std::variant<std::size_t, UnavailableObservation>;
using LinkReadQuery = std::function<LinkReadReply(std::span<std::byte>)>;
// readlink has no size query and silently truncates at capacity. A bounded
// budget+1 witness distinguishes an exact-budget target from truncation. No
// prefix is returned on limits/failure. Native errors are never retried.
Observation<PosixSymlinkData> read_posix_link(const LinkReadQuery& query, InventoryLimits limits);
constexpr std::size_t windows_reparse_max_bytes = 16 * 1024;
// Structural parsing only. Retains the entire native buffer, including opaque
// data and unknown link flags. No target traversal, normalization or execution.
Observation<WindowsReparseData> decode_windows_reparse(std::span<const std::byte> bytes, InventoryLimits limits);
}
} // namespace ck::files
