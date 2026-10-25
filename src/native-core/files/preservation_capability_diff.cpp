// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "preservation_capability_diff.hpp"

#include "destination_directory_assessment.hpp"

#include <algorithm>
#include <cstdint>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation, ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

const DestinationCapabilityEvidence* destination_for(std::span<const DestinationCapabilityEvidence> destination,
    VolumeFeature feature) noexcept {
    for (const auto& candidate : destination)
        if (candidate.feature == feature) return &candidate;
    return nullptr;
}

PreservationIssueState issue_state(ObservationState state) noexcept {
    switch (state) {
    case ObservationState::Unsupported: return PreservationIssueState::Unsupported;
    case ObservationState::Denied: return PreservationIssueState::Denied;
    case ObservationState::Changed: return PreservationIssueState::Changed;
    case ObservationState::Gone:
    case ObservationState::Unknown:
    case ObservationState::Failed:
    case ObservationState::Available: return PreservationIssueState::Unknown;
    }
    return PreservationIssueState::Unknown;
}

bool hard_unavailable(ObservationState state) noexcept {
    return state == ObservationState::Unknown || state == ObservationState::Denied ||
        state == ObservationState::Changed || state == ObservationState::Gone || state == ObservationState::Failed;
}

bool duplicate_source(std::span<const SourceCapabilityEvidence> source, std::size_t index) noexcept {
    for (std::size_t prior = 0; prior < index; ++prior)
        if (source[prior].item == source[index].item && source[prior].feature == source[index].feature &&
            source[prior].destination_feature == source[index].destination_feature)
            return true;
    return false;
}

bool duplicate_destination(std::span<const DestinationCapabilityEvidence> destination, std::size_t index) noexcept {
    for (std::size_t prior = 0; prior < index; ++prior)
        if (destination[prior].feature == destination[index].feature)
            return true;
    return false;
}

} // namespace

Observation<std::vector<PreservationIssue>> PreservationCapabilityDiff::compare(
    std::span<const SourceCapabilityEvidence> source,
    std::span<const DestinationCapabilityEvidence> destination,
    PreservationDiffLimits limits) {
    using Result = Observation<std::vector<PreservationIssue>>;
    if (!limits.max_issues || limits.max_issues > PreservationDiffLimits::hard_max_issues)
        return rejected<std::vector<PreservationIssue>>(AdapterError::InvalidLimit, "preservation_diff.limits");
    for (std::size_t index = 0; index < destination.size(); ++index) {
        if (destination[index].feature == VolumeFeature::Unknown || duplicate_destination(destination, index))
            return rejected<std::vector<PreservationIssue>>(AdapterError::PlanInvalid, "preservation_diff.destination");
    }
    for (std::size_t index = 0; index < source.size(); ++index) {
        const auto& evidence = source[index];
        if (evidence.destination_feature == VolumeFeature::Unknown || duplicate_source(source, index))
            return rejected<std::vector<PreservationIssue>>(AdapterError::PlanInvalid, "preservation_diff.source");
    }

    std::vector<PreservationIssue> issues;
    issues.reserve(std::min(limits.max_issues, source.size()));
    for (const auto& evidence : source) {
        if (evidence.state == SourceCapabilityState::Absent) continue;
        PreservationIssueState state = PreservationIssueState::Unknown;
        bool hard = evidence.hard_if_missing;
        if (evidence.state == SourceCapabilityState::Present) {
            const auto* destination_evidence = destination_for(destination, evidence.destination_feature);
            if (destination_evidence == nullptr) {
                state = PreservationIssueState::Unknown;
                hard = true;
            } else if (destination_evidence->advertisement.value()) {
                if (*destination_evidence->advertisement.value() == FeatureAdvertisement::Advertised) continue;
                state = PreservationIssueState::Missing;
            } else {
                state = issue_state(destination_evidence->advertisement.state());
                hard = hard || hard_unavailable(destination_evidence->advertisement.state());
            }
        } else {
            state = evidence.state == SourceCapabilityState::Denied ? PreservationIssueState::Denied
                : evidence.state == SourceCapabilityState::Changed ? PreservationIssueState::Changed
                : PreservationIssueState::Unknown;
            hard = true;
        }
        if (issues.size() >= limits.max_issues)
            return rejected<std::vector<PreservationIssue>>(AdapterError::InventoryLimit, "preservation_diff.issues",
                ObservationState::Unknown);
        issues.push_back(PreservationIssue{evidence.item, evidence.feature, state, evidence.consequence, hard});
    }
    return Result::available(std::move(issues));
}

std::vector<DestinationCapabilityEvidence> PreservationCapabilityDiff::from_destination_assessment(
    const DestinationDirectoryAssessment& assessment) {
    std::vector<DestinationCapabilityEvidence> result;
    result.reserve(destination_advertised_features.size());
    for (const auto feature : destination_advertised_features)
        result.push_back({feature, assessment.advertisement(feature)});
    return result;
}

} // namespace ck::files
