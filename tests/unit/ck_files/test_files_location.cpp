// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "location_info.hpp"
#include "directory_cursor.hpp"
#include "cvision/testing/cktest.hpp"
#include <limits>

namespace {
using namespace ck::files;
class EmptyCursor final : public DirectoryCursor {
    Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
};
}
CK_TEST(location_defaults_and_missing_native_queries_never_imply_features_or_write_access) {
    const LocationInfo info;
    CK_CHECK(info.family == LocationQueryFamily::Unknown && info.features.state() == ObservationState::Unknown);
    CK_CHECK(info.read_only_mount.state() == ObservationState::Unknown && info.available_bytes.state() == ObservationState::Unknown);
    EmptyCursor cursor;
    CK_CHECK(cursor.inspect_location().state() == ObservationState::Unsupported);
    CK_CHECK(advertised_feature(info.features, VolumeFeature::SparseFiles).state() == ObservationState::Unknown);
}
CK_TEST(volume_advertisements_require_the_native_valid_bit_even_when_the_feature_bit_is_set) {
    NativeVolumeFeatures native;
    native.domain = VolumeFeatureDomain::DarwinVolumeCapabilities;
    native.value[0] = 0x40;
    CK_CHECK(advertised_feature(Observation<NativeVolumeFeatures>::available(native), VolumeFeature::SparseFiles).state() == ObservationState::Unknown);
    native.valid[0] = 0x40;
    CK_CHECK(*advertised_feature(Observation<NativeVolumeFeatures>::available(native), VolumeFeature::SparseFiles).value() == FeatureAdvertisement::Advertised);
    native.value[0] = 0;
    CK_CHECK(*advertised_feature(Observation<NativeVolumeFeatures>::available(native), VolumeFeature::SparseFiles).value() == FeatureAdvertisement::NotAdvertised);
    CK_CHECK(advertised_feature(Observation<NativeVolumeFeatures>::available(native), VolumeFeature::FileEncryption).state() == ObservationState::Unsupported);
}
CK_TEST(volume_advertisements_do_not_conflate_reparse_support_cloning_families_or_unknown_domains) {
    NativeVolumeFeatures native;
    native.domain = VolumeFeatureDomain::WindowsVolumeFlags;
    native.value[0] = 0x80; // Reparse-point support alone does not establish symbolic-link capability.
    native.valid[0] = 0xffffffff;
    const auto features = Observation<NativeVolumeFeatures>::available(native);
    CK_CHECK(advertised_feature(features, VolumeFeature::SymbolicLinks).state() == ObservationState::Unsupported);
    CK_CHECK(advertised_feature(features, VolumeFeature::NativeClones).state() == ObservationState::Unsupported);
    CK_CHECK(*advertised_feature(features, VolumeFeature::BlockRefcounting).value() == FeatureAdvertisement::NotAdvertised);
    CK_CHECK(advertised_feature(features, VolumeFeature::Unknown).state() == ObservationState::Failed);
    native.domain = static_cast<VolumeFeatureDomain>(255);
    CK_CHECK(advertised_feature(Observation<NativeVolumeFeatures>::available(native), VolumeFeature::SparseFiles).state() == ObservationState::Unknown);
}
CK_TEST(volume_advertisement_failures_preserve_the_query_error_and_do_not_become_known_false) {
    for (const auto state : {ObservationState::Denied, ObservationState::Failed, ObservationState::Changed, ObservationState::Unsupported}) {
        const NativeError error{ErrorDomain::Posix, 13, "injected.volume_query"};
        const auto result = advertised_feature(Observation<NativeVolumeFeatures>::unavailable(state, error), VolumeFeature::HardLinks);
        CK_CHECK(result.state() == state && result.unavailable_detail()->error == error && !result.value());
    }
}
CK_TEST(space_accounting_preserves_zero_and_rejects_negative_counts_zero_units_and_overflow) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    CK_CHECK(*detail::space_bytes(0, 4096).value() == 0);
    CK_CHECK(*detail::space_bytes(maximum, 1).value() == maximum);
    CK_CHECK(*detail::space_bytes(maximum / 4096, 4096).value() == (maximum / 4096) * 4096);
    CK_CHECK(detail::space_bytes(maximum / 4096 + 1, 4096).state() == ObservationState::Unknown);
    CK_CHECK(detail::space_bytes(0, 0).state() == ObservationState::Failed);
    CK_CHECK(detail::signed_space_bytes(-1, 4096).state() == ObservationState::Unknown);
    CK_CHECK(*detail::signed_space_bytes(1, 4096).value() == 4096);
}
CK_TEST(linux_directory_flags_decode_only_their_query_namespace_and_keep_clear_distinct_from_support) {
    LocationInfo location; location.family = LocationQueryFamily::LinuxDescriptor;
    for (auto feature : {LinuxDirectoryFeature::Casefold, LinuxDirectoryFeature::NoCow, LinuxDirectoryFeature::ProjectInherit}) {
        const auto bit = feature == LinuxDirectoryFeature::Casefold ? 0x40000000U : feature == LinuxDirectoryFeature::NoCow ? 0x00800000U : 0x20000000U;
        location.linux_directory_inode_flags = Observation<std::uint32_t>::available(bit | 0x80000000U);
        const auto set = reported_linux_directory_flag(location, feature);
        CK_CHECK(set.value() && *set.value() == ReportedFlag::Set);
        location.linux_directory_inode_flags = Observation<std::uint32_t>::available(~bit);
        const auto clear = reported_linux_directory_flag(location, feature);
        CK_CHECK(clear.value() && *clear.value() == ReportedFlag::Clear);
    }
    CK_CHECK(location.case_sensitive_directory.state() == ObservationState::Unknown);
    for (auto family : {LocationQueryFamily::Unknown, LocationQueryFamily::DarwinDescriptor, LocationQueryFamily::WindowsHandle}) {
        location.family = family;
        CK_CHECK(reported_linux_directory_flag(location, LinuxDirectoryFeature::Casefold).state() == ObservationState::Unsupported);
    }
    CK_CHECK(reported_linux_directory_flag(location, static_cast<LinuxDirectoryFeature>(999)).state() == ObservationState::Failed);
}
CK_TEST(linux_directory_flag_failures_retain_the_original_state_and_native_diagnostic) {
    LocationInfo location; location.family = LocationQueryFamily::LinuxDescriptor;
    const NativeError error{ErrorDomain::Posix, 25, "FS_IOC_GETFLAGS.directory"};
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
        ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        location.linux_directory_inode_flags = Observation<std::uint32_t>::unavailable(state, error);
        const auto result = reported_linux_directory_flag(location, LinuxDirectoryFeature::Casefold);
        CK_CHECK(result.state() == state && result.unavailable_detail() && result.unavailable_detail()->error == error);
    }
}
