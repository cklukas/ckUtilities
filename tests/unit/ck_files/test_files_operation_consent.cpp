// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_consent.hpp"
#include "frozen_inventory.hpp"

#include "cvision/testing/cktest.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;

FrozenOperationItem item(std::uint64_t id, ObjectKind kind = ObjectKind::RegularFile,
    SourceTargetTopology topology = SourceTargetTopology::Disjoint) {
    return {ObjectIdentity{AdapterScope{}, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(id), {}}, kind, topology, std::nullopt};
}

template <class T>
T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("operation consent fixture unavailable");
    return std::move(value).take_value();
}

OperationPlan::Pointer plan(std::vector<FrozenOperationItem> items,
    std::vector<PreservationIssue> issues = {}, std::vector<DestinationConflict> conflicts = {}) {
    return take(OperationPlan::create(FileOperation::Copy, std::move(items), std::move(issues), std::move(conflicts)));
}

FrozenInventory::Pointer inventory_with_hard_link_aliases() {
    FrozenInventoryBuilder builder(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    const AdapterScope scope;
    const auto make = [&](const char* text, std::uint64_t object) {
        auto native = NativeAbsolutePath::from_posix(text);
        if (!native.value()) throw std::runtime_error("operation inventory fixture path unavailable");
        return FrozenInventoryEntry{std::move(native).take_value(),
            ObjectIdentity{scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1),
                NativeIdentifier::from_u64(object), {}}, ObjectKind::RegularFile,
            Observation<std::uint64_t>::available(1), 1, LinkTraversalEvidence::NotLink, {}, {}};
    };
    if (!builder.add(make("/source/a", 9)) || !builder.add(make("/source/alias", 9)) || !builder.seal())
        throw std::runtime_error("operation inventory fixture unavailable");
    return std::move(builder.completed()).take_value();
}
} // namespace

CK_TEST(operation_plan_requires_qualified_disjoint_unique_sources) {
    const auto duplicate = item(4);
    const auto duplicate_result = OperationPlan::create(FileOperation::Copy, {duplicate, duplicate});
    CK_CHECK(duplicate_result.state() == ObservationState::Failed);
    CK_CHECK(duplicate_result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));

    const auto unsafe = OperationPlan::create(FileOperation::Move, {item(5, ObjectKind::Directory,
        SourceTargetTopology::DestinationInsideSource)});
    CK_CHECK(unsafe.state() == ObservationState::Failed);
    CK_CHECK(unsafe.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanTopologyUnsafe));

    auto unqualified = item(6);
    unqualified.source.scheme = IdentityScheme::Unknown;
    const auto unqualified_result = OperationPlan::create(FileOperation::Copy, {unqualified});
    CK_CHECK(unqualified_result.state() == ObservationState::Failed);
    CK_CHECK(unqualified_result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity));
}

CK_TEST(operation_consent_cancel_is_always_safe_and_continue_requires_complete_known_review) {
    const std::vector issues{
        PreservationIssue{0, PreservationFeature::AdditionalChannels, PreservationIssueState::Missing,
            PreservationConsequence::DataLoss, false},
        PreservationIssue{0, PreservationFeature::TimesAndFlags, PreservationIssueState::Changed,
            PreservationConsequence::MetadataLoss, false}};
    const auto operation = plan({item(7)}, issues);
    const auto cancel = take(operation->bind(ConsentAction::Cancel));
    CK_CHECK(operation->receipt_matches(cancel) && cancel.action == ConsentAction::Cancel);
    CK_CHECK(operation->bind(ConsentAction::ContinueWithListedLosses, {0}).state() == ObservationState::Failed);

    const auto accepted = take(operation->bind(ConsentAction::ContinueWithListedLosses, {1, 0}));
    CK_CHECK(operation->receipt_matches(accepted) && accepted.reviewed_issues.size() == 2);
    auto forged = accepted;
    forged.action = ConsentAction::Cancel;
    CK_CHECK(!operation->receipt_matches(forged));

    const auto uncertain = plan({item(8)}, {PreservationIssue{0, PreservationFeature::MainData,
        PreservationIssueState::Unknown, PreservationConsequence::Uncertain, true}});
    CK_CHECK(uncertain->bind(ConsentAction::ContinueWithListedLosses, {0}).state() == ObservationState::Failed);
    CK_CHECK(uncertain->bind(ConsentAction::ChooseAnotherDestination).value());

    const auto known_metadata_loss = plan({item(14)}, {
        PreservationIssue{0, PreservationFeature::ExtendedAttributes, PreservationIssueState::Denied,
            PreservationConsequence::MetadataLoss, true},
        PreservationIssue{0, PreservationFeature::PermissionsOwnership, PreservationIssueState::Denied,
            PreservationConsequence::SecurityChange, true}});
    const auto reviewed_metadata_loss = take(
        known_metadata_loss->bind(ConsentAction::ContinueWithListedLosses, {0, 1}));
    CK_CHECK(known_metadata_loss->receipt_matches(reviewed_metadata_loss));
}

