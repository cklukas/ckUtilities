// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_capability_projection.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <utility>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation, ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

SourceCapabilityState unavailable_state(const UnavailableObservation& detail) noexcept {
    switch (detail.state) {
    case ObservationState::Denied: return SourceCapabilityState::Denied;
    case ObservationState::Changed: return SourceCapabilityState::Changed;
    case ObservationState::Unknown:
    case ObservationState::Gone:
    case ObservationState::Failed:
    case ObservationState::Unsupported:
    case ObservationState::Available: return SourceCapabilityState::Unknown;
    }
    return SourceCapabilityState::Unknown;
}

template <class T>
bool is_unavailable(const Observation<T>& value, ObservationState state) noexcept {
    return !value.value() && value.unavailable_detail()->state == state;
}

bool is_additional(const DataChannelInfo& channel) noexcept {
    return channel.kind == DataChannelKind::NamedData || channel.kind == DataChannelKind::ResourceFork ||
        channel.kind == DataChannelKind::Opaque;
}

VolumeFeature channel_destination(const DataChannelCatalog& catalog,
    SourceCapabilityProjectionPolicy policy) noexcept {
    switch (catalog.coverage) {
    case DataChannelCoverage::WindowsFileStreamInfo: return VolumeFeature::NamedStreams;
    case DataChannelCoverage::MacResourceFork: return VolumeFeature::ExtendedAttributes;
    case DataChannelCoverage::Unknown: return policy.unavailable_channel_destination;
    }
    return VolumeFeature::Unknown;
}

template <class T>
std::optional<SourceCapabilityState> unavailable_source(const Observation<T>& value) {
    if (value.value()) return std::nullopt;
    if (is_unavailable(value, ObservationState::Unsupported)) return std::nullopt;
    return unavailable_state(*value.unavailable_detail());
}

} // namespace

