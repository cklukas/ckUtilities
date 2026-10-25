// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_plan_assembly.hpp"
#include "source_capability_projection.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace ck::files {

struct OperationEvidenceAssemblyLimits final {
    OperationPlanAssemblyLimits plan;
    SourceCapabilityProjectionLimits projection;
    std::size_t max_source_evidence = 4'000'000;
    static constexpr std::size_t hard_max_source_evidence = 8'000'000;
};

// Composes already-completed worker observations into an immutable operation
// plan. This boundary performs no native I/O, path reopening, probing,
// normalization or mutation. Each source object must have a corresponding
// completed inventory record; execution-time revalidation remains mandatory.
class OperationEvidenceAssembly final {
public:
    static Observation<OperationPlan::Pointer> assemble(FileOperation operation,
        std::shared_ptr<const FrozenInventory> inventory, std::span<const SourceTargetTopology> topologies,
        std::span<const ObjectInventoryData> source_objects,
        std::span<const DestinationCapabilityEvidence> destination,
        std::vector<DestinationConflict> conflicts = {}, SourceCapabilityProjectionPolicy projection_policy = {},
        OperationEvidenceAssemblyLimits limits = {});
};

} // namespace ck::files
