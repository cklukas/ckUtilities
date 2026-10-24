// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_directory_assessment.hpp"
#include "pane_page_budget.hpp"
namespace ck::files {
static_assert([] {
    if (destination_advertised_features.size() != static_cast<std::size_t>(VolumeFeature::BlockRefcounting))
        return false;
    for (std::size_t i = 0; i < destination_advertised_features.size(); ++i)
        if (static_cast<std::size_t>(destination_advertised_features[i]) != i + 1)
            return false;
    return true;
}());
namespace {
template <class T> Observation<T> error(ObservationState state, AdapterError code, const char *op) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op});
}
template <class T> Observation<T> unavailable(const UnavailableObservation &e) {
    return Observation<T>::unavailable(e.state, e.error);
}
Observation<IdentityComparison> owner(const CompletedDirectoryInventory &inventory,
                                      const Observation<ObjectIdentity> &id) {
    auto compared = inventory.compare_owner(id);
    if (compared.value() && *compared.value() != IdentityComparison::Same)
        return error<IdentityComparison>(ObservationState::Changed, AdapterError::MetadataChanged,
                                         "destination_assessment.owner_changed");
    return compared;
}
template <class T> bool changed(const Observation<T> &a, const Observation<T> &b) {
    return a.value() && b.value() && *a.value() != *b.value();
}
bool valid_domain(const LocationInfo &info) {
    if (!info.features.value() || info.features.value()->domain == VolumeFeatureDomain::Unknown)
        return true;
    return (info.family == LocationQueryFamily::WindowsHandle &&
            info.features.value()->domain == VolumeFeatureDomain::WindowsVolumeFlags) ||
           (info.family == LocationQueryFamily::DarwinDescriptor &&
            info.features.value()->domain == VolumeFeatureDomain::DarwinVolumeCapabilities);
}
Observation<IdentityComparison> assess_context(const CompletedDirectoryInventory &inventory) {
    const auto &data = inventory.data();
    auto qualified = owner(inventory, data.anchor);
    if (!qualified.value())
        return qualified;
    for (const auto *observed : {&data.location_before, &data.location_after}) {
        if (!observed->value())
            return unavailable<IdentityComparison>(*observed->unavailable_detail());
        const auto &info = *observed->value();
        auto same = owner(inventory, info.directory);
        if (!same.value())
            return same;
        if (info.family == LocationQueryFamily::Unknown)
            return error<IdentityComparison>(ObservationState::Unknown, AdapterError::UnsupportedQuery,
                                             "destination_assessment.family_unknown");
        const auto scheme = data.anchor.value()->scheme;
        if ((info.family == LocationQueryFamily::WindowsHandle && scheme != IdentityScheme::WindowsFileId128 &&
             scheme != IdentityScheme::WindowsFileIndex64) ||
            (info.family != LocationQueryFamily::WindowsHandle && scheme != IdentityScheme::PosixDeviceInode))
            return error<IdentityComparison>(ObservationState::Failed, AdapterError::InvalidNativeResult,
                                             "destination_assessment.identity_family");
        if (!valid_domain(info))
            return error<IdentityComparison>(ObservationState::Failed, AdapterError::InvalidNativeResult,
                                             "destination_assessment.feature_domain");
    }
    const auto &a = *data.location_before.value(), &b = *data.location_after.value();
    if (a.family != b.family || changed(a.filesystem_type_code, b.filesystem_type_code) ||
        changed(a.filesystem_query_id, b.filesystem_query_id) ||
        changed(a.volume_query_serial, b.volume_query_serial) || changed(a.native_mount_flags, b.native_mount_flags) ||
        changed(a.case_sensitive_directory, b.case_sensitive_directory) ||
        changed(a.case_preserving_names, b.case_preserving_names) ||
        changed(a.windows_directory_case_flags, b.windows_directory_case_flags) ||
        changed(a.linux_directory_inode_flags, b.linux_directory_inode_flags) ||
        changed(data.before.value()->modified, data.after.value()->modified) ||
        changed(data.before.value()->changed, data.after.value()->changed))
        return error<IdentityComparison>(ObservationState::Changed, AdapterError::MetadataChanged,
                                         "destination_assessment.context_changed");
    return Observation<IdentityComparison>::available(IdentityComparison::Same);
}
Observation<bool> assess_read_only(const CompletedDirectoryInventory &inventory) {
    const auto &a = inventory.data().location_before.value()->read_only_mount;
    const auto &b = inventory.data().location_after.value()->read_only_mount;
    if (!a.value())
        return unavailable<bool>(*a.unavailable_detail());
    if (!b.value())
        return unavailable<bool>(*b.unavailable_detail());
    if (*a.value() != *b.value())
        return error<bool>(ObservationState::Changed, AdapterError::MetadataChanged,
                           "destination_assessment.read_only_changed");
    return Observation<bool>::available(*b.value());
}
Observation<FeatureAdvertisement> assess_feature(const CompletedDirectoryInventory &inventory,
                                                 VolumeFeature requested) {
    const auto &a = inventory.data().location_before.value()->features;
    const auto &b = inventory.data().location_after.value()->features;
    // Preserve a native query refusal before considering missing mappings.
    if (!a.value())
        return unavailable<FeatureAdvertisement>(*a.unavailable_detail());
    if (!b.value())
        return unavailable<FeatureAdvertisement>(*b.unavailable_detail());
    auto first = advertised_feature(a, requested), last = advertised_feature(b, requested);
    if (!first.value())
        return first;
    if (!last.value())
        return last;
    if (*first.value() != *last.value())
        return error<FeatureAdvertisement>(ObservationState::Changed, AdapterError::MetadataChanged,
                                           "destination_assessment.advertisement_changed");
    return last;
}
bool access_family(LocationQueryFamily location, DirectoryAccessFamily access) {
    return (location == LocationQueryFamily::LinuxDescriptor && access == DirectoryAccessFamily::LinuxFaccessat2) ||
           (location == LocationQueryFamily::DarwinDescriptor && access == DirectoryAccessFamily::DarwinFaccessat) ||
           (location == LocationQueryFamily::WindowsHandle && access == DirectoryAccessFamily::WindowsOpenById);
}
bool access_mask(const DirectoryAccessCheck &access) {
    if (access.family == DirectoryAccessFamily::WindowsOpenById) {
        // FILE_READ_ATTRIBUTES | SYNCHRONIZE plus the requested add right.
        const std::uint32_t required = 0x00100080U | (access.requested == DirectoryAccess::AddFile ? 2U : 4U);
        return (access.native_requested_mask & required) == required;
    }
    // Linux/Darwin W_OK | X_OK; an additional R_OK is still sufficient.
    return (access.native_requested_mask & 3U) == 3U && (access.native_requested_mask & ~7U) == 0;
}
Observation<DestinationCreationObservation> assess_creation(const CompletedDirectoryInventory &inventory,
                                                            const Observation<bool> &mount, DirectoryAccess requested) {
    using Result = Observation<DestinationCreationObservation>;
    const auto &access = inventory.data().access[static_cast<std::size_t>(requested)];
    if (!access.value())
        return unavailable<DestinationCreationObservation>(*access.unavailable_detail());
    auto checked = owner(inventory, Observation<ObjectIdentity>::available(access.value()->directory));
    if (!checked.value())
        return unavailable<DestinationCreationObservation>(*checked.unavailable_detail());
    if (access.value()->requested != requested ||
        !access_family(inventory.data().location_after.value()->family, access.value()->family) ||
        !access_mask(*access.value()))
        return error<DestinationCreationObservation>(ObservationState::Failed, AdapterError::InvalidNativeResult,
                                                     "destination_assessment.access_family");
    if (!mount.value())
        return unavailable<DestinationCreationObservation>(*mount.unavailable_detail());
    if (*mount.value())
        return Result::available(DestinationCreationObservation::ReadOnlyMountReported);
    return Result::available(DestinationCreationObservation::AccessReportedWithoutReadOnlyMount);
}
} // namespace
Observation<DestinationDirectoryAssessment::Pointer>
DestinationDirectoryAssessment::inspect(DirectoryInventoryJob::Result inventory, DestinationAssessmentLimits limits) {
    using Result = Observation<Pointer>;
    if (!inventory)
        return error<Pointer>(ObservationState::Failed, AdapterError::InvalidIdentifier,
                              "destination_assessment.null_inventory");
    if (!limits.variable_bytes || limits.variable_bytes > limits.hard_variable_bytes)
        return error<Pointer>(ObservationState::Failed, AdapterError::InvalidLimit, "destination_assessment.limits");
    auto result =
        std::unique_ptr<DestinationDirectoryAssessment>(new DestinationDirectoryAssessment(std::move(inventory)));
    const auto retain = [&]<class T>(Observation<T> value, Observation<T> &target) {
        if (!detail::charge(value, result->bytes_, limits.variable_bytes))
            return false;
        target = std::move(value);
        return true;
    };
    const auto exhausted = [] {
        return error<Pointer>(ObservationState::Failed, AdapterError::InventoryLimit,
                              "destination_assessment.variable_limit");
    };
    if (!retain(error<FeatureAdvertisement>(ObservationState::Failed, AdapterError::InvalidIdentifier,
                                            "destination_assessment.feature"),
                result->invalid_feature_) ||
        !retain(error<DestinationCreationObservation>(ObservationState::Failed, AdapterError::InvalidIdentifier,
                                                      "destination_assessment.creation"),
                result->invalid_creation_) ||
        !retain(assess_context(*result->inventory_), result->context_))
        return exhausted();
    if (!retain(result->context_.value() ? assess_read_only(*result->inventory_)
                                         : unavailable<bool>(*result->context_.unavailable_detail()),
                result->read_only_))
        return exhausted();
    for (std::size_t i = 0; i < destination_advertised_features.size(); ++i)
        if (!retain(result->context_.value()
                        ? assess_feature(*result->inventory_, destination_advertised_features[i])
                        : unavailable<FeatureAdvertisement>(*result->context_.unavailable_detail()),
                    result->features_[i]))
            return exhausted();
    for (std::size_t i = 0; i < result->creation_.size(); ++i)
        if (!retain(result->context_.value()
                        ? assess_creation(*result->inventory_, result->read_only_,
                                          i == 0 ? DirectoryAccess::AddFile : DirectoryAccess::AddDirectory)
                        : unavailable<DestinationCreationObservation>(*result->context_.unavailable_detail()),
                    result->creation_[i]))
            return exhausted();
    return Result::available(Pointer(std::move(result)));
}
const Observation<FeatureAdvertisement> &
DestinationDirectoryAssessment::advertisement(VolumeFeature requested) const noexcept {
    if (requested <= VolumeFeature::Unknown || requested > VolumeFeature::BlockRefcounting)
        return invalid_feature_;
    return features_[static_cast<std::size_t>(requested) - 1];
}
const Observation<DestinationCreationObservation> &
DestinationDirectoryAssessment::creation(DirectoryAccess requested) const noexcept {
    if (requested == DirectoryAccess::AddFile)
        return creation_[0];
    if (requested == DirectoryAccess::AddDirectory)
        return creation_[1];
    return invalid_creation_;
}
} // namespace ck::files
