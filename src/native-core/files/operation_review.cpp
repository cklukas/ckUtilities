// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_review.hpp"

#include <cstdint>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation) {
    return Observation<T>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

} // namespace

Observation<OperationReviewModel::Pointer> OperationReviewModel::create(OperationPlan::Pointer plan,
    OperationReviewLimits limits) {
    using Result = Observation<Pointer>;
    if (!plan) return rejected<Pointer>(AdapterError::InvalidIdentifier, "operation_review.plan");
    if (!limits.max_rows || limits.max_rows > OperationReviewLimits::hard_max_rows)
        return rejected<Pointer>(AdapterError::InvalidLimit, "operation_review.limits");
    if (plan->issues().size() > limits.max_rows || plan->conflicts().size() > limits.max_rows ||
        plan->issues().size() > limits.max_rows - plan->conflicts().size())
        return rejected<Pointer>(AdapterError::InventoryLimit, "operation_review.rows");

    auto writable = std::unique_ptr<OperationReviewModel>(new OperationReviewModel(std::move(plan)));
    writable->issue_rows_.reserve(writable->plan_->issues().size());
    for (std::size_t index = 0; index < writable->plan_->issues().size(); ++index)
        writable->issue_rows_.push_back({index, writable->plan_->issues()[index]});
    writable->conflict_rows_.reserve(writable->plan_->conflicts().size());
    for (std::size_t index = 0; index < writable->plan_->conflicts().size(); ++index)
        writable->conflict_rows_.push_back({index, writable->plan_->conflicts()[index]});
    writable->summary_ = summarize(*writable->plan_);
    return Result::available(Pointer(std::move(writable)));
}

OperationReviewSummary OperationReviewModel::summarize(const OperationPlan& plan) noexcept {
    OperationReviewSummary summary;
    summary.conflicts = plan.conflicts().size();
    for (const auto& issue : plan.issues()) {
        switch (issue.consequence) {
        case PreservationConsequence::DataLoss: ++summary.data_loss; break;
        case PreservationConsequence::SecurityChange: ++summary.security_change; break;
        case PreservationConsequence::MetadataLoss: ++summary.metadata_loss; break;
        case PreservationConsequence::RepresentationChange: ++summary.representation_change; break;
        case PreservationConsequence::Uncertain: ++summary.uncertain; break;
        }
        if (issue.hard_failure || issue.state == PreservationIssueState::Unknown ||
            issue.state == PreservationIssueState::Denied || issue.state == PreservationIssueState::Changed)
            ++summary.hard_failures;
    }
    return summary;
}

Observation<ConsentReceipt> OperationReviewModel::bind(ConsentAction action,
    std::span<const std::size_t> reviewed_issues, std::span<const std::size_t> resolved_conflicts,
    std::vector<NativeName> keep_both_names) const {
    if (!plan_) return rejected<ConsentReceipt>(AdapterError::InvalidIdentifier, "operation_review.plan");
    return plan_->bind(action, std::vector<std::size_t>(reviewed_issues.begin(), reviewed_issues.end()),
        std::vector<std::size_t>(resolved_conflicts.begin(), resolved_conflicts.end()), std::move(keep_both_names));
}

} // namespace ck::files
