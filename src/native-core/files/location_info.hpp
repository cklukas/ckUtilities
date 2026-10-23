// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "data_channels.hpp"
#include "storage_info.hpp"

namespace ck::files {
enum class LocationQueryFamily { Unknown, LinuxDescriptor, DarwinDescriptor, WindowsHandle };
enum class VolumeFeatureDomain { Unknown, WindowsVolumeFlags, DarwinVolumeCapabilities };
struct NativeVolumeFeatures {
    VolumeFeatureDomain domain = VolumeFeatureDomain::Unknown;
    std::array<std::uint32_t, 4> value{};
    std::array<std::uint32_t, 4> valid{};
};
enum class VolumeFeature {
    Unknown, NamedStreams, ExtendedAttributes, PersistentAcls, SparseFiles,
    HardLinks, SymbolicLinks, CaseSensitiveNames, CasePreservedNames,
    FileCompression, FileEncryption, NativeClones, BlockRefcounting
};
enum class FeatureAdvertisement { Advertised, NotAdvertised };
enum class LinuxDirectoryFeature { Casefold, NoCow, ProjectInherit };
// An advertisement is not an operation capability or a preservation guarantee.
// Unsupported means this query family has no mapping, not feature absence.
Observation<FeatureAdvertisement> advertised_feature(const Observation<NativeVolumeFeatures>& features, VolumeFeature feature);

struct LocationInfo {
    LocationQueryFamily family = LocationQueryFamily::Unknown;
    Observation<ObjectIdentity> directory;
    Observation<NativeFeatureIdentifier> filesystem_name;
    Observation<std::uint64_t> filesystem_type_code;
    Observation<NativeIdentifier> filesystem_query_id; // native statfs bytes, not durable/global object identity
    Observation<std::uint64_t> volume_query_serial;    // native query serial, not the held object's volume identity
    Observation<NativeVolumeFeatures> features;
    Observation<std::uint64_t> native_mount_flags; // interpreted only in this query family's namespace
    // Independent native mount/volume setting; false does not grant write access.
    Observation<bool> read_only_mount;
    Observation<bool> case_sensitive_directory;
    Observation<bool> case_preserving_names;
    Observation<std::uint32_t> windows_directory_case_flags;
    // FS_IOC_GETFLAGS on the held directory; separate from statx attributes.
    // Clear bits do not prove filesystem support/absence of equivalent features.
    Observation<std::uint32_t> linux_directory_inode_flags;
    // Raw POSIX pathconf results. These are advisory native reports, not a
    // universal Unicode length rule or a promise that a name can be created.
    Observation<std::uint64_t> posix_name_max;
    Observation<std::uint64_t> posix_name_chars_max;
    Observation<std::uint64_t> posix_path_max;
    Observation<std::uint64_t> windows_component_utf16_max;
    // Native reported bytes at query time. Not reserved space, authority to
    // write, proof of quota/physical allocation, or a durable capacity promise.
    Observation<std::uint64_t> space_accounting_unit;
    Observation<std::uint64_t> total_bytes;
    Observation<std::uint64_t> free_bytes;
    Observation<std::uint64_t> available_bytes;
};
// Decode only this native query namespace. Set/Clear describe reported bits,
// not name equivalence, actual CoW extent sharing, quota IDs or write authority.
Observation<ReportedFlag> reported_linux_directory_flag(const LocationInfo& location, LinuxDirectoryFeature feature);
namespace detail {
Observation<std::uint64_t> space_bytes(std::uint64_t count, std::uint64_t unit);
Observation<std::uint64_t> signed_space_bytes(std::int64_t count, std::uint64_t unit);
}
} // namespace ck::files