CK_TEST(operation_consent_clean_plan_requires_explicit_proceed) {
    const auto operation = plan({item(15)});
    const auto proceed = take(operation->bind(ConsentAction::Proceed));
    CK_CHECK(proceed.action == ConsentAction::Proceed && operation->receipt_matches(proceed));
    CK_CHECK(operation->bind(ConsentAction::ContinueWithListedLosses).state() == ObservationState::Failed);
}

CK_TEST(operation_consent_receipts_are_bound_to_the_immutable_plan_identity) {
    const auto first = plan({item(9)});
    const auto receipt = take(first->bind(ConsentAction::Cancel));
    const auto second = plan({item(10)});
    CK_CHECK(first->receipt_matches(receipt));
    CK_CHECK(!second->receipt_matches(receipt));
    CK_CHECK(first->identity() == first->identity());
    CK_CHECK(first->identity() != second->identity());
}

CK_TEST(operation_consent_conflicts_require_type_specific_review) {
    const auto file_conflict = plan({item(11)}, {}, {{0, DestinationConflictKind::ExistingFile}});
    CK_CHECK(file_conflict->bind(ConsentAction::Replace, {}, {0}).value());
    CK_CHECK(!file_conflict->bind(ConsentAction::MergeDirectories, {}, {0}).value());

    const auto directory_conflict = plan({item(12, ObjectKind::Directory)}, {},
        {{0, DestinationConflictKind::ExistingDirectory}});
    CK_CHECK(directory_conflict->bind(ConsentAction::MergeDirectories, {}, {0}).value());
    CK_CHECK(!directory_conflict->bind(ConsentAction::Replace, {}, {0}).value());

    const auto mismatch = plan({item(13)}, {}, {{0, DestinationConflictKind::TypeMismatch}});
    CK_CHECK(!mismatch->bind(ConsentAction::Replace, {}, {0}).value());
    CK_CHECK(mismatch->bind(ConsentAction::SkipConflict, {}, {0}).value());

    const auto keep_both_name = NativeName::from_posix("copy");
    CK_CHECK(keep_both_name.has_value());
    CK_CHECK(!mismatch->bind(ConsentAction::KeepBoth, {}, {0}).value());
    CK_CHECK(mismatch->bind(ConsentAction::KeepBoth, {}, {0}, {*keep_both_name}).value());
    auto moved_name = NativeName::from_posix("copy");
    CK_CHECK(moved_name.has_value());
    auto valid_name = std::move(*moved_name);
    (void)valid_name;
    CK_CHECK(!moved_name->is_valid());
    CK_CHECK(!mismatch->bind(ConsentAction::KeepBoth, {}, {0}, {std::move(*moved_name)}).value());
}

CK_TEST(operation_plan_from_frozen_inventory_preserves_hard_link_aliases) {
    const auto inventory = inventory_with_hard_link_aliases();
    const std::array topologies{SourceTargetTopology::Disjoint, SourceTargetTopology::Disjoint};
    const auto result = OperationPlan::create_from_inventory(FileOperation::Copy, inventory, topologies);
    CK_CHECK(result.value() && (*result.value())->items().size() == 2);
    CK_CHECK((*result.value())->items()[0].hard_link_group && (*result.value())->items()[1].hard_link_group &&
        *(*result.value())->items()[0].hard_link_group == *(*result.value())->items()[1].hard_link_group);

    const auto unsafe = std::array{SourceTargetTopology::DestinationInsideSource,
        SourceTargetTopology::Disjoint};
    CK_CHECK(OperationPlan::create_from_inventory(FileOperation::Move, inventory, unsafe).state() ==
        ObservationState::Failed);
}

