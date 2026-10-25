// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_planning_coordinator.hpp"

#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;
using FS = ck::testing::SimulatedFileSystem;

template <class T>
T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("planning coordinator fixture unavailable");
    return std::move(value).take_value();
}

NativeName name(const char* text) { return *NativeName::from_posix(text); }

std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/planning-coordinator";
#else
    return "/planning-coordinator";
#endif
}

FrozenInventory::Pointer inventory() {
    FrozenInventoryBuilder builder(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    const AdapterScope scope;
    auto source = NativeAbsolutePath::from_posix("/source/item");
    if (!source.value() || !builder.add({std::move(source).take_value(),
            {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
                NativeIdentifier::from_u64(1), {}}, ObjectKind::RegularFile,
            Observation<std::uint64_t>::available(1), 1, LinkTraversalEvidence::NotLink, {}}) ||
        !builder.seal())
        throw std::runtime_error("planning coordinator inventory unavailable");
    return take(builder.completed());
}

ObjectInventoryData object_inventory() {
    ObjectInventoryData result;
    const auto unsupported = []<class T>() {
        return Observation<T>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "fixture.unsupported"});
    };
    result.before = unsupported.template operator()<BasicMetadata>();
    result.after = unsupported.template operator()<BasicMetadata>();
    result.storage = unsupported.template operator()<StorageInfo>();
    result.integrity = unsupported.template operator()<FileIntegrityInfo>();
    result.inode_flags = unsupported.template operator()<FileInodeFlags>();
    result.layout = unsupported.template operator()<FileDataLayout>();
    result.link = unsupported.template operator()<LinkMetadata>();
    result.link_limits = unsupported.template operator()<FileLinkLimits>();
    result.channels = unsupported.template operator()<DataChannelCatalog>();
    result.attributes = unsupported.template operator()<AttributeCatalog>();
    for (auto& security : result.security) security = unsupported.template operator()<NativeSecurityValue>();
    return result;
}

struct Fixture final {
    FS fs;
    FS::Pointer destination = fs.make_node(ObjectKind::Directory);
    std::shared_ptr<const DirectoryCursor> cursor;

    Fixture() {
        fs.bind(path(), destination);
        cursor = std::shared_ptr<const DirectoryCursor>(take(fs.open_directory(path())));
    }

    DestinationBatchJob::Result batch() {
        DestinationBatchJob job(cursor, {name("missing")}, NameEncoding::PosixBytes, {}, fs.clock());
        for (int step = 0; step < 200; ++step)
            if (job.advance() == DestinationBatchState::Complete) return take(job.completed());
        throw std::runtime_error("planning coordinator batch exceeded step bound");
    }
};
} // namespace

CK_TEST(operation_planning_coordinator_joins_topology_conflict_and_capability_evidence) {
    Fixture f;
    const auto plan_inventory = inventory();
    auto object = object_inventory();
    const std::array objects{object};
    BasicMetadata source_metadata;
    source_metadata.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
    const std::array source_target{SourceTargetInspection{
        Observation<BasicMetadata>::available(source_metadata), {},
        Observation<IdentityComparison>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable),
                "source_target.no_existing_target"})}};
    const std::array ancestry{Observation<AncestorWalkWitness>::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "fixture.not_needed"})};
    const std::array destination_capabilities{DestinationCapabilityEvidence{
        VolumeFeature::SparseFiles, Observation<FeatureAdvertisement>::available(FeatureAdvertisement::Advertised)}};
    const auto plan = OperationPlanningCoordinator::assemble(FileOperation::Copy, plan_inventory, source_target,
        ancestry, objects, *f.batch(), destination_capabilities);
    CK_CHECK(plan.value() && (*plan.value())->items().size() == 1 &&
        (*plan.value())->items().front().topology == SourceTargetTopology::Disjoint &&
        (*plan.value())->conflicts().empty() && (*plan.value())->issues().empty());
}

CK_TEST(operation_planning_coordinator_rejects_unknown_topology_before_plan_creation) {
    Fixture f;
    const auto plan_inventory = inventory();
    const std::array objects{object_inventory()};
    BasicMetadata source_metadata;
    source_metadata.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
    const std::array source_target{SourceTargetInspection{
        Observation<BasicMetadata>::available(source_metadata), {},
        Observation<IdentityComparison>::available(IdentityComparison::Different)}};
    const std::array ancestry{Observation<AncestorWalkWitness>::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "fixture.unknown"})};
    const auto result = OperationPlanningCoordinator::assemble(FileOperation::Copy, plan_inventory, source_target,
        ancestry, objects, *f.batch(), {});
    CK_CHECK(result.state() == ObservationState::Failed &&
        result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanTopologyUnsafe));
}
