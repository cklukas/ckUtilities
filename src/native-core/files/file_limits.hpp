// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "location_info.hpp"
namespace ck::files {
struct FileSizeBitAssessment {
    Observation<ObjectIdentity> directory;
    std::uint64_t proposed_size = 0, reported_signed_bits = 0;
    unsigned required_signed_bits = 0;
    bool exceeds_reported_width() const noexcept { return required_signed_bits > reported_signed_bits; }
};
struct SymlinkLengthAssessment {
    Observation<ObjectIdentity> directory;
    std::uint64_t payload_bytes = 0, reported_maximum = 0;
    bool exceeds_reported_maximum() const noexcept { return payload_bytes > reported_maximum; }
};
enum class FileLinkLimitFamily { Unknown, PosixFpathconf };
struct FileLinkLimits {
    FileLinkLimitFamily family = FileLinkLimitFamily::Unknown;
    ObjectIdentity owner;
    ObjectKind kind = ObjectKind::Other;
    Observation<std::uint64_t> before_link_count, after_link_count;
    Observation<std::uint64_t> maximum_links;
};
struct HardLinkCountAssessment {
    ObjectIdentity owner;
    ObjectKind kind;
    std::uint64_t proposed_total = 0, reported_maximum = 0;
    bool exceeds_reported_maximum() const noexcept { return proposed_total > reported_maximum; }
};
// Numeric comparisons only, not filesystem acceptance, exact maximum file size,
// remaining space/quota, native offset/API range, target validity or permission.
// FILESIZEBITS describes a signed representation bound, not an exact byte limit.
Observation<FileSizeBitAssessment> assess_file_size_bits(std::uint64_t bytes, const LocationInfo& location);
Observation<SymlinkLengthAssessment> assess_posix_symlink_length(std::uint64_t bytes, const LocationInfo& location);
// Subject-specific total only, never addition to a racing link count. A directory
// report concerns that directory, and must NOT be applied to its child files.
Observation<HardLinkCountAssessment> assess_hard_link_total(std::uint64_t total, const FileLinkLimits& limits);
namespace detail {
Observation<std::uint64_t> decode_posix_limit(std::int64_t result, ObservationState error_state, NativeError error);
Observation<ObjectIdentity> qualify_file_link_subject(const Observation<BasicMetadata>& before, const Observation<BasicMetadata>& after);
}
} // namespace ck::files
