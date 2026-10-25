// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "destination_batch_job.hpp"
#include "frozen_inventory.hpp"

#include <cstddef>
#include <vector>

namespace ck::files {

struct DestinationConflictProjectionLimits final {
    std::size_t max_conflicts = 1'000'000;
    static constexpr std::size_t hard_max_conflicts = 8'000'000;
};

// Projects only the immutable destination-batch observations into plan
// conflicts. It performs no new lookup, path reopen, normalization, probe or
// mutation. Unreadable or malformed destination evidence never becomes an
// absent target.
class DestinationConflictProjection final {
public:
    static Observation<std::vector<DestinationConflict>> from_batch(
        const FrozenInventory& inventory, const CompletedDestinationBatch& batch,
        DestinationConflictProjectionLimits limits = {});
};

} // namespace ck::files
