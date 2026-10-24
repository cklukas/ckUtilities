// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "storage_info.hpp"
#include <span>
namespace ck::files {
// Exact fixed FSCTL response. Unknown algorithms, flag bits and reserved bytes
// are retained; decoding is not validation of data, geometry or replay support.
class WindowsIntegrityData final {
public:
    static constexpr std::size_t wire_bytes = 16;
    static Observation<WindowsIntegrityData> decode(std::span<const std::byte> bytes);
    std::span<const std::byte> raw() const noexcept { return bytes_; }
    std::uint16_t checksum_algorithm() const noexcept { return u16(0); }
    std::uint16_t reserved() const noexcept { return u16(2); }
    std::uint32_t flags() const noexcept { return u32(4); }
    std::uint32_t checksum_chunk_bytes() const noexcept { return u32(8); }
    std::uint32_t cluster_bytes() const noexcept { return u32(12); }
private:
    explicit WindowsIntegrityData(std::array<std::byte, wire_bytes> bytes) noexcept : bytes_(bytes) {}
    std::uint16_t u16(std::size_t offset) const noexcept;
    std::uint32_t u32(std::size_t offset) const noexcept;
    std::array<std::byte, wire_bytes> bytes_;
};
enum class ReportedIntegrityAlgorithm { None, Crc32, Crc64 };
enum class IntegrityQueryFamily { Unknown, WindowsFsctl };
struct FileIntegrityInfo {
    IntegrityQueryFamily family;
    Observation<ObjectIdentity> owner;
    ObjectKind kind;
    std::uint32_t before_windows_attributes, after_windows_attributes;
    WindowsIntegrityData data;
};
// Literal code/bit interpretation only. None does not mean the filesystem lacks
// integrity support, and a clear enforcement-off bit does not certify enabled
// checksums, verified bytes or effective enforcement. No preservation authority.
Observation<ReportedIntegrityAlgorithm> reported_integrity_algorithm(const WindowsIntegrityData& data);
ReportedFlag reported_integrity_enforcement_off(const WindowsIntegrityData& data) noexcept;
namespace detail {
// Regular files/directories only, qualified Windows IDs, no known provider or
// reparse objects, same kind and stable integrity attribute at both endpoints.
// Does not freeze intervening configuration/content or compare all metadata.
Observation<ObjectIdentity> qualify_integrity_subject(const Observation<BasicMetadata>& before, const Observation<BasicMetadata>& after);
}
} // namespace ck::files
