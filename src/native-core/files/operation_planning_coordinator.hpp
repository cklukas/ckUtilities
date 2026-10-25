// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "destination_conflict_projection.hpp"
#include "operation_evidence_assembly.hpp"
#include "operation_topology_projection.hpp"

#include <memory>
#include <span>

namespace ck::files {

struct OperationPlanningCoordinatorLimits final {
    OperationEvidenceAssemblyLimits evidence;
    DestinationConflictProjectionLimits conflicts;
    OperationTopologyProjectionLimits topology;
};

// Joins completed worker observations into one immutable plan. All inputs are
// already-published values; this coordinator performs no native I/O, path
// reopening, probing, normalization or mutation. A plan receipt still needs
// the worker-owned execution-time revalidation boundary before authorization.
class OperationPlanningCoordinator final {
public:
    static Observation<OperationPlan::Pointer> assemble(FileOperation operation,
        std::shared_ptr<const FrozenInventory> inventory,
        std::span<const SourceTargetInspection> source_target,
        std::span<const Observation<AncestorWalkWitness>> ancestry,
        std::span<const ObjectInventoryData> source_objects,
        const CompletedDestinationBatch& destination_batch,
        std::span<const DestinationCapabilityEvidence> destination_capabilities,
        SourceCapabilityProjectionPolicy projection_policy = {},
        OperationPlanningCoordinatorLimits limits = {});
};

} // namespace ck::files
