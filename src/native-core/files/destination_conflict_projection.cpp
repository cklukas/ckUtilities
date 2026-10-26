// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_conflict_projection.hpp"

#include <cstdint>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
    ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

bool same_components(const std::vector<NativeName>& left, const std::vector<NativeName>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (left[index] != right[index]) return false;
    return true;
}

bool destination_directory_is_known_missing(const FrozenInventory& inventory,
                                            const CompletedDestinationBatch& batch, std::size_t item) noexcept {
    const auto *probe = batch.probe(item);
    if (probe && probe->lookup().value())
        return probe->lookup().value()->state() == ChildLookupState::Missing;
    const auto *failure = batch.probe_failure(item);
    if (!failure || failure->error.operation != "destination_batch.parent_unavailable") return false;

    const auto& relative = inventory.entries()[item].relative_components;
    if (relative.empty()) return false;
    std::vector<NativeName> parent_components(relative.begin(), relative.end() - 1);
    for (std::size_t candidate = 0; candidate < item; ++candidate) {
        if (inventory.entries()[candidate].kind == ObjectKind::Directory &&
            same_components(inventory.entries()[candidate].relative_components, parent_components))
            return destination_directory_is_known_missing(inventory, batch, candidate);
    }
    return false;
}

bool parent_is_known_missing(const FrozenInventory& inventory, const CompletedDestinationBatch& batch,
                             std::size_t item) noexcept {
    const auto& relative = inventory.entries()[item].relative_components;
    if (relative.empty()) return false;
    std::vector<NativeName> parent_components(relative.begin(), relative.end() - 1);
    for (std::size_t candidate = 0; candidate < item; ++candidate)
        if (inventory.entries()[candidate].kind == ObjectKind::Directory &&
            same_components(inventory.entries()[candidate].relative_components, parent_components))
            return destination_directory_is_known_missing(inventory, batch, candidate);
    return false;
}

} // namespace

Observation<std::vector<DestinationConflict>> DestinationConflictProjection::from_batch(
    const FrozenInventory& inventory, const CompletedDestinationBatch& batch,
    DestinationConflictProjectionLimits limits) {
    using Result = Observation<std::vector<DestinationConflict>>;
    if (!limits.max_conflicts || limits.max_conflicts > DestinationConflictProjectionLimits::hard_max_conflicts)
        return rejected<std::vector<DestinationConflict>>(AdapterError::InvalidLimit,
            "destination_conflict_projection.limits");
    if (batch.entries().size() != inventory.entries().size())
        return rejected<std::vector<DestinationConflict>>(AdapterError::PlanInvalid,
            "destination_conflict_projection.shape");

    std::vector<DestinationConflict> result;
    const auto append = [&](DestinationConflict conflict) -> bool {
        if (result.size() >= limits.max_conflicts) return false;
        result.push_back(conflict);
        return true;
    };
    for (std::size_t item = 0; item < batch.entries().size(); ++item) {
        const auto& entry = batch.entries()[item];
        if (!entry.mapped.value())
            return Result::unavailable(entry.mapped.unavailable_detail()->state, entry.mapped.unavailable_detail()->error);

        // Multiple source rows mapped to one exact native destination name
        // cannot be resolved by replacing or merging one target.
        if (batch.exact_name_count(item) > 1) {
            if (!append({item, DestinationConflictKind::CaseCollision}))
                return rejected<std::vector<DestinationConflict>>(AdapterError::InventoryLimit,
                    "destination_conflict_projection.conflicts", ObservationState::Unknown);
            continue;
        }

        if (const auto* failure = batch.probe_failure(item)) {
            // A recursive copy whose destination root is absent has no
            // destination cursor for descendants yet. Their parent-unavailable
            // observations are expected absence, not destination conflicts;
            // retain every other unavailable observation conservatively.
            if (failure->error.operation == "destination_batch.parent_unavailable" &&
                parent_is_known_missing(inventory, batch, item))
                continue;
            if (!append({item, DestinationConflictKind::Unknown}))
                return rejected<std::vector<DestinationConflict>>(AdapterError::InventoryLimit,
                    "destination_conflict_projection.conflicts", ObservationState::Unknown);
            (void)failure;
            continue;
        }
        const auto* probe = batch.probe(item);
        if (!probe || !probe->lookup().value()) {
            if (!append({item, DestinationConflictKind::Unknown}))
                return rejected<std::vector<DestinationConflict>>(AdapterError::InventoryLimit,
                    "destination_conflict_projection.conflicts", ObservationState::Unknown);
            continue;
        }
        const auto& lookup = *probe->lookup().value();
        if (lookup.state() == ChildLookupState::Missing) continue;
        if (lookup.state() != ChildLookupState::Found || !lookup.entry())
            return rejected<std::vector<DestinationConflict>>(AdapterError::InvalidNativeResult,
                "destination_conflict_projection.lookup");
        if (!probe->existing_metadata().value() || !probe->existing_metadata().value()->kind.value()) {
            if (!append({item, DestinationConflictKind::Unknown}))
                return rejected<std::vector<DestinationConflict>>(AdapterError::InventoryLimit,
                    "destination_conflict_projection.conflicts", ObservationState::Unknown);
            continue;
        }
        const auto target_kind = *probe->existing_metadata().value()->kind.value();
        if (target_kind < ObjectKind::RegularFile || target_kind > ObjectKind::Other)
            return rejected<std::vector<DestinationConflict>>(AdapterError::InvalidNativeResult,
                "destination_conflict_projection.kind");
        const auto source_kind = inventory.entries()[item].kind;
        const auto conflict = source_kind == ObjectKind::Directory
            ? target_kind == ObjectKind::Directory ? DestinationConflictKind::ExistingDirectory
                                                    : DestinationConflictKind::TypeMismatch
            : target_kind == ObjectKind::Directory ? DestinationConflictKind::TypeMismatch
                                                    : DestinationConflictKind::ExistingFile;
        if (!append({item, conflict}))
            return rejected<std::vector<DestinationConflict>>(AdapterError::InventoryLimit,
                "destination_conflict_projection.conflicts", ObservationState::Unknown);
    }
    return Result::available(std::move(result));
}

} // namespace ck::files
