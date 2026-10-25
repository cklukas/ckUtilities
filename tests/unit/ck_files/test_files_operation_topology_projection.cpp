// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_topology_projection.hpp"

#include "cvision/testing/cktest.hpp"

#include <array>
#include <utility>

namespace {
using namespace ck::files;

Observation<BasicMetadata> source(ObjectKind kind) {
    BasicMetadata value;
    value.kind = Observation<ObjectKind>::available(kind);
    return Observation<BasicMetadata>::available(std::move(value));
}

Observation<AncestorWalkWitness> observed() {
    return Observation<AncestorWalkWitness>::available({AncestorWalkEnd::ObservedSource, 1, {}});
}

Observation<AncestorWalkWitness> unknown() {
    return Observation<AncestorWalkWitness>::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "fixture.ancestor"});
}
} // namespace

CK_TEST(operation_topology_projection_keeps_same_inside_and_file_disjoint_distinct) {
    const std::array rows{
        SourceTargetInspection{source(ObjectKind::RegularFile), {}, Observation<IdentityComparison>::available(IdentityComparison::Same)},
        SourceTargetInspection{source(ObjectKind::Directory), {}, Observation<IdentityComparison>::available(IdentityComparison::Different)},
        SourceTargetInspection{source(ObjectKind::RegularFile), {}, Observation<IdentityComparison>::available(IdentityComparison::Different)}};
    const std::array ancestry{unknown(), observed(), unknown()};
    const auto result = OperationTopologyProjection::from_worker(rows, ancestry);
    CK_CHECK(result.value() && result.value()->size() == 3);
    CK_CHECK((*result.value())[0] == SourceTargetTopology::SameObject &&
        (*result.value())[1] == SourceTargetTopology::DestinationInsideSource &&
        (*result.value())[2] == SourceTargetTopology::Disjoint);
}

CK_TEST(operation_topology_projection_allows_file_missing_target_but_not_unknown_target) {
    const auto missing = Observation<IdentityComparison>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "source_target.no_existing_target"});
    const auto denied = Observation<IdentityComparison>::unavailable(ObservationState::Denied,
        {ErrorDomain::Posix, 13, "fixture.denied"});
    const std::array rows{
        SourceTargetInspection{source(ObjectKind::RegularFile), {}, missing},
        SourceTargetInspection{source(ObjectKind::RegularFile), {}, denied}};
    const std::array ancestry{unknown(), unknown()};
    const auto result = OperationTopologyProjection::from_worker(rows, ancestry);
    CK_CHECK(result.value() && (*result.value())[0] == SourceTargetTopology::Disjoint &&
        (*result.value())[1] == SourceTargetTopology::Unknown);
}

CK_TEST(operation_topology_projection_rejects_shape_kind_and_limits) {
    const std::array row{SourceTargetInspection{source(ObjectKind::RegularFile), {},
        Observation<IdentityComparison>::available(IdentityComparison::Different)}};
    const std::array no_ancestry{unknown()};
    OperationTopologyProjectionLimits limits;
    limits.max_rows = 0;
    const auto invalid_limit = OperationTopologyProjection::from_worker(row, no_ancestry, limits);
    CK_CHECK(invalid_limit.state() == ObservationState::Failed &&
        invalid_limit.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));
    const std::array wrong_shape{unknown(), unknown()};
    const auto shape = OperationTopologyProjection::from_worker(row, wrong_shape);
    CK_CHECK(shape.state() == ObservationState::Failed &&
        shape.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));

    const std::array invalid_kind{SourceTargetInspection{source(static_cast<ObjectKind>(999)), {},
        Observation<IdentityComparison>::available(IdentityComparison::Different)}};
    const auto kind = OperationTopologyProjection::from_worker(invalid_kind, no_ancestry);
    CK_CHECK(kind.state() == ObservationState::Failed &&
        kind.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidNativeResult));
}
