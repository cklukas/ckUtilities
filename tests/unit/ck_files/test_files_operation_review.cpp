// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_review.hpp"

#include "cvision/testing/cktest.hpp"

#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;

FrozenOperationItem item(std::uint64_t id, ObjectKind kind = ObjectKind::RegularFile) {
    return {ObjectIdentity{AdapterScope{}, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(id), {}}, kind,
        SourceTargetTopology::Disjoint};
}

OperationPlan::Pointer plan() {
    auto result = OperationPlan::create(FileOperation::Copy, {item(1)},
        {PreservationIssue{0, PreservationFeature::AdditionalChannels, PreservationIssueState::Missing,
            PreservationConsequence::DataLoss, false},
            PreservationIssue{0, PreservationFeature::PermissionsOwnership, PreservationIssueState::Unknown,
                PreservationConsequence::SecurityChange, true}},
        {{0, DestinationConflictKind::ExistingFile}});
    if (!result.value()) throw std::runtime_error("operation review plan fixture unavailable");
    return std::move(result).take_value();
}

template <class T>
T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("operation review fixture unavailable");
    return std::move(value).take_value();
}
} // namespace

CK_TEST(operation_review_projects_risks_without_rewriting_plan_evidence) {
    const auto model = take(OperationReviewModel::create(plan()));
    CK_CHECK(model->cancel_is_default() && model->issue_rows().size() == 2 &&
        model->conflict_rows().size() == 1 && model->summary().data_loss == 1 &&
        model->summary().security_change == 1 && model->summary().hard_failures == 1 &&
        model->summary().conflicts == 1);
}

CK_TEST(operation_review_delegates_conservative_actions_to_the_plan) {
    const auto model = take(OperationReviewModel::create(plan()));
    const auto cancel = take(model->bind(ConsentAction::Cancel));
    CK_CHECK(model->plan()->receipt_matches(cancel));
    const std::vector<std::size_t> reviewed{0, 1};
    CK_CHECK(model->bind(ConsentAction::ContinueWithListedLosses, reviewed).state() == ObservationState::Failed);
    CK_CHECK(model->bind(ConsentAction::ChooseAnotherDestination).value());
}

CK_TEST(operation_review_rejects_null_and_unbounded_models) {
    CK_CHECK(OperationReviewModel::create(nullptr).state() == ObservationState::Failed);
    const auto bounded = OperationReviewModel::create(plan(), {0});
    CK_CHECK(bounded.state() == ObservationState::Failed &&
        bounded.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));
}
