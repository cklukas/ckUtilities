// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_consent.hpp"
#include "frozen_inventory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ck::files {

struct PlanToken {};

namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char* operation, ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

bool contains(const std::vector<std::size_t>& values, std::size_t value) noexcept {
    return std::find(values.begin(), values.end(), value) != values.end();
}

} // namespace

Observation<OperationPlan::Pointer> OperationPlan::create(FileOperation operation,
    std::vector<FrozenOperationItem> items, std::vector<PreservationIssue> issues,
    std::vector<DestinationConflict> conflicts, OperationPlanLimits limits) {
    if (limits.max_items == 0 || limits.max_issues == 0 || limits.max_conflicts == 0)
        return rejected<Pointer>(AdapterError::InvalidLimit, "operation_plan.limits");
    if (items.empty() || items.size() > limits.max_items || issues.size() > limits.max_issues ||
        conflicts.size() > limits.max_conflicts)
        return rejected<Pointer>(AdapterError::PlanInvalid, "operation_plan.bounds");

    const AdapterScope source_scope = items.front().source.scope;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (item.source.scheme == IdentityScheme::Unknown || item.source.scope != source_scope)
            return rejected<Pointer>(AdapterError::UnqualifiedIdentity, "operation_plan.identity");
        if (item.topology != SourceTargetTopology::Disjoint)
            return rejected<Pointer>(AdapterError::PlanTopologyUnsafe, "operation_plan.topology");
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (compare_identity(items[prior].source, item.source) == IdentityComparison::Same) {
                if (!items[prior].hard_link_group || !item.hard_link_group ||
                    *items[prior].hard_link_group != *item.hard_link_group)
                    return rejected<Pointer>(AdapterError::PlanInvalid, "operation_plan.duplicate_source");
            }
        }
    }
    for (std::size_t index = 0; index < items.size(); ++index)
        if (items[index].hard_link_group) {
            for (std::size_t prior = 0; prior < index; ++prior)
                if (items[prior].hard_link_group && *items[prior].hard_link_group == *items[index].hard_link_group &&
                    compare_identity(items[prior].source, items[index].source) != IdentityComparison::Same)
                    return rejected<Pointer>(AdapterError::PlanInvalid, "operation_plan.hard_link_group");
        }
    for (const auto& issue : issues)
        if (issue.item >= items.size()) return rejected<Pointer>(AdapterError::PlanInvalid, "operation_plan.issue_item");
    for (const auto& conflict : conflicts)
        if (conflict.item >= items.size()) return rejected<Pointer>(AdapterError::PlanInvalid, "operation_plan.conflict_item");

    return Observation<Pointer>::available(Pointer(new OperationPlan(std::move(operation), std::move(items),
        std::move(issues), std::move(conflicts))));
}

Observation<OperationPlan::Pointer> OperationPlan::create_from_inventory(FileOperation operation,
    std::shared_ptr<const FrozenInventory> inventory, std::span<const SourceTargetTopology> topologies,
    std::vector<PreservationIssue> issues, std::vector<DestinationConflict> conflicts, OperationPlanLimits limits) {
    if (!inventory || inventory->entries().empty() || topologies.size() != inventory->entries().size() ||
        inventory->hard_link_groups().size() != inventory->entries().size())
        return rejected<Pointer>(AdapterError::PlanInvalid, "operation_plan.inventory_shape");

    std::vector<FrozenOperationItem> items;
    items.reserve(inventory->entries().size());
    for (std::size_t index = 0; index < inventory->entries().size(); ++index) {
        const auto group = inventory->hard_link_groups()[index];
        if (group > index || group >= inventory->entries().size() ||
            compare_identity(inventory->entries()[group].identity, inventory->entries()[index].identity) !=
                IdentityComparison::Same)
            return rejected<Pointer>(AdapterError::PlanInvalid, "operation_plan.inventory_group");
        items.push_back({inventory->entries()[index].identity, inventory->entries()[index].kind,
            topologies[index], group});
    }
    return create(operation, std::move(items), std::move(issues), std::move(conflicts), limits);
}

OperationPlan::OperationPlan(FileOperation operation, std::vector<FrozenOperationItem> items,
    std::vector<PreservationIssue> issues, std::vector<DestinationConflict> conflicts) noexcept
    : operation_(operation), items_(std::move(items)), issues_(std::move(issues)), conflicts_(std::move(conflicts)),
      identity_(PlanIdentity(std::make_shared<const PlanToken>())) {}

bool OperationPlan::indices_valid_and_unique(const std::vector<std::size_t>& selected, std::size_t count) noexcept {
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if (selected[index] >= count) return false;
        if (std::find(selected.begin(), selected.begin() + static_cast<std::ptrdiff_t>(index), selected[index]) !=
            selected.begin() + static_cast<std::ptrdiff_t>(index))
            return false;
    }
    return true;
}

bool OperationPlan::all_indices_present(const std::vector<std::size_t>& selected, std::size_t count) noexcept {
    if (selected.size() != count) return false;
    for (std::size_t index = 0; index < count; ++index)
        if (!contains(selected, index)) return false;
    return true;
}

