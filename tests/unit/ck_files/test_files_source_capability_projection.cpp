// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_capability_projection.hpp"

#include "cvision/testing/cktest.hpp"

#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;

template <class T>
Observation<T> unsupported() {
    return Observation<T>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "fixture.unsupported"});
}

ObjectInventoryData empty_inventory() {
    ObjectInventoryData result;
    result.before = unsupported<BasicMetadata>();
    result.after = unsupported<BasicMetadata>();
    result.storage = unsupported<StorageInfo>();
    result.integrity = unsupported<FileIntegrityInfo>();
    result.inode_flags = unsupported<FileInodeFlags>();
    result.layout = unsupported<FileDataLayout>();
    result.link = unsupported<LinkMetadata>();
    result.link_limits = unsupported<FileLinkLimits>();
    result.channels = unsupported<DataChannelCatalog>();
    result.attributes = unsupported<AttributeCatalog>();
    for (auto& security : result.security)
        security = unsupported<NativeSecurityValue>();
    return result;
}

std::vector<SourceCapabilityEvidence> take(Observation<std::vector<SourceCapabilityEvidence>> value) {
    if (!value.value()) throw std::runtime_error("source capability projection fixture unavailable");
    return std::move(value).take_value();
}

const SourceCapabilityEvidence* find(const std::vector<SourceCapabilityEvidence>& values,
    PreservationFeature feature, VolumeFeature destination) {
    for (const auto& value : values)
        if (value.feature == feature && value.destination_feature == destination)
            return &value;
    return nullptr;
}
} // namespace

CK_TEST(source_capability_projection_maps_complete_channel_attribute_and_storage_observations) {
    auto inventory = empty_inventory();
    const auto identifier = NativeFeatureIdentifier::from_posix("user.example");
    CK_CHECK(identifier.has_value());
    inventory.channels = Observation<DataChannelCatalog>::available({
        {{*identifier, DataChannelKind::NamedData, Observation<std::uint64_t>::available(4),
            Observation<std::uint64_t>::available(4)}},
        DataChannelCoverage::WindowsFileStreamInfo, {}});
    inventory.attributes = Observation<AttributeCatalog>::available({
        {{*identifier, AttributeRole::Opaque, Observation<std::uint64_t>::available(2),
            Observation<std::uint32_t>::available(0)}},
        AttributeCoverage::WindowsExtendedAttributes, {}});
    inventory.attribute_values.push_back({0, Observation<AttributeValue>::available({*identifier, {}, {},
        Observation<std::uint32_t>::available(0)})});
    StorageInfo storage;
    storage.model = StorageModel::WindowsHandleAttributes;
    storage.flags = Observation<NativeStorageFlags>::available({0x200 | 0x800 | 0x4000, 0xffff});
    inventory.storage = Observation<StorageInfo>::available(std::move(storage));

    const auto evidence = take(SourceCapabilityProjection::from_inventory(7, inventory));
    CK_CHECK(evidence.size() == 5);
    const auto* channels = find(evidence, PreservationFeature::AdditionalChannels, VolumeFeature::NamedStreams);
    const auto* attributes = find(evidence, PreservationFeature::ExtendedAttributes, VolumeFeature::ExtendedAttributes);
    const auto* sparse = find(evidence, PreservationFeature::StorageRepresentation, VolumeFeature::SparseFiles);
    const auto* compression = find(evidence, PreservationFeature::StorageRepresentation, VolumeFeature::FileCompression);
    const auto* encryption = find(evidence, PreservationFeature::StorageRepresentation, VolumeFeature::FileEncryption);
    CK_CHECK(channels && channels->state == SourceCapabilityState::Present && channels->hard_if_missing);
    CK_CHECK(attributes && attributes->state == SourceCapabilityState::Present);
    CK_CHECK(sparse && compression && encryption);
    CK_CHECK(sparse->state == SourceCapabilityState::Present && compression->state == SourceCapabilityState::Present &&
        encryption->state == SourceCapabilityState::Present);
}

