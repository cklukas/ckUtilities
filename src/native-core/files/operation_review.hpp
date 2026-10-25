// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_consent.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace ck::files {

struct OperationReviewLimits final {
    std::size_t max_rows = 1'000'000;
    static constexpr std::size_t hard_max_rows = 8'000'000;
};

struct OperationReviewSummary final {
    std::size_t data_loss = 0;
    std::size_t security_change = 0;
    std::size_t metadata_loss = 0;
    std::size_t representation_change = 0;
    std::size_t uncertain = 0;
    std::size_t hard_failures = 0;
    std::size_t conflicts = 0;
};

struct OperationReviewIssueRow final {
    std::size_t ordinal = 0;
    PreservationIssue issue;
};

struct OperationReviewConflictRow final {
    std::size_t ordinal = 0;
    DestinationConflict conflict;
};

class OperationReviewModel final {
public:
    using Pointer = std::shared_ptr<const OperationReviewModel>;

    static Observation<Pointer> create(OperationPlan::Pointer plan, OperationReviewLimits limits = {});

    const OperationPlan::Pointer& plan() const noexcept { return plan_; }
    const std::vector<OperationReviewIssueRow>& issue_rows() const noexcept { return issue_rows_; }
    const std::vector<OperationReviewConflictRow>& conflict_rows() const noexcept { return conflict_rows_; }
    const OperationReviewSummary& summary() const noexcept { return summary_; }
    bool cancel_is_default() const noexcept { return true; }

    // The dialog supplies exactly the rows the user reviewed. This method does
    // not execute anything; the plan remains the authority for policy.
    Observation<ConsentReceipt> bind(ConsentAction action,
        std::span<const std::size_t> reviewed_issues = {},
        std::span<const std::size_t> resolved_conflicts = {},
        std::vector<NativeName> keep_both_names = {}) const;

private:
    explicit OperationReviewModel(OperationPlan::Pointer plan) noexcept : plan_(std::move(plan)) {}
    static OperationReviewSummary summarize(const OperationPlan& plan) noexcept;

    OperationPlan::Pointer plan_;
    std::vector<OperationReviewIssueRow> issue_rows_;
    std::vector<OperationReviewConflictRow> conflict_rows_;
    OperationReviewSummary summary_;
};

} // namespace ck::files
