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
