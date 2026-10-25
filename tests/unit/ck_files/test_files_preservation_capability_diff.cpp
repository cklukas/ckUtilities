// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "preservation_capability_diff.hpp"

#include "cvision/testing/cktest.hpp"

#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;

DestinationCapabilityEvidence advertised(VolumeFeature feature, FeatureAdvertisement value) {
    return {feature, Observation<FeatureAdvertisement>::available(value)};
}

DestinationCapabilityEvidence unavailable(VolumeFeature feature, ObservationState state) {
    return {feature, Observation<FeatureAdvertisement>::unavailable(state,
        {ErrorDomain::Adapter, 77, "fixture.destination_capability"})};
}

std::vector<PreservationIssue> take(Observation<std::vector<PreservationIssue>> value) {
    if (!value.value()) throw std::runtime_error("preservation diff fixture unavailable");
    return std::move(value).take_value();
}
} // namespace

CK_TEST(preservation_diff_ignores_absent_features_and_accepts_advertised_sources) {
    const std::vector source{
        SourceCapabilityEvidence{0, PreservationFeature::AdditionalChannels, SourceCapabilityState::Present,
            PreservationConsequence::DataLoss, VolumeFeature::NamedStreams, false},
        SourceCapabilityEvidence{0, PreservationFeature::ExtendedAttributes, SourceCapabilityState::Absent,
            PreservationConsequence::MetadataLoss, VolumeFeature::ExtendedAttributes, true}};
    const std::vector destination{advertised(VolumeFeature::NamedStreams, FeatureAdvertisement::Advertised)};
    const auto issues = take(PreservationCapabilityDiff::compare(source, destination));
    CK_CHECK(issues.empty());
}

CK_TEST(preservation_diff_retains_missing_and_unavailable_destination_knowledge) {
    const std::vector source{
        SourceCapabilityEvidence{0, PreservationFeature::AdditionalChannels, SourceCapabilityState::Present,
            PreservationConsequence::DataLoss, VolumeFeature::NamedStreams, false},
        SourceCapabilityEvidence{1, PreservationFeature::ExtendedAttributes, SourceCapabilityState::Present,
            PreservationConsequence::MetadataLoss, VolumeFeature::ExtendedAttributes, false},
        SourceCapabilityEvidence{2, PreservationFeature::PermissionsOwnership, SourceCapabilityState::Present,
            PreservationConsequence::SecurityChange, VolumeFeature::PersistentAcls, false}};
    const std::vector destination{
        advertised(VolumeFeature::NamedStreams, FeatureAdvertisement::NotAdvertised),
            unavailable(VolumeFeature::ExtendedAttributes, ObservationState::Unsupported),
            unavailable(VolumeFeature::PersistentAcls, ObservationState::Unknown)};
    const auto issues = take(PreservationCapabilityDiff::compare(source, destination));
    CK_CHECK(issues.size() == 3);
    CK_CHECK(issues[0].state == PreservationIssueState::Missing && !issues[0].hard_failure);
    CK_CHECK(issues[1].state == PreservationIssueState::Unsupported && !issues[1].hard_failure);
    CK_CHECK(issues[2].state == PreservationIssueState::Unknown && issues[2].hard_failure);
}

CK_TEST(preservation_diff_source_uncertainty_is_hard_and_destination_errors_are_preserved) {
    const std::vector source{
        SourceCapabilityEvidence{4, PreservationFeature::TimesAndFlags, SourceCapabilityState::Unknown,
            PreservationConsequence::Uncertain, VolumeFeature::CasePreservedNames, false},
        SourceCapabilityEvidence{5, PreservationFeature::LinksAndIdentity, SourceCapabilityState::Denied,
            PreservationConsequence::SecurityChange, VolumeFeature::SymbolicLinks, false}};
    const std::vector destination{
        unavailable(VolumeFeature::CasePreservedNames, ObservationState::Denied),
        unavailable(VolumeFeature::SymbolicLinks, ObservationState::Changed)};
    const auto issues = take(PreservationCapabilityDiff::compare(source, destination));
    CK_CHECK(issues.size() == 2);
    CK_CHECK(issues[0].state == PreservationIssueState::Unknown && issues[0].hard_failure);
    CK_CHECK(issues[1].state == PreservationIssueState::Denied && issues[1].hard_failure);
}

CK_TEST(preservation_diff_rejects_ambiguous_or_unbounded_inputs) {
    const SourceCapabilityEvidence source{0, PreservationFeature::AdditionalChannels,
        SourceCapabilityState::Present, PreservationConsequence::DataLoss, VolumeFeature::NamedStreams, false};
    const std::vector duplicate_capabilities{
        advertised(VolumeFeature::NamedStreams, FeatureAdvertisement::Advertised),
        advertised(VolumeFeature::NamedStreams, FeatureAdvertisement::Advertised)};
    const auto duplicate_destination = PreservationCapabilityDiff::compare(std::vector{source}, duplicate_capabilities);
    CK_CHECK(duplicate_destination.state() == ObservationState::Failed);
    CK_CHECK(duplicate_destination.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));

    const auto invalid_source = PreservationCapabilityDiff::compare(
        std::vector{SourceCapabilityEvidence{0, PreservationFeature::MainData, SourceCapabilityState::Present,
            PreservationConsequence::DataLoss, VolumeFeature::Unknown, true}}, {});
    CK_CHECK(invalid_source.state() == ObservationState::Failed);

    const std::vector bounded_capabilities{advertised(VolumeFeature::NamedStreams, FeatureAdvertisement::NotAdvertised)};
    const auto bounded = PreservationCapabilityDiff::compare(std::vector{source}, bounded_capabilities, {0});
    CK_CHECK(bounded.state() == ObservationState::Failed &&
        bounded.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));
}