Observation<std::vector<SourceCapabilityEvidence>> SourceCapabilityProjection::from_inventory(
    std::size_t item, const ObjectInventoryData& inventory, SourceCapabilityProjectionPolicy policy,
    SourceCapabilityProjectionLimits limits) {
    using Result = Observation<std::vector<SourceCapabilityEvidence>>;
    if (!limits.max_evidence || limits.max_evidence > SourceCapabilityProjectionLimits::hard_max_evidence)
        return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InvalidLimit,
            "source_capability_projection.limits");

    std::vector<SourceCapabilityEvidence> result;
    result.reserve(std::min(limits.max_evidence, std::size_t{8}));
    const auto append = [&](SourceCapabilityEvidence evidence) -> bool {
        if (result.size() >= limits.max_evidence) return false;
        result.push_back(std::move(evidence));
        return true;
    };
    const auto append_unavailable = [&](PreservationFeature feature, PreservationConsequence consequence,
                                            VolumeFeature destination, const auto& observation, bool hard) -> bool {
        return append({item, feature, unavailable_state(*observation.unavailable_detail()), consequence,
            destination, hard});
    };

    // A channel catalog is only meaningful with an exact native query family.
    // An unavailable query therefore needs an explicit policy mapping; it must
    // never be guessed from the host platform or a filesystem label.
    if (const auto* channels = inventory.channels.value()) {
        const auto destination = channel_destination(*channels, policy);
        if (destination == VolumeFeature::Unknown)
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::UnsupportedQuery,
                "source_capability.channels_mapping");
        bool additional = false;
        for (const auto& channel : channels->channels)
            additional = additional || is_additional(channel);
        if (!additional) {
            if (!append({item, PreservationFeature::AdditionalChannels, SourceCapabilityState::Absent,
                    PreservationConsequence::DataLoss, destination, true}))
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                    "source_capability.evidence", ObservationState::Unknown);
        } else if (!append({item, PreservationFeature::AdditionalChannels, SourceCapabilityState::Present,
                       PreservationConsequence::DataLoss, destination, true})) {
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                "source_capability.evidence", ObservationState::Unknown);
        }
    } else if (const auto state = unavailable_source(inventory.channels); state) {
        const auto destination = policy.unavailable_channel_destination;
        if (destination == VolumeFeature::Unknown)
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::UnsupportedQuery,
                "source_capability.channels_mapping");
        if (!append({item, PreservationFeature::AdditionalChannels, *state, PreservationConsequence::DataLoss,
                destination, true}))
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                "source_capability.evidence", ObservationState::Unknown);
    }

    // Attribute values are part of preservation evidence: a readable catalog
    // with an unreadable value is not a complete Present observation.
    if (const auto* attributes = inventory.attributes.value()) {
        if (inventory.attribute_values.size() != attributes->attributes.size())
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InvalidNativeResult,
                "source_capability.attribute_values");
        const bool unknown_coverage = attributes->coverage == AttributeCoverage::Unknown;
        SourceCapabilityState state = unknown_coverage ? SourceCapabilityState::Unknown
            : attributes->attributes.empty() ? SourceCapabilityState::Absent : SourceCapabilityState::Present;
        bool hard = unknown_coverage;
        for (const auto& value : inventory.attribute_values) {
            if (value.catalog_index >= attributes->attributes.size())
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InvalidNativeResult,
                    "source_capability.attribute_index");
            if (!value.value.value()) {
                state = unavailable_state(*value.value.unavailable_detail());
                hard = true;
                break;
            }
            if (value.value.value()->identifier != attributes->attributes[value.catalog_index].identifier)
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InvalidNativeResult,
                    "source_capability.attribute_identifier");
        }
        if (!append({item, PreservationFeature::ExtendedAttributes, state, PreservationConsequence::MetadataLoss,
                VolumeFeature::ExtendedAttributes, hard}))
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                "source_capability.evidence", ObservationState::Unknown);
    } else if (const auto state = unavailable_source(inventory.attributes); state) {
        if (!append({item, PreservationFeature::ExtendedAttributes, *state, PreservationConsequence::MetadataLoss,
                VolumeFeature::ExtendedAttributes, true}))
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                "source_capability.evidence", ObservationState::Unknown);
    }

    // Security parts use one aggregate evidence record. A mixture of a
    // present ACL and an unknown sibling is hard Unknown, never silently
    // reduced to Present.
    SourceCapabilityState security_state = SourceCapabilityState::Absent;
    bool security_observed = false;
    bool security_hard = false;
    for (const auto& security : inventory.security) {
        if (security.value()) {
            security_observed = true;
            if (security.value()->presence == SecurityPresence::Present && security_state == SourceCapabilityState::Absent)
                security_state = SourceCapabilityState::Present;
            else if (security.value()->presence == SecurityPresence::Unknown) {
                security_state = SourceCapabilityState::Unknown;
                security_hard = true;
            }
            continue;
        }
        if (security.unavailable_detail()->state == ObservationState::Unsupported) continue;
        security_observed = true;
        const auto unavailable = unavailable_state(*security.unavailable_detail());
        if (security_state == SourceCapabilityState::Absent || security_state == SourceCapabilityState::Present)
            security_state = unavailable;
        else if (security_state != unavailable)
            security_state = SourceCapabilityState::Unknown;
        security_hard = true;
    }
    if (security_observed && !append({item, PreservationFeature::PermissionsOwnership, security_state,
            PreservationConsequence::SecurityChange, VolumeFeature::PersistentAcls, security_hard}))
        return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
            "source_capability.evidence", ObservationState::Unknown);

    // Link identity is represented by separate destination capabilities. A
    // known opaque reparse/mount-point kind has no exact mapping here and is
    // refused rather than relabelled as a symbolic link.
    if (const auto* link = inventory.link.value()) {
        if (std::holds_alternative<PosixSymlinkData>(link->payload)) {
            if (!append({item, PreservationFeature::LinksAndIdentity, SourceCapabilityState::Present,
                    PreservationConsequence::DataLoss, VolumeFeature::SymbolicLinks, true}))
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                    "source_capability.evidence", ObservationState::Unknown);
        } else if (const auto* windows = std::get_if<WindowsReparseData>(&link->payload)) {
            if (windows->kind != WindowsReparseKind::SymbolicLink)
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::UnsupportedQuery,
                    "source_capability.reparse_mapping");
            if (!append({item, PreservationFeature::LinksAndIdentity, SourceCapabilityState::Present,
                    PreservationConsequence::DataLoss, VolumeFeature::SymbolicLinks, true}))
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                    "source_capability.evidence", ObservationState::Unknown);
        }
    } else if (const auto state = unavailable_source(inventory.link); state) {
        if (!append({item, PreservationFeature::LinksAndIdentity, *state, PreservationConsequence::DataLoss,
                VolumeFeature::SymbolicLinks, true}))
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                "source_capability.evidence", ObservationState::Unknown);
    }

    if (const auto* before = inventory.before.value()) {
        if (const auto* links = before->link_count.value(); links && *links > 1) {
            if (!append({item, PreservationFeature::LinksAndIdentity, SourceCapabilityState::Present,
                    PreservationConsequence::DataLoss, VolumeFeature::HardLinks, true}))
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                    "source_capability.evidence", ObservationState::Unknown);
        } else if (!links) {
            const auto state = unavailable_source(before->link_count);
            if (state && !append({item, PreservationFeature::LinksAndIdentity, *state,
                    PreservationConsequence::DataLoss, VolumeFeature::HardLinks, true}))
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                    "source_capability.evidence", ObservationState::Unknown);
        }
    } else if (const auto state = unavailable_source(inventory.before); state) {
        if (!append({item, PreservationFeature::LinksAndIdentity, *state, PreservationConsequence::DataLoss,
                VolumeFeature::HardLinks, true}))
            return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                "source_capability.evidence", ObservationState::Unknown);
    }

    if (const auto* storage = inventory.storage.value()) {
        for (const auto& [flag, destination] : std::array{
                 std::pair{StorageFeature::SparseFlag, VolumeFeature::SparseFiles},
                 std::pair{StorageFeature::CompressionFlag, VolumeFeature::FileCompression},
                 std::pair{StorageFeature::EncryptionFlag, VolumeFeature::FileEncryption}}) {
            const auto reported = reported_storage_flag(*storage, flag);
            if (reported.value()) {
                if (*reported.value() != ReportedFlag::Set) continue;
                if (!append({item, PreservationFeature::StorageRepresentation, SourceCapabilityState::Present,
                        PreservationConsequence::RepresentationChange, destination, true}))
                    return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                        "source_capability.evidence", ObservationState::Unknown);
            } else if (reported.unavailable_detail()->state != ObservationState::Unsupported) {
                if (!append_unavailable(PreservationFeature::StorageRepresentation,
                        PreservationConsequence::RepresentationChange, destination, reported, true))
                    return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                        "source_capability.evidence", ObservationState::Unknown);
            }
        }
    } else if (const auto state = unavailable_source(inventory.storage); state) {
        for (const auto destination : {VolumeFeature::SparseFiles, VolumeFeature::FileCompression,
                                       VolumeFeature::FileEncryption})
            if (!append({item, PreservationFeature::StorageRepresentation, *state,
                    PreservationConsequence::RepresentationChange, destination, true}))
                return rejected<std::vector<SourceCapabilityEvidence>>(AdapterError::InventoryLimit,
                    "source_capability.evidence", ObservationState::Unknown);
    }

    return Result::available(std::move(result));
}

} // namespace ck::files
