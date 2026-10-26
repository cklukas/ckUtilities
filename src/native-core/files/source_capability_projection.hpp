// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "object_inventory_job.hpp"
#include "preservation_capability_diff.hpp"

#include <cstddef>
#include <vector>

namespace ck::files {

// A platform adapter may supply this only when the native channel query is
// unavailable but its destination mapping is still known. No default maps an
// unknown channel namespace to a guessed destination feature.
struct SourceCapabilityProjectionPolicy final {
    VolumeFeature unavailable_channel_destination = VolumeFeature::Unknown;
    // The execution adapter may intentionally preserve only a declared
    // portable subset. When set, the evidence assembly adds an explicit,
    // reviewable loss row for that adapter boundary instead of treating the
    // omission as a successful preservation result.
    bool require_unhandled_metadata_consent = false;
};

struct SourceCapabilityProjectionLimits final {
    std::size_t max_evidence = 32;
    static constexpr std::size_t hard_max_evidence = 256;
};

// Converts only already-published object inventory observations into source
// capability evidence. It performs no native I/O, path reopening, value
// normalization or capability inference from filesystem labels. Unsupported
// query families that are irrelevant to the object are omitted; an unknown or
// denied relevant observation becomes hard evidence rather than absence.
class SourceCapabilityProjection final {
public:
    static Observation<std::vector<SourceCapabilityEvidence>> from_inventory(
        std::size_t item, const ObjectInventoryData& inventory,
        SourceCapabilityProjectionPolicy policy = {}, SourceCapabilityProjectionLimits limits = {});

    static Observation<std::vector<SourceCapabilityEvidence>> from_completed(
        std::size_t item, const CompletedObjectInventory& inventory,
        SourceCapabilityProjectionPolicy policy = {}, SourceCapabilityProjectionLimits limits = {}) {
        return from_inventory(item, inventory.data(), policy, limits);
    }
};

} // namespace ck::files
