// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_inventory_job.hpp"
namespace ck::files {
inline constexpr std::array destination_advertised_features{
    VolumeFeature::NamedStreams,       VolumeFeature::ExtendedAttributes, VolumeFeature::PersistentAcls,
    VolumeFeature::SparseFiles,        VolumeFeature::HardLinks,          VolumeFeature::SymbolicLinks,
    VolumeFeature::CaseSensitiveNames, VolumeFeature::CasePreservedNames, VolumeFeature::FileCompression,
    VolumeFeature::FileEncryption,     VolumeFeature::NativeClones,       VolumeFeature::BlockRefcounting};
enum class DestinationCreationObservation { ReadOnlyMountReported, AccessReportedWithoutReadOnlyMount };
struct DestinationAssessmentLimits {
    // All newly retained diagnostic strings; the already-owned input is separate.
    std::size_t variable_bytes = 1024 * 1024;
    static constexpr std::size_t hard_variable_bytes = 8 * 1024 * 1024;
};
// Worker-owned fixed-size assessment of native observations. Factory/getters do
// no I/O, probes, writes, path reopening, label-based inference or normalization.
// Advertised/NotAdvertised are not preservation or operation capabilities.
// AccessReportedWithoutReadOnlyMount says only those checks reported that way;
// names, flags, quota, inheritance, providers and execution still need checking.
class DestinationDirectoryAssessment final {
  public:
    using Pointer = std::shared_ptr<const DestinationDirectoryAssessment>;
    static Observation<Pointer> inspect(DirectoryInventoryJob::Result inventory,
                                        DestinationAssessmentLimits limits = {});
    const DirectoryInventoryJob::Result &inventory_for_worker() const noexcept {
        return inventory_;
    }
    const Observation<IdentityComparison> &context() const noexcept {
        return context_;
    }
    const Observation<bool> &read_only_mount() const noexcept {
        return read_only_;
    }
    const Observation<FeatureAdvertisement> &advertisement(VolumeFeature feature) const noexcept;
    const Observation<DestinationCreationObservation> &creation(DirectoryAccess requested) const noexcept;
    std::size_t variable_bytes() const noexcept {
        return bytes_;
    }

  private:
    explicit DestinationDirectoryAssessment(DirectoryInventoryJob::Result inventory)
        : inventory_(std::move(inventory)) {
    }
    DirectoryInventoryJob::Result inventory_;
    Observation<IdentityComparison> context_;
    Observation<bool> read_only_;
    std::array<Observation<FeatureAdvertisement>, destination_advertised_features.size()> features_;
    std::array<Observation<DestinationCreationObservation>, 2> creation_;
    Observation<FeatureAdvertisement> invalid_feature_;
    Observation<DestinationCreationObservation> invalid_creation_;
    std::size_t bytes_ = 0;
};
} // namespace ck::files