CK_TEST(operation_consent_skip_conflicts_requires_consent_only_for_transferred_items) {
    auto first = item(21), second = item(22), third = item(23);
    second.source.scope = first.source.scope;
    third.source.scope = first.source.scope;
    const auto operation = plan({first, second, third}, {
        {0, PreservationFeature::MainData, PreservationIssueState::Unknown, PreservationConsequence::Uncertain, true},
        {1, PreservationFeature::ExtendedAttributes, PreservationIssueState::Missing,
            PreservationConsequence::MetadataLoss, false}
    }, {{0, DestinationConflictKind::ExistingFile}, {2, DestinationConflictKind::ExistingFile}});
    CK_CHECK(!operation->bind(ConsentAction::SkipConflict, {}, {0, 1}).value());
    CK_CHECK(!operation->bind(ConsentAction::SkipConflict, {1}, {0}).value());
    CK_CHECK(!operation->bind(ConsentAction::SkipConflict, {1}, {0, 0}).value());
    const auto receipt = take(operation->bind(ConsentAction::SkipConflict, {1}, {0, 1}));
    CK_CHECK(operation->receipt_matches(receipt));
    auto unchecked = receipt;
    unchecked.reviewed_issues.clear();
    CK_CHECK(!operation->receipt_matches(unchecked));
}

CK_TEST(operation_consent_keep_both_binds_names_and_requires_every_reviewable_loss) {
    const auto operation = plan({item(24)}, {
        {0, PreservationFeature::ExtendedAttributes, PreservationIssueState::Missing,
            PreservationConsequence::MetadataLoss, false}
    }, {{0, DestinationConflictKind::ExistingFile}});
    const auto name = *NativeName::from_posix("incoming");
    CK_CHECK(!operation->bind(ConsentAction::KeepBoth, {}, {0}, {name}).value());
    const auto receipt = take(operation->bind(ConsentAction::KeepBoth, {0}, {0}, {name}));
    CK_CHECK(operation->receipt_matches(receipt));
    CK_CHECK(receipt.keep_both_names == std::vector<NativeName>{name});
    auto unchecked = receipt;
    unchecked.reviewed_issues.clear();
    CK_CHECK(!operation->receipt_matches(unchecked));
}

CK_TEST(operation_plan_from_frozen_inventory_rejects_incomplete_topology_evidence) {
    const auto inventory = inventory_with_hard_link_aliases();
    auto groups = inventory->hard_link_groups();
    (void)groups;
    const std::array topologies{SourceTargetTopology::Disjoint};
    CK_CHECK(OperationPlan::create_from_inventory(FileOperation::Copy, inventory, topologies).state() ==
        ObservationState::Failed);
}

CK_TEST(operation_consent_revalidation_invalidates_changed_worker_evidence) {
    const auto operation = plan({item(14)},
        {PreservationIssue{0, PreservationFeature::TimesAndFlags, PreservationIssueState::Missing,
            PreservationConsequence::MetadataLoss, false}},
        {{0, DestinationConflictKind::ExistingFile}});
    const auto receipt = take(operation->bind(ConsentAction::ChooseAnotherDestination));
    const std::vector<FrozenOperationItem> same_items = operation->items();
    const std::vector<PreservationIssue> same_issues{
        {0, PreservationFeature::TimesAndFlags, PreservationIssueState::Missing,
            PreservationConsequence::MetadataLoss, false}};
    const std::vector<DestinationConflict> same_conflicts{{0, DestinationConflictKind::ExistingFile}};
    CK_CHECK(operation->revalidate(receipt, same_items, same_issues, same_conflicts).value());

    auto changed_items = same_items;
    changed_items[0].topology = SourceTargetTopology::Unknown;
    auto stale = operation->revalidate(receipt, changed_items, same_issues, same_conflicts);
    CK_CHECK(stale.state() == ObservationState::Changed &&
        stale.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanDecisionStale));

    auto changed_issue = same_issues;
    changed_issue[0].state = PreservationIssueState::Unknown;
    stale = operation->revalidate(receipt, same_items, changed_issue, same_conflicts);
    CK_CHECK(stale.state() == ObservationState::Changed &&
        stale.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanDecisionStale));

    auto forged = receipt;
    forged.action = ConsentAction::Cancel;
    forged.reviewed_issues = {0};
    CK_CHECK(operation->revalidate(forged, same_items, same_issues, same_conflicts).state() == ObservationState::Failed);
}