CK_TEST(source_capability_projection_does_not_treat_directory_link_count_as_hard_links) {
    auto inventory = empty_inventory();
    BasicMetadata before;
    before.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
    before.link_count = Observation<std::uint64_t>::available(3);
    inventory.before = Observation<BasicMetadata>::available(std::move(before));

    const auto evidence = take(SourceCapabilityProjection::from_inventory(0, inventory));
    CK_CHECK(find(evidence, PreservationFeature::LinksAndIdentity, VolumeFeature::HardLinks) == nullptr);
}

CK_TEST(source_capability_projection_keeps_relevant_unknowns_hard_and_omits_unsupported_families) {
    auto inventory = empty_inventory();
    inventory.channels = Observation<DataChannelCatalog>::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, 41, "fixture.channels"});
    inventory.storage = Observation<StorageInfo>::unavailable(ObservationState::Denied,
        {ErrorDomain::Adapter, 42, "fixture.storage"});
    const auto evidence = take(SourceCapabilityProjection::from_inventory(3, inventory,
        {.unavailable_channel_destination = VolumeFeature::NamedStreams}));
    CK_CHECK(evidence.size() == 4);
    const auto* channels = find(evidence, PreservationFeature::AdditionalChannels, VolumeFeature::NamedStreams);
    CK_CHECK(channels && channels->state == SourceCapabilityState::Unknown && channels->hard_if_missing);
    for (const auto destination : {VolumeFeature::SparseFiles, VolumeFeature::FileCompression,
                                   VolumeFeature::FileEncryption}) {
        const auto* value = find(evidence, PreservationFeature::StorageRepresentation, destination);
        CK_CHECK(value && value->state == SourceCapabilityState::Denied && value->hard_if_missing);
    }
    CK_CHECK(find(evidence, PreservationFeature::ExtendedAttributes, VolumeFeature::ExtendedAttributes) == nullptr);
}

CK_TEST(source_capability_projection_refuses_unmappable_or_inconsistent_inventory) {
    auto inventory = empty_inventory();
    inventory.channels = Observation<DataChannelCatalog>::available({{}, DataChannelCoverage::Unknown, {}});
    const auto unknown_channels = SourceCapabilityProjection::from_inventory(0, inventory);
    CK_CHECK(unknown_channels.state() == ObservationState::Failed &&
        unknown_channels.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::UnsupportedQuery));

    inventory = empty_inventory();
    inventory.attributes = Observation<AttributeCatalog>::available({{}, AttributeCoverage::Unknown, {}});
    const auto identifier = NativeFeatureIdentifier::from_posix("user.example");
    CK_CHECK(identifier.has_value());
    inventory.attributes = Observation<AttributeCatalog>::available({
        {{*identifier, AttributeRole::Opaque, Observation<std::uint64_t>::available(1),
            Observation<std::uint32_t>::available(0)}}, AttributeCoverage::Unknown, {}});
    const auto malformed_values = SourceCapabilityProjection::from_inventory(0, inventory);
    CK_CHECK(malformed_values.state() == ObservationState::Failed &&
        malformed_values.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidNativeResult));

    inventory.attribute_values.push_back({1, Observation<AttributeValue>::available({*identifier, {}, {},
        Observation<std::uint32_t>::available(0)})});
    const auto malformed_index = SourceCapabilityProjection::from_inventory(0, inventory);
    CK_CHECK(malformed_index.state() == ObservationState::Failed &&
        malformed_index.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidNativeResult));

    inventory = empty_inventory();
    inventory.link = Observation<LinkMetadata>::available({{}, WindowsReparseData{1,
        WindowsReparseKind::MountPoint, {}, std::nullopt, std::nullopt}});
    const auto opaque_link = SourceCapabilityProjection::from_inventory(0, inventory);
    CK_CHECK(opaque_link.state() == ObservationState::Failed &&
        opaque_link.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::UnsupportedQuery));
}

CK_TEST(source_capability_projection_rejects_invalid_limits_before_reading_values) {
    const auto result = SourceCapabilityProjection::from_inventory(0, empty_inventory(), {}, {0});
    CK_CHECK(result.state() == ObservationState::Failed &&
        result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));
}