bool OperationPlan::conflict_action_valid(ConsentAction action, const std::vector<std::size_t>& selected,
    const std::vector<NativeName>& keep_both_names) const noexcept {
    if (selected.empty() || !indices_valid_and_unique(selected, conflicts_.size())) return false;
    if (action == ConsentAction::KeepBoth) {
        if (keep_both_names.size() != selected.size()) return false;
        for (const auto& name : keep_both_names)
            if (!name.is_valid()) return false;
    } else if (!keep_both_names.empty()) return false;
    for (const std::size_t index : selected) {
        const auto& conflict = conflicts_[index];
        const auto kind = items_[conflict.item].kind;
        switch (action) {
        case ConsentAction::SkipConflict:
        case ConsentAction::KeepBoth:
            break;
        case ConsentAction::Replace:
            if (conflict.kind != DestinationConflictKind::ExistingFile || kind == ObjectKind::Directory)
                return false;
            break;
        case ConsentAction::MergeDirectories:
            if (conflict.kind != DestinationConflictKind::ExistingDirectory || kind != ObjectKind::Directory)
                return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

Observation<ConsentReceipt> OperationPlan::bind(ConsentAction action,
    std::vector<std::size_t> reviewed_issues, std::vector<std::size_t> resolved_conflicts,
    std::vector<NativeName> keep_both_names) const {
    if (!indices_valid_and_unique(reviewed_issues, issues_.size()) ||
        !indices_valid_and_unique(resolved_conflicts, conflicts_.size()) ||
        (action != ConsentAction::KeepBoth && !keep_both_names.empty()))
        return rejected<ConsentReceipt>(AdapterError::PlanInvalid, "operation_plan.review_indices");

    if (action == ConsentAction::Cancel)
        return Observation<ConsentReceipt>::available(ConsentReceipt{identity_, action, {}, {}, {}});

    if (action == ConsentAction::ChooseAnotherDestination) {
        if (!requires_review()) return rejected<ConsentReceipt>(AdapterError::PlanInvalid, "operation_plan.no_review");
        return Observation<ConsentReceipt>::available(ConsentReceipt{identity_, action,
            std::move(reviewed_issues), std::move(resolved_conflicts), {}});
    }
    if (action == ConsentAction::ContinueWithListedLosses || action == ConsentAction::SkipAffectedItems) {
        if (!all_indices_present(reviewed_issues, issues_.size()) || issues_.empty() || !resolved_conflicts.empty() ||
            !keep_both_names.empty())
            return rejected<ConsentReceipt>(AdapterError::ProviderReviewRequired, "operation_plan.unreviewed_issue");
        if (action == ConsentAction::ContinueWithListedLosses) {
            for (const auto& issue : issues_)
                if (issue.hard_failure || issue.state == PreservationIssueState::Unknown ||
                    issue.consequence == PreservationConsequence::Uncertain)
                    return rejected<ConsentReceipt>(AdapterError::ProviderReviewRequired, "operation_plan.uncertain_loss");
        }
        return Observation<ConsentReceipt>::available(ConsentReceipt{identity_, action,
            std::move(reviewed_issues), std::move(resolved_conflicts), {}});
    }
    if (reviewed_issues.empty() && conflict_action_valid(action, resolved_conflicts, keep_both_names))
        return Observation<ConsentReceipt>::available(ConsentReceipt{identity_, action,
            std::move(reviewed_issues), std::move(resolved_conflicts), std::move(keep_both_names)});
    return rejected<ConsentReceipt>(AdapterError::ProviderReviewRequired, "operation_plan.conflict_review");
}

bool OperationPlan::receipt_matches(const ConsentReceipt& receipt) const noexcept {
    if (!(receipt.plan == identity_)) return false;
    if (!indices_valid_and_unique(receipt.reviewed_issues, issues_.size()) ||
        !indices_valid_and_unique(receipt.resolved_conflicts, conflicts_.size()))
        return false;
    switch (receipt.action) {
    case ConsentAction::Cancel:
        return receipt.reviewed_issues.empty() && receipt.resolved_conflicts.empty() && receipt.keep_both_names.empty();
    case ConsentAction::ChooseAnotherDestination:
        return requires_review() && receipt.keep_both_names.empty();
    case ConsentAction::ContinueWithListedLosses:
        if (!receipt.resolved_conflicts.empty() || !receipt.keep_both_names.empty() ||
            !all_indices_present(receipt.reviewed_issues, issues_.size()) || issues_.empty())
            return false;
        for (const auto& issue : issues_)
            if (issue.hard_failure || issue.state == PreservationIssueState::Unknown ||
                issue.consequence == PreservationConsequence::Uncertain)
                return false;
        return true;
    case ConsentAction::SkipAffectedItems:
        return receipt.resolved_conflicts.empty() && receipt.keep_both_names.empty() &&
            all_indices_present(receipt.reviewed_issues, issues_.size()) && !issues_.empty();
    case ConsentAction::SkipConflict:
    case ConsentAction::KeepBoth:
    case ConsentAction::Replace:
    case ConsentAction::MergeDirectories:
        return receipt.reviewed_issues.empty() &&
            conflict_action_valid(receipt.action, receipt.resolved_conflicts, receipt.keep_both_names);
    }
    return false;
}

} // namespace ck::files
