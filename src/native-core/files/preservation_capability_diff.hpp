// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "location_info.hpp"
#include "operation_consent.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace ck::files {

class DestinationDirectoryAssessment;

// These are worker-owned facts, not filesystem probes. Main-data verification
// and destination creation are separate requirements; this adapter compares
// only source features that have an explicit destination feature mapping.
enum class SourceCapabilityState { Present, Absent, Unknown, Denied, Changed };

struct SourceCapabilityEvidence final {
    std::size_t item = 0;
    PreservationFeature feature = PreservationFeature::MainData;
    SourceCapabilityState state = SourceCapabilityState::Unknown;
    PreservationConsequence consequence = PreservationConsequence::Uncertain;
    VolumeFeature destination_feature = VolumeFeature::Unknown;
    // A source feature that cannot be advertised by the destination is a hard
    // refusal when the caller says its loss cannot produce a meaningful result.
    bool hard_if_missing = false;
};

struct DestinationCapabilityEvidence final {
    VolumeFeature feature = VolumeFeature::Unknown;
    Observation<FeatureAdvertisement> advertisement;
};

struct PreservationDiffLimits final {
    std::size_t max_issues = 1'000'000;
    static constexpr std::size_t hard_max_issues = 8'000'000;
};

class PreservationCapabilityDiff final {
public:
    // No I/O, path reopening, probing, normalization or mutation. The caller
    // owns the worker evidence and must revalidate it before execution.
    static Observation<std::vector<PreservationIssue>> compare(
        std::span<const SourceCapabilityEvidence> source,
        std::span<const DestinationCapabilityEvidence> destination,
        PreservationDiffLimits limits = {});

    // Copies only the already-published worker observations. It does not query
    // the assessment's held directory or infer capabilities from labels.
    static std::vector<DestinationCapabilityEvidence> from_destination_assessment(
        const DestinationDirectoryAssessment& assessment);
};

} // namespace ck::files
