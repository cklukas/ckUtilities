// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "frozen_inventory.hpp"
#include "preservation_capability_diff.hpp"

#include <memory>
#include <span>
#include <vector>

namespace ck::files {

struct OperationPlanAssemblyLimits final {
    OperationPlanLimits plan;
    PreservationDiffLimits preservation;
};

// Binds already-published recursive inventory, source capability evidence,
// destination advertisements, topology observations and conflict rows into
// one immutable operation plan. This value-only assembly performs no native
// I/O, path reopening, probing or mutation. The caller remains responsible for
// obtaining all inputs on the worker and for execution-time revalidation.
class OperationPlanAssembly final {
public:
    static Observation<OperationPlan::Pointer> assemble(FileOperation operation,
        std::shared_ptr<const FrozenInventory> inventory, std::span<const SourceTargetTopology> topologies,
        std::span<const SourceCapabilityEvidence> source,
        std::span<const DestinationCapabilityEvidence> destination,
        std::vector<DestinationConflict> conflicts = {}, OperationPlanAssemblyLimits limits = {});
};

} // namespace ck::files
