// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "attributes.hpp"

namespace ck::files {
// Exact native query scope. None of these alone means complete file security.
enum class SecurityPart {
    Unknown, LinuxAccessAcl, LinuxDefaultAcl, DarwinExtendedAcl,
    WindowsAccess, WindowsIntegrity, WindowsFullDescriptor
};
constexpr bool valid_security_part(SecurityPart part) noexcept {
    return part >= SecurityPart::LinuxAccessAcl && part <= SecurityPart::WindowsFullDescriptor;
}
enum class SecurityEncoding { Unknown, LinuxPosixAclXattrV2, DarwinExternalAcl, WindowsRelativeDescriptor };
enum class SecurityPresence { Unknown, Absent, Present };
struct NativeSecurityValue {
    SecurityPart part = SecurityPart::Unknown;
    SecurityEncoding encoding = SecurityEncoding::Unknown;
    SecurityPresence presence = SecurityPresence::Unknown;
    NativeBytes bytes;
    Observation<ObjectIdentity> owner;
    std::uint32_t native_query_mask = 0;
};
namespace detail {
// Bounded structural validation only, not ACL semantic equivalence, principal
// resolution, effective access evaluation or authorization to replay security.
Observation<std::size_t> validate_linux_acl(std::span<const std::byte> bytes, InventoryLimits limits);
Observation<std::size_t> validate_windows_descriptor(std::span<const std::byte> bytes, InventoryLimits limits);
}
} // namespace ck::files
