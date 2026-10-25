// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_directory_assessment.hpp"
#include "preservation_capability_diff.hpp"
#include "manual_monotonic_clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <functional>
namespace {
using namespace ck::files;
template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("destination assessment fixture unavailable");
    return std::move(value).take_value();
}
struct Script {
    ObjectIdentity id{AdapterScope{},
                      IdentityScheme::WindowsFileId128,
                      NativeIdentifier::from_u64(7),
                      NativeIdentifier::from_u64(8),
                      {}};
    Observation<ObjectIdentity> anchor;
    BasicMetadata before, after;
    Observation<LocationInfo> first, last;
    std::array<Observation<DirectoryAccessCheck>, 4> access;
    std::shared_ptr<ck::testing::ManualMonotonicClock> clock = std::make_shared<ck::testing::ManualMonotonicClock>();
    int calls = 0, basics = 0, locations = 0, live = 0;
    explicit Script(LocationQueryFamily family = LocationQueryFamily::WindowsHandle) {
        if (family != LocationQueryFamily::WindowsHandle)
            id.scheme = IdentityScheme::PosixDeviceInode;
        anchor = Observation<ObjectIdentity>::available(id);
        before.identity = anchor;
        before.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
        after = before;
        LocationInfo info;
        info.family = family;
        info.directory = anchor;
        info.read_only_mount = Observation<bool>::available(false);
        info.filesystem_type_code = Observation<std::uint64_t>::available(0xef53);
        info.filesystem_query_id = Observation<NativeIdentifier>::available(NativeIdentifier::from_u64(3));
        info.volume_query_serial = Observation<std::uint64_t>::available(5);
        info.linux_directory_inode_flags = Observation<std::uint32_t>::available(0);
        NativeVolumeFeatures flags;
        flags.domain = family == LocationQueryFamily::WindowsHandle ? VolumeFeatureDomain::WindowsVolumeFlags
                                                                    : VolumeFeatureDomain::DarwinVolumeCapabilities;
        flags.valid.fill(0xffffffffU);
        flags.value[0] = family == LocationQueryFamily::WindowsHandle ? 0x00840008U : 0x00000040U;
        flags.value[1] = 0x00010000U;
        info.features = family == LocationQueryFamily::LinuxDescriptor
                            ? Observation<NativeVolumeFeatures>::unavailable(
                                  ObservationState::Unsupported, {ErrorDomain::Adapter, 4, "no.generic.linux.features"})
                            : Observation<NativeVolumeFeatures>::available(flags);
        first = Observation<LocationInfo>::available(info);
        last = first;
        const auto access_family = family == LocationQueryFamily::WindowsHandle ? DirectoryAccessFamily::WindowsOpenById
                                   : family == LocationQueryFamily::DarwinDescriptor
                                       ? DirectoryAccessFamily::DarwinFaccessat
                                       : DirectoryAccessFamily::LinuxFaccessat2;
        for (std::size_t i = 0; i < access.size(); ++i) {
            const auto requested = static_cast<DirectoryAccess>(i);
            const auto mask = family == LocationQueryFamily::WindowsHandle ? 0x00100080U | (i == 2   ? 2U
                                                                                            : i == 3 ? 4U
                                                                                                     : 1U)
                                                                           : 3U;
            access[i] = Observation<DirectoryAccessCheck>::available({requested, access_family, id, mask});
        }
    }
};
class Cursor final : public DirectoryCursor {
  public:
    explicit Cursor(std::shared_ptr<Script> s) : DirectoryCursor(s->anchor), s_(std::move(s)) {
        ++s_->live;
    }
    ~Cursor() override {
        --s_->live;
    }
    Observation<BasicMetadata> inspect_basic() const override {
        ++s_->calls;
        return Observation<BasicMetadata>::available(s_->basics++ ? s_->after : s_->before);
    }
    Observation<LocationInfo> inspect_location() const override {
        ++s_->calls;
        return s_->locations++ ? s_->last : s_->first;
    }
    Observation<DirectoryAccessCheck> check_access(DirectoryAccess requested) const override {
        ++s_->calls;
        return s_->access[static_cast<std::size_t>(requested)];
    }

  protected:
    Observation<DirectoryStep> next_step() override {
        throw std::runtime_error("assessment enumerated destination");
    }

