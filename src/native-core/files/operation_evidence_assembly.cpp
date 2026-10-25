// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_evidence_assembly.hpp"

#include <cstdint>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
    ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

} // namespace

Observation<OperationPlan::Pointer> OperationEvidenceAssembly::assemble(FileOperation operation,
    std::shared_ptr<const FrozenInventory> inventory, std::span<const SourceTargetTopology> topologies,
    std::span<const ObjectInventoryData> source_objects,
    std::span<const DestinationCapabilityEvidence> destination, std::vector<DestinationConflict> conflicts,
    SourceCapabilityProjectionPolicy projection_policy, OperationEvidenceAssemblyLimits limits) {
    using Result = Observation<OperationPlan::Pointer>;
    if (!inventory || source_objects.size() != inventory->entries().size())
        return rejected<OperationPlan::Pointer>(AdapterError::PlanInvalid, "operation_evidence.inventory_shape");
    if (!limits.max_source_evidence || limits.max_source_evidence > OperationEvidenceAssemblyLimits::hard_max_source_evidence)
        return rejected<OperationPlan::Pointer>(AdapterError::InvalidLimit, "operation_evidence.source_limit");

    std::vector<SourceCapabilityEvidence> source;
    for (std::size_t item = 0; item < source_objects.size(); ++item) {
        auto projected = SourceCapabilityProjection::from_inventory(item, source_objects[item], projection_policy,
            limits.projection);
        if (!projected.value())
            return Result::unavailable(projected.unavailable_detail()->state, projected.unavailable_detail()->error);
        if (projected.value()->size() > limits.max_source_evidence - source.size())
            return rejected<OperationPlan::Pointer>(AdapterError::InventoryLimit, "operation_evidence.source_limit",
                ObservationState::Unknown);
        auto values = std::move(projected).take_value();
        source.insert(source.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
    }
    return OperationPlanAssembly::assemble(operation, std::move(inventory), topologies, source, destination,
        std::move(conflicts), limits.plan);
}

} // namespace ck::files
