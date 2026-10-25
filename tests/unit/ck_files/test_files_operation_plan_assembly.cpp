// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_plan_assembly.hpp"

#include "cvision/testing/cktest.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;

FrozenInventory::Pointer inventory_with_aliases() {
    FrozenInventoryBuilder builder(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    const AdapterScope scope;
    const auto add = [&](const char* text, std::uint64_t object) {
        auto path = NativeAbsolutePath::from_posix(text);
        if (!path.value()) throw std::runtime_error("operation assembly path fixture unavailable");
        return builder.add({std::move(path).take_value(),
            {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
                NativeIdentifier::from_u64(object), {}}, ObjectKind::RegularFile,
            Observation<std::uint64_t>::available(4), 1, LinkTraversalEvidence::NotLink, {}});
    };
    if (!add("/source/a", 7) || !add("/source/alias", 7) || !builder.seal())
        throw std::runtime_error("operation assembly inventory fixture unavailable");
    return std::move(builder.completed()).take_value();
}

DestinationCapabilityEvidence advertised(VolumeFeature feature, FeatureAdvertisement value) {
    return {feature, Observation<FeatureAdvertisement>::available(value)};
}
} // namespace

CK_TEST(operation_plan_assembly_binds_inventory_capabilities_and_conflicts) {
    const auto inventory = inventory_with_aliases();
    const std::array topologies{SourceTargetTopology::Disjoint, SourceTargetTopology::Disjoint};
    const std::vector source{
        SourceCapabilityEvidence{0, PreservationFeature::StorageRepresentation, SourceCapabilityState::Present,
            PreservationConsequence::RepresentationChange, VolumeFeature::SparseFiles, true},
        SourceCapabilityEvidence{0, PreservationFeature::StorageRepresentation, SourceCapabilityState::Present,
            PreservationConsequence::RepresentationChange, VolumeFeature::FileCompression, true}};
    const std::vector destination{
        advertised(VolumeFeature::SparseFiles, FeatureAdvertisement::Advertised),
        advertised(VolumeFeature::FileCompression, FeatureAdvertisement::NotAdvertised)};
    const auto plan = OperationPlanAssembly::assemble(FileOperation::Copy, inventory, topologies, source,
        destination, {{1, DestinationConflictKind::ExistingFile}});
    CK_CHECK(plan.value());
    CK_CHECK((*plan.value())->items().size() == 2 && (*plan.value())->items()[0].hard_link_group &&
        (*plan.value())->items()[1].hard_link_group &&
        *(*plan.value())->items()[0].hard_link_group == *(*plan.value())->items()[1].hard_link_group);
    CK_CHECK((*plan.value())->issues().size() == 1 &&
        (*plan.value())->issues().front().feature == PreservationFeature::StorageRepresentation &&
        (*plan.value())->issues().front().state == PreservationIssueState::Missing);
    CK_CHECK((*plan.value())->conflicts().size() == 1 && (*plan.value())->conflicts().front().item == 1);
}

CK_TEST(operation_plan_assembly_rejects_foreign_shapes_and_preserves_diff_failures) {
    const auto inventory = inventory_with_aliases();
    const std::array topologies{SourceTargetTopology::Disjoint};
    const std::vector<SourceCapabilityEvidence> source;
    const std::vector<DestinationCapabilityEvidence> destination;
    const auto wrong_topology = OperationPlanAssembly::assemble(FileOperation::Copy, inventory, topologies,
        source, destination);
    CK_CHECK(wrong_topology.state() == ObservationState::Failed &&
        wrong_topology.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));

    const std::array complete{SourceTargetTopology::Disjoint, SourceTargetTopology::Disjoint};
    const std::vector invalid_source{SourceCapabilityEvidence{2, PreservationFeature::AdditionalChannels,
        SourceCapabilityState::Present, PreservationConsequence::DataLoss, VolumeFeature::NamedStreams, true}};
    const auto foreign_source = OperationPlanAssembly::assemble(FileOperation::Copy, inventory, complete,
        invalid_source, destination);
    CK_CHECK(foreign_source.state() == ObservationState::Failed &&
        foreign_source.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));

    const std::vector invalid_destination{
        advertised(VolumeFeature::SparseFiles, FeatureAdvertisement::Advertised),
        advertised(VolumeFeature::SparseFiles, FeatureAdvertisement::Advertised)};
    const auto invalid_diff = OperationPlanAssembly::assemble(FileOperation::Copy, inventory, complete, source,
        invalid_destination);
    CK_CHECK(invalid_diff.state() == ObservationState::Failed &&
        invalid_diff.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));
}

CK_TEST(operation_plan_assembly_rejects_invalid_limits_before_assembly) {
    const auto inventory = inventory_with_aliases();
    const std::array topologies{SourceTargetTopology::Disjoint, SourceTargetTopology::Disjoint};
    const std::vector<SourceCapabilityEvidence> source;
    const std::vector<DestinationCapabilityEvidence> destination;
    OperationPlanAssemblyLimits limits;
    limits.preservation.max_issues = 0;
    const auto result = OperationPlanAssembly::assemble(FileOperation::Copy, inventory, topologies, source,
        destination, {}, limits);
    CK_CHECK(result.state() == ObservationState::Failed &&
        result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));
}