  private:
    std::shared_ptr<Script> s_;
};
DirectoryInventoryJob::Result inventory(const std::shared_ptr<Script> &s) {
    DirectoryInventoryJob job(std::make_shared<Cursor>(s), {}, s->clock);
    for (int i = 0; i < 100; ++i) {
        if (job.advance() == DirectoryInventoryState::Complete)
            return take(job.completed());
        if (job.state() == DirectoryInventoryState::Failed)
            throw std::runtime_error("assessment inventory failed");
    }
    throw std::runtime_error("assessment inventory step budget");
}
DestinationDirectoryAssessment::Pointer assess(const std::shared_ptr<Script> &s) {
    return take(DestinationDirectoryAssessment::inspect(inventory(s)));
}
template <class F> void change_location(Observation<LocationInfo> &value, F change) {
    auto info = take(value);
    change(info);
    value = Observation<LocationInfo>::available(std::move(info));
}
template <class F> void change_features(Observation<LocationInfo> &value, F change) {
    change_location(value, [&](auto &info) {
        auto flags = take(info.features);
        change(flags);
        info.features = Observation<NativeVolumeFeatures>::available(flags);
    });
}
} // namespace
CK_TEST(destination_assessment_binds_feature_reports_without_IO_or_operation_permission) {
    auto s = std::make_shared<Script>();
    auto input = inventory(s);
    const auto calls = s->calls;
    auto report = take(DestinationDirectoryAssessment::inspect(input));
    CK_CHECK(s->calls == calls && report->inventory_for_worker() == input &&
             take(report->context()) == IdentityComparison::Same);
    CK_CHECK(take(report->advertisement(VolumeFeature::NamedStreams)) == FeatureAdvertisement::Advertised);
    CK_CHECK(take(report->advertisement(VolumeFeature::PersistentAcls)) == FeatureAdvertisement::Advertised);
    CK_CHECK(take(report->advertisement(VolumeFeature::SparseFiles)) == FeatureAdvertisement::NotAdvertised);
    CK_CHECK(report->advertisement(VolumeFeature::NativeClones).state() == ObservationState::Unsupported);
    CK_CHECK(take(report->creation(DirectoryAccess::AddFile)) ==
             DestinationCreationObservation::AccessReportedWithoutReadOnlyMount);
    input.reset();
    CK_CHECK(s->live == 1);
    report.reset();
    CK_CHECK(s->live == 0);
}

