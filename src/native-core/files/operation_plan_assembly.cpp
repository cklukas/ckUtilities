// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_plan_assembly.hpp"

#include <cstdint>
#include <iterator>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

} // namespace

Observation<OperationPlan::Pointer> OperationPlanAssembly::assemble(FileOperation operation,
    std::shared_ptr<const FrozenInventory> inventory, std::span<const SourceTargetTopology> topologies,
    std::span<const SourceCapabilityEvidence> source,
    std::span<const DestinationCapabilityEvidence> destination,
    std::vector<DestinationConflict> conflicts, OperationPlanAssemblyLimits limits,
    std::vector<PreservationIssue> additional_issues) {
    using Result = Observation<OperationPlan::Pointer>;
    if (!inventory || topologies.size() != inventory->entries().size())
        return rejected<OperationPlan::Pointer>(AdapterError::PlanInvalid, "operation_plan_assembly.inventory_shape");
    for (const auto& evidence : source)
        if (evidence.item >= inventory->entries().size())
            return rejected<OperationPlan::Pointer>(AdapterError::PlanInvalid, "operation_plan_assembly.source_item");
    for (const auto& issue : additional_issues)
        if (issue.item >= inventory->entries().size())
            return rejected<OperationPlan::Pointer>(AdapterError::PlanInvalid, "operation_plan_assembly.issue_item");

    auto differences = PreservationCapabilityDiff::compare(source, destination, limits.preservation);
    if (!differences.value())
        return Result::unavailable(differences.unavailable_detail()->state, differences.unavailable_detail()->error);
    auto issues = std::move(differences).take_value();
    if (additional_issues.size() > limits.plan.max_issues ||
        issues.size() > limits.plan.max_issues - additional_issues.size())
        return rejected<OperationPlan::Pointer>(AdapterError::InventoryLimit, "operation_plan_assembly.issues",
            ObservationState::Unknown);
    issues.insert(issues.end(), std::make_move_iterator(additional_issues.begin()),
        std::make_move_iterator(additional_issues.end()));
    return OperationPlan::create_from_inventory(operation, std::move(inventory), topologies,
        std::move(issues), std::move(conflicts), limits.plan);
}

} // namespace ck::files
