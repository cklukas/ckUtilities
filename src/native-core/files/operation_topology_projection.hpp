// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ancestor_walk_job.hpp"
#include "operation_consent.hpp"
#include "source_target_job.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace ck::files {

struct OperationTopologyProjectionLimits final {
    std::size_t max_rows = 1'000'000;
    static constexpr std::size_t hard_max_rows = 8'000'000;
};

// Converts completed worker topology observations into plan topology values.
// A regular file proven different from its target is disjoint because it
// cannot contain a destination directory. Directory rows require an explicit
// ancestor witness; an unavailable or repeated walk never becomes Disjoint.
// This adapter performs no native I/O or path reopening.
class OperationTopologyProjection final {
public:
    static Observation<std::vector<SourceTargetTopology>> from_worker(
        std::span<const SourceTargetInspection> source_target,
        std::span<const Observation<AncestorWalkWitness>> ancestry,
        OperationTopologyProjectionLimits limits = {});
};

} // namespace ck::files
