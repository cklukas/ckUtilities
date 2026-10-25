// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_topology_projection.hpp"

#include <cstdint>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
    ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

bool valid_kind(ObjectKind kind) noexcept {
    return kind >= ObjectKind::RegularFile && kind <= ObjectKind::Other;
}

SourceTargetTopology ancestry_topology(const Observation<AncestorWalkWitness>& ancestry) {
    if (!ancestry.value()) return SourceTargetTopology::Unknown;
    return ancestry.value()->end == AncestorWalkEnd::ObservedSource
        ? SourceTargetTopology::DestinationInsideSource : SourceTargetTopology::Unknown;
}

} // namespace

Observation<std::vector<SourceTargetTopology>> OperationTopologyProjection::from_worker(
    std::span<const SourceTargetInspection> source_target,
    std::span<const Observation<AncestorWalkWitness>> ancestry,
    OperationTopologyProjectionLimits limits) {
    using Result = Observation<std::vector<SourceTargetTopology>>;
    if (!limits.max_rows || limits.max_rows > OperationTopologyProjectionLimits::hard_max_rows)
        return rejected<std::vector<SourceTargetTopology>>(AdapterError::InvalidLimit,
            "operation_topology_projection.limits");
    if (source_target.size() != ancestry.size() || source_target.size() > limits.max_rows)
        return rejected<std::vector<SourceTargetTopology>>(AdapterError::PlanInvalid,
            "operation_topology_projection.shape");

    std::vector<SourceTargetTopology> result;
    result.reserve(source_target.size());
    for (std::size_t index = 0; index < source_target.size(); ++index) {
        const auto& row = source_target[index];
        if (!row.source_now.value())
            return Result::unavailable(row.source_now.unavailable_detail()->state, row.source_now.unavailable_detail()->error);
        if (!row.source_now.value()->kind.value())
            return Result::unavailable(row.source_now.value()->kind.unavailable_detail()->state,
                row.source_now.value()->kind.unavailable_detail()->error);
        const auto kind = *row.source_now.value()->kind.value();
        if (!valid_kind(kind))
            return rejected<std::vector<SourceTargetTopology>>(AdapterError::InvalidNativeResult,
                "operation_topology_projection.kind");

        if (row.comparison.value()) {
            if (*row.comparison.value() == IdentityComparison::Same) {
                result.push_back(SourceTargetTopology::SameObject);
            } else if (*row.comparison.value() == IdentityComparison::Different && kind != ObjectKind::Directory) {
                result.push_back(SourceTargetTopology::Disjoint);
            } else {
                result.push_back(ancestry_topology(ancestry[index]));
            }
            continue;
        }

        const auto& detail = *row.comparison.unavailable_detail();
        const bool missing_target = detail.state == ObservationState::Unsupported &&
            detail.error.code == static_cast<std::int64_t>(AdapterError::NotApplicable) &&
            detail.error.operation == "source_target.no_existing_target";
        if (kind != ObjectKind::Directory && missing_target)
            result.push_back(SourceTargetTopology::Disjoint);
        else if (kind == ObjectKind::Directory)
            result.push_back(ancestry_topology(ancestry[index]));
        else
            result.push_back(SourceTargetTopology::Unknown);
    }
    return Result::available(std::move(result));
}

} // namespace ck::files
