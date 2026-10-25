// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_evidence_assembly.hpp"

#include "cvision/testing/cktest.hpp"

#include <array>
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

FrozenInventory::Pointer inventory_with_two_entries() {
    FrozenInventoryBuilder builder(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    const AdapterScope scope;
    const auto add = [&](const char* text, std::uint64_t object) {
        auto path = NativeAbsolutePath::from_posix(text);
        if (!path.value()) throw std::runtime_error("operation evidence path fixture unavailable");
        return builder.add({std::move(path).take_value(),
            {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
                NativeIdentifier::from_u64(object), {}}, ObjectKind::RegularFile,
            Observation<std::uint64_t>::available(4), 1, LinkTraversalEvidence::NotLink, {}});
    };
    if (!add("/source/a", 7) || !add("/source/b", 8) || !builder.seal())
        throw std::runtime_error("operation evidence inventory fixture unavailable");
    return std::move(builder.completed()).take_value();
}

DestinationCapabilityEvidence advertised(VolumeFeature feature, FeatureAdvertisement value) {
    return {feature, Observation<FeatureAdvertisement>::available(value)};
}

} // namespace

CK_TEST(operation_evidence_assembly_projects_each_object_before_plan_creation) {
    const auto inventory = inventory_with_two_entries();
    std::array objects{empty_inventory(), empty_inventory()};
    StorageInfo storage;
    storage.model = StorageModel::WindowsHandleAttributes;
    storage.flags = Observation<NativeStorageFlags>::available({0x200, 0xffff});
    objects[0].storage = Observation<StorageInfo>::available(std::move(storage));
    const std::array topologies{SourceTargetTopology::Disjoint, SourceTargetTopology::Disjoint};
    const std::array destination{advertised(VolumeFeature::SparseFiles, FeatureAdvertisement::NotAdvertised)};

    const auto plan = OperationEvidenceAssembly::assemble(FileOperation::Copy, inventory, topologies, objects,
        destination);
    CK_CHECK(plan.value());
    CK_CHECK((*plan.value())->items().size() == 2 && (*plan.value())->issues().size() == 1);
    CK_CHECK((*plan.value())->issues().front().item == 0 &&
        (*plan.value())->issues().front().feature == PreservationFeature::StorageRepresentation);
}

CK_TEST(operation_evidence_assembly_propagates_projection_failures_and_shape_errors) {
    const auto inventory = inventory_with_two_entries();
    const std::array topologies{SourceTargetTopology::Disjoint, SourceTargetTopology::Disjoint};
    const std::array one_object{empty_inventory()};
    const auto wrong_shape = OperationEvidenceAssembly::assemble(FileOperation::Copy, inventory, topologies,
        one_object, {});
    CK_CHECK(wrong_shape.state() == ObservationState::Failed &&
        wrong_shape.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));

    auto unmappable = empty_inventory();
    unmappable.channels = Observation<DataChannelCatalog>::available({{}, DataChannelCoverage::Unknown, {}});
    const std::array objects{unmappable, empty_inventory()};
    const auto failed_projection = OperationEvidenceAssembly::assemble(FileOperation::Copy, inventory, topologies,
        objects, {});
    CK_CHECK(failed_projection.state() == ObservationState::Failed &&
        failed_projection.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::UnsupportedQuery));
}

CK_TEST(operation_evidence_assembly_enforces_total_evidence_limit_before_plan_creation) {
    const auto inventory = inventory_with_two_entries();
    const std::array topologies{SourceTargetTopology::Disjoint, SourceTargetTopology::Disjoint};
    const std::array objects{empty_inventory(), empty_inventory()};
    OperationEvidenceAssemblyLimits limits;
    limits.max_source_evidence = 0;
    const auto invalid_limit = OperationEvidenceAssembly::assemble(FileOperation::Copy, inventory, topologies,
        objects, {}, {}, {}, limits);
    CK_CHECK(invalid_limit.state() == ObservationState::Failed &&
        invalid_limit.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));
}