CK_TEST(destination_assessment_capability_snapshot_copies_only_published_feature_observations) {
    auto s = std::make_shared<Script>();
    const auto report = assess(s);
    const auto calls = s->calls;
    const auto snapshot = PreservationCapabilityDiff::from_destination_assessment(*report);
    CK_CHECK(snapshot.size() == destination_advertised_features.size() && s->calls == calls);
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
        CK_CHECK(snapshot[index].feature == destination_advertised_features[index]);
        CK_CHECK(snapshot[index].advertisement.state() == report->advertisement(snapshot[index].feature).state());
        if (snapshot[index].advertisement.value())
            CK_CHECK(*snapshot[index].advertisement.value() == *report->advertisement(snapshot[index].feature).value());
    }
}
CK_TEST(destination_assessment_darwin_and_linux_do_not_infer_features_from_filesystem_labels) {
    for (const auto family : {LocationQueryFamily::DarwinDescriptor, LocationQueryFamily::LinuxDescriptor}) {
        auto s = std::make_shared<Script>(family);
        for (auto *value : {&s->first, &s->last})
            change_location(*value, [](auto &info) {
                info.filesystem_name =
                    Observation<NativeFeatureIdentifier>::available(*NativeFeatureIdentifier::from_posix("NTFS"));
            });
        auto report = assess(s);
        CK_CHECK(take(report->context()) == IdentityComparison::Same);
        for (const auto feature : destination_advertised_features) {
            const auto expected = advertised_feature(s->first.value()->features, feature);
            const auto &actual = report->advertisement(feature);
            CK_CHECK(actual.state() == expected.state());
            if (expected.value())
                CK_CHECK(take(actual) == take(expected));
            else
                CK_CHECK(actual.unavailable_detail()->error == expected.unavailable_detail()->error);
        }
        CK_CHECK(take(report->creation(DirectoryAccess::AddDirectory)) ==
                 DestinationCreationObservation::AccessReportedWithoutReadOnlyMount);
    }
}
CK_TEST(destination_assessment_feature_bookends_check_each_mapping_and_validity_independently) {
    for (const bool validity : {false, true}) {
        auto s = std::make_shared<Script>();
        change_features(s->last, [&](auto &f) {
            if (validity)
                f.valid[0] &= ~0x00040000U;
            else
                f.value[0] ^= 0x00040000U;
        });
        auto report = assess(s);
        CK_CHECK(report->advertisement(VolumeFeature::NamedStreams).state() ==
                 (validity ? ObservationState::Unknown : ObservationState::Changed));
        CK_CHECK(take(report->advertisement(VolumeFeature::ExtendedAttributes)) == FeatureAdvertisement::Advertised);
        CK_CHECK(report->context().value());
    }
}
CK_TEST(destination_assessment_unavailable_feature_queries_preserve_states_and_native_codes) {
    for (const auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
                             ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        for (const bool after : {false, true}) {
            auto s = std::make_shared<Script>();
            const NativeError native{ErrorDomain::NtStatus, -9, "features.refused"};
            change_location(after ? s->last : s->first, [&](auto &info) {
                info.features = Observation<NativeVolumeFeatures>::unavailable(state, native);
            });
            auto report = assess(s);
            for (const auto feature : destination_advertised_features) {
                const auto &actual = report->advertisement(feature);
                CK_CHECK(actual.state() == state && actual.unavailable_detail()->error == native);
            }
            CK_CHECK(report->creation(DirectoryAccess::AddFile).value());
        }
    }
}
CK_TEST(destination_assessment_unqualified_or_conflicting_context_never_borrows_positive_flags) {
    for (int fault = 0; fault < 9; ++fault) {
        auto s = std::make_shared<Script>();
        if (fault == 0)
            s->anchor = {};
        if (fault == 1) {
            auto other = s->id;
            other.object = NativeIdentifier::from_u64(99);
            s->after.identity = Observation<ObjectIdentity>::available(other);
        }
        if (fault == 2 || fault == 3)
            change_location(fault == 2 ? s->first : s->last, [&](auto &info) {
                auto other = s->id;
                if (fault == 2)
                    other.scope = AdapterScope{};
                else
                    other.object = NativeIdentifier::from_u64(99);
                info.directory = Observation<ObjectIdentity>::available(other);
            });
        if (fault == 4)
            s->after.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        if (fault == 5)
            s->first = Observation<LocationInfo>::unavailable(ObservationState::Denied,
                                                              {ErrorDomain::Posix, 13, "location.denied"});
        if (fault == 6) {
            s->id.scheme = IdentityScheme::PosixDeviceInode;
            s->anchor = Observation<ObjectIdentity>::available(s->id);
            s->before.identity = s->anchor;
            s->after.identity = s->anchor;
            for (auto *loc : {&s->first, &s->last})
                change_location(*loc, [&](auto &info) { info.directory = s->anchor; });
        }
        if (fault == 7)
            change_location(s->first, [](auto &info) { info.family = LocationQueryFamily::Unknown; });
        if (fault == 8)
            change_features(s->last, [](auto &f) { f.domain = VolumeFeatureDomain::DarwinVolumeCapabilities; });
        auto report = assess(s);
        CK_CHECK(!report->context().value() && !report->read_only_mount().value());
        for (const auto feature : destination_advertised_features)
            CK_CHECK(!report->advertisement(feature).value());
        CK_CHECK(!report->creation(DirectoryAccess::AddFile).value());
        if (fault == 5)
            CK_CHECK(report->context().unavailable_detail()->error.code == 13);
    }
}
CK_TEST(destination_assessment_observed_location_or_metadata_contradictions_invalidate_context) {
    for (int fault = 0; fault < 10; ++fault) {
        auto s = std::make_shared<Script>();
        change_location(s->first, [](auto &info) {
            info.native_mount_flags = Observation<std::uint64_t>::available(0);
            info.case_sensitive_directory = Observation<bool>::available(false);
            info.case_preserving_names = Observation<bool>::available(true);
            info.windows_directory_case_flags = Observation<std::uint32_t>::available(0);
        });
        s->last = s->first;
        change_location(s->last, [&](auto &info) {
            if (fault == 0)
                info.filesystem_type_code = Observation<std::uint64_t>::available(2);
            if (fault == 1)
                info.filesystem_query_id = Observation<NativeIdentifier>::available(NativeIdentifier::from_u64(99));
            if (fault == 2)
                info.volume_query_serial = Observation<std::uint64_t>::available(99);
            if (fault == 3)
                info.native_mount_flags = Observation<std::uint64_t>::available(1);
            if (fault == 4)
                info.case_sensitive_directory = Observation<bool>::available(true);
            if (fault == 5)
                info.case_preserving_names = Observation<bool>::available(false);
            if (fault == 6)
                info.windows_directory_case_flags = Observation<std::uint32_t>::available(1);
            if (fault == 7)
                info.linux_directory_inode_flags = Observation<std::uint32_t>::available(1);
        });
        if (fault >= 8) {
            auto &a = fault == 8 ? s->before.modified : s->before.changed;
            auto &b = fault == 8 ? s->after.modified : s->after.changed;
            a = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(1, 0));
            b = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(2, 0));
        }
        auto report = assess(s);
        CK_CHECK(report->context().state() == ObservationState::Changed &&
                 !report->advertisement(VolumeFeature::NamedStreams).value());
    }
}
CK_TEST(destination_assessment_access_owner_family_and_required_mask_are_independently_qualified) {
    for (int fault = 0; fault < 5; ++fault) {
        auto s = std::make_shared<Script>();
        auto access = take(s->access[2]);
        if (fault == 0) {
            access.directory.object = NativeIdentifier::from_u64(99);
        }
        if (fault == 1)
            access.family = DirectoryAccessFamily::LinuxFaccessat2;
        if (fault == 2)
            access.native_requested_mask = 2;
        if (fault == 3)
            access.native_requested_mask = 0;
        if (fault == 4)
            access.native_requested_mask |= 0x100U;
        s->access[2] = Observation<DirectoryAccessCheck>::available(access);
        auto report = assess(s);
        CK_CHECK((report->creation(DirectoryAccess::AddFile).value() != nullptr) == (fault == 4));
        CK_CHECK(report->creation(DirectoryAccess::AddDirectory).value() &&
                 report->advertisement(VolumeFeature::NamedStreams).value());
    }
    for (const auto mask : {1U, 2U, 3U, 7U, 0x13U}) {
        auto s = std::make_shared<Script>(LocationQueryFamily::LinuxDescriptor);
        auto a = take(s->access[2]);
        a.native_requested_mask = mask;
        s->access[2] = Observation<DirectoryAccessCheck>::available(a);
        CK_CHECK((assess(s)->creation(DirectoryAccess::AddFile).value() != nullptr) == (mask == 3 || mask == 7));
    }
}
CK_TEST(destination_assessment_read_only_uncertainty_changes_and_access_denial_stay_distinct) {
    for (int fault = 0; fault < 5; ++fault) {
        auto s = std::make_shared<Script>();
        if (fault == 0)
            for (auto *value : {&s->first, &s->last})
                change_location(*value, [](auto &info) { info.read_only_mount = Observation<bool>::available(true); });
        if (fault == 1)
            change_location(s->first, [](auto &info) { info.read_only_mount = {}; });
        if (fault == 2)
            change_location(s->last, [](auto &info) { info.read_only_mount = Observation<bool>::available(true); });
        if (fault >= 3) {
            if (fault == 3)
                change_location(s->first, [](auto &info) { info.read_only_mount = {}; });
            else
                for (auto *location : {&s->first, &s->last})
                    change_location(*location,
                                    [](auto &info) { info.read_only_mount = Observation<bool>::available(true); });
            s->access[2] = Observation<DirectoryAccessCheck>::unavailable(ObservationState::Denied,
                                                                          {ErrorDomain::Posix, 13, "access.denied"});
        }
        auto report = assess(s);
        if (fault == 0)
            CK_CHECK(take(report->creation(DirectoryAccess::AddFile)) ==
                     DestinationCreationObservation::ReadOnlyMountReported);
        if (fault == 1)
            CK_CHECK(report->creation(DirectoryAccess::AddFile).state() == ObservationState::Unknown);
        if (fault == 2)
            CK_CHECK(report->creation(DirectoryAccess::AddFile).state() == ObservationState::Changed);
        if (fault >= 3)
            CK_CHECK(report->creation(DirectoryAccess::AddFile).state() == ObservationState::Denied &&
                     report->creation(DirectoryAccess::AddFile).unavailable_detail()->error.code == 13);
        if (fault == 4)
            CK_CHECK(take(report->read_only_mount()));
        CK_CHECK(report->advertisement(VolumeFeature::NamedStreams).value());
    }
}
CK_TEST(destination_assessment_exact_text_budget_invalid_requests_and_factory_inputs_do_no_IO) {
    auto s = std::make_shared<Script>();
    for (auto *value : {&s->first, &s->last})
        change_location(*value, [](auto &info) {
            info.features = Observation<NativeVolumeFeatures>::unavailable(
                ObservationState::Denied, {ErrorDomain::Posix, 13, std::string(200, 'x')});
        });
    auto input = inventory(s);
    const auto calls = s->calls;
    auto report = take(DestinationDirectoryAssessment::inspect(input));
    const auto bytes = report->variable_bytes();
    CK_CHECK(DestinationDirectoryAssessment::inspect(input, {bytes}).value());
    const auto short_report = DestinationDirectoryAssessment::inspect(input, {bytes - 1});
    CK_CHECK(!short_report.value() &&
             short_report.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
    CK_CHECK(!DestinationDirectoryAssessment::inspect({}).value() &&
             !DestinationDirectoryAssessment::inspect(input, {0}).value());
    CK_CHECK(!DestinationDirectoryAssessment::inspect(input, {DestinationAssessmentLimits::hard_variable_bytes + 1})
                  .value());
    CK_CHECK(report->advertisement(VolumeFeature::Unknown).state() == ObservationState::Failed);
    CK_CHECK(report->advertisement(static_cast<VolumeFeature>(99)).state() == ObservationState::Failed);
    CK_CHECK(report->creation(DirectoryAccess::List).state() == ObservationState::Failed &&
             report->creation(static_cast<DirectoryAccess>(99)).state() == ObservationState::Failed);
    CK_CHECK(s->calls == calls && report->variable_bytes() == bytes);
}
