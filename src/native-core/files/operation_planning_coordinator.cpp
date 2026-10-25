// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_planning_coordinator.hpp"

namespace ck::files {

Observation<OperationPlan::Pointer> OperationPlanningCoordinator::assemble(FileOperation operation,
    std::shared_ptr<const FrozenInventory> inventory,
    std::span<const SourceTargetInspection> source_target,
    std::span<const Observation<AncestorWalkWitness>> ancestry,
    std::span<const ObjectInventoryData> source_objects,
    const CompletedDestinationBatch& destination_batch,
    std::span<const DestinationCapabilityEvidence> destination_capabilities,
    SourceCapabilityProjectionPolicy projection_policy, OperationPlanningCoordinatorLimits limits) {
    using Result = Observation<OperationPlan::Pointer>;
    if (!inventory)
        return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::PlanInvalid),
                "operation_planning_coordinator.inventory"});
    auto topologies = OperationTopologyProjection::from_worker(source_target, ancestry, limits.topology);
    if (!topologies.value())
        return Result::unavailable(topologies.unavailable_detail()->state, topologies.unavailable_detail()->error);
    auto conflicts = DestinationConflictProjection::from_batch(*inventory, destination_batch, limits.conflicts);
    if (!conflicts.value())
        return Result::unavailable(conflicts.unavailable_detail()->state, conflicts.unavailable_detail()->error);
    return OperationEvidenceAssembly::assemble(operation, std::move(inventory), *topologies.value(), source_objects,
        destination_capabilities, std::move(conflicts).take_value(), projection_policy, limits.evidence);
}

} // namespace ck::files
