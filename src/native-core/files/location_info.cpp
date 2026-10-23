// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "location_info.hpp"
#include <limits>
#include <optional>

namespace ck::files {
namespace {
template <class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
struct FeatureBit { std::size_t group; std::uint32_t bit; };
std::optional<FeatureBit> feature_bit(VolumeFeatureDomain domain, VolumeFeature feature) {
    if (domain == VolumeFeatureDomain::WindowsVolumeFlags) {
        switch (feature) {
        case VolumeFeature::NamedStreams: return FeatureBit{0, 0x00040000};
        case VolumeFeature::ExtendedAttributes: return FeatureBit{0, 0x00800000};
        case VolumeFeature::PersistentAcls: return FeatureBit{0, 0x00000008};
        case VolumeFeature::SparseFiles: return FeatureBit{0, 0x00000040};
        case VolumeFeature::HardLinks: return FeatureBit{0, 0x00400000};
        case VolumeFeature::CaseSensitiveNames: return FeatureBit{0, 0x00000001};
        case VolumeFeature::CasePreservedNames: return FeatureBit{0, 0x00000002};
        case VolumeFeature::FileCompression: return FeatureBit{0, 0x00000010};
        case VolumeFeature::FileEncryption: return FeatureBit{0, 0x00020000};
        case VolumeFeature::BlockRefcounting: return FeatureBit{0, 0x08000000};
        default: return std::nullopt;
        }
    }
    if (domain == VolumeFeatureDomain::DarwinVolumeCapabilities) {
        switch (feature) {
        case VolumeFeature::NamedStreams: return FeatureBit{1, 0x00002000};
        case VolumeFeature::ExtendedAttributes: return FeatureBit{1, 0x00004000};
        case VolumeFeature::PersistentAcls: return FeatureBit{1, 0x00000400};
        case VolumeFeature::SparseFiles: return FeatureBit{0, 0x00000040};
        case VolumeFeature::HardLinks: return FeatureBit{0, 0x00000004};
        case VolumeFeature::SymbolicLinks: return FeatureBit{0, 0x00000002};
        case VolumeFeature::CaseSensitiveNames: return FeatureBit{0, 0x00000100};
        case VolumeFeature::CasePreservedNames: return FeatureBit{0, 0x00000200};
        case VolumeFeature::FileCompression: return FeatureBit{0, 0x00010000};
        case VolumeFeature::NativeClones: return FeatureBit{1, 0x00010000};
        default: return std::nullopt;
        }
    }
    return std::nullopt;
}
}
Observation<FeatureAdvertisement> advertised_feature(const Observation<NativeVolumeFeatures>& features, VolumeFeature feature) {
    using Result = Observation<FeatureAdvertisement>;
    if (feature <= VolumeFeature::Unknown || feature > VolumeFeature::BlockRefcounting)
        return error<FeatureAdvertisement>(ObservationState::Failed, AdapterError::InvalidIdentifier, "volume.feature");
    if (!features.value()) { const auto& e = *features.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (features.value()->domain != VolumeFeatureDomain::WindowsVolumeFlags && features.value()->domain != VolumeFeatureDomain::DarwinVolumeCapabilities)
        return error<FeatureAdvertisement>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "volume.feature_domain");
    const auto bit = feature_bit(features.value()->domain, feature);
    if (!bit) return error<FeatureAdvertisement>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "volume.feature_mapping");
    if ((features.value()->valid[bit->group] & bit->bit) == 0)
        return error<FeatureAdvertisement>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "volume.feature_validity");
    return Result::available((features.value()->value[bit->group] & bit->bit) != 0 ?
        FeatureAdvertisement::Advertised : FeatureAdvertisement::NotAdvertised);
}
Observation<ReportedFlag> reported_linux_directory_flag(const LocationInfo& location, LinuxDirectoryFeature feature) {
    std::uint32_t bit = 0;
    switch (feature) {
    case LinuxDirectoryFeature::Casefold: bit = 0x40000000U; break;
    case LinuxDirectoryFeature::NoCow: bit = 0x00800000U; break;
    case LinuxDirectoryFeature::ProjectInherit: bit = 0x20000000U; break;
    default: return error<ReportedFlag>(ObservationState::Failed, AdapterError::InvalidIdentifier, "directory.linux_flag");
    }
    if (location.family != LocationQueryFamily::LinuxDescriptor)
        return error<ReportedFlag>(ObservationState::Unsupported, AdapterError::NotApplicable, "directory.linux_flag_family");
    if (!location.linux_directory_inode_flags.value()) {
        const auto& unavailable = *location.linux_directory_inode_flags.unavailable_detail();
        return Observation<ReportedFlag>::unavailable(unavailable.state, unavailable.error);
    }
    return Observation<ReportedFlag>::available((*location.linux_directory_inode_flags.value() & bit) != 0 ? ReportedFlag::Set : ReportedFlag::Clear);
}
namespace detail {
Observation<std::uint64_t> space_bytes(std::uint64_t count, std::uint64_t unit) {
    if (unit == 0) return error<std::uint64_t>(ObservationState::Failed, AdapterError::InvalidNativeResult, "volume.allocation_unit");
    if (count > std::numeric_limits<std::uint64_t>::max() / unit)
        return error<std::uint64_t>(ObservationState::Unknown, AdapterError::InventoryLimit, "volume.byte_count_overflow");
    return Observation<std::uint64_t>::available(count * unit);
}
Observation<std::uint64_t> signed_space_bytes(std::int64_t count, std::uint64_t unit) {
    if (count < 0) return error<std::uint64_t>(ObservationState::Unknown, AdapterError::InvalidNativeResult, "volume.negative_space_count");
    return space_bytes(static_cast<std::uint64_t>(count), unit);
}
}
} // namespace ck::files
