// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "native_name.hpp"
#include "observation.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ck::files {

// The plan layer names preservation families without claiming that a family is
// actually supported. Concrete inventory observations remain worker-owned.
enum class PreservationFeature {
    MainData,
    AdditionalChannels,
    ExtendedAttributes,
    PermissionsOwnership,
    TimesAndFlags,
    LinksAndIdentity,
    StorageRepresentation,
    NamesAndStructure,
    ProviderManagedObject
};

enum class PreservationIssueState { Missing, Unknown, Unsupported, Denied, Changed, Conflict };
enum class PreservationConsequence { DataLoss, SecurityChange, MetadataLoss, RepresentationChange, Uncertain };

struct PreservationIssue final {
    std::size_t item = 0;
    PreservationFeature feature = PreservationFeature::MainData;
    PreservationIssueState state = PreservationIssueState::Unknown;
    PreservationConsequence consequence = PreservationConsequence::Uncertain;
    // Hard failures cannot be authorized by ContinueWithListedLosses.
    bool hard_failure = false;
};

enum class SourceTargetTopology { Disjoint, SameObject, DestinationInsideSource, Unknown };
enum class DestinationConflictKind { ExistingFile, ExistingDirectory, TypeMismatch, CaseCollision, Unknown };

struct DestinationConflict final {
    std::size_t item = 0;
    DestinationConflictKind kind = DestinationConflictKind::Unknown;
};

enum class FileOperation { Copy, Move };
enum class ConsentAction {
    Cancel,
    ChooseAnotherDestination,
    Proceed,
    SkipAffectedItems,
    ContinueWithListedLosses,
    SkipConflict,
    KeepBoth,
    Replace,
    MergeDirectories
};

struct OperationPlanLimits final {
    std::size_t max_items = 1'000'000;
    std::size_t max_issues = 1'000'000;
    std::size_t max_conflicts = 1'000'000;
};

struct FrozenOperationItem final {
    ObjectIdentity source;
    ObjectKind kind = ObjectKind::Other;
    SourceTargetTopology topology = SourceTargetTopology::Unknown;
    // A recursive inventory may contain several names for one hard-linked
    // object. The inventory supplies this group only after qualifying each
    // name; an ordinary selection leaves it absent and duplicate identities
    // remain invalid.
    std::optional<std::size_t> hard_link_group;
};

struct PlanToken;
class FrozenInventory;

class PlanIdentity final {
public:
    bool is_valid() const noexcept { return token_ != nullptr; }
    friend bool operator==(const PlanIdentity&, const PlanIdentity&) = default;

private:
    explicit PlanIdentity(std::shared_ptr<const PlanToken> token) noexcept : token_(std::move(token)) {}
    std::shared_ptr<const PlanToken> token_;
    friend class OperationPlan;
};

struct ConsentReceipt final {
    PlanIdentity plan;
    ConsentAction action = ConsentAction::Cancel;
    std::vector<std::size_t> reviewed_issues;
    std::vector<std::size_t> resolved_conflicts;
    // One valid destination basename per resolved KeepBoth conflict. Native
    // alias/collision checks remain execution-time responsibilities.
    std::vector<NativeName> keep_both_names;
};

class OperationPlan final {
public:
    using Pointer = std::shared_ptr<const OperationPlan>;

    static Observation<Pointer> create(FileOperation operation, std::vector<FrozenOperationItem> items,
        std::vector<PreservationIssue> issues = {}, std::vector<DestinationConflict> conflicts = {},
        OperationPlanLimits limits = {});
    // Builds one item per sealed inventory entry. Hard-link aliases retain
    // their entry identity and group instead of being silently deduplicated;
    // the execution layer can use that mapping when preserving names.
    static Observation<Pointer> create_from_inventory(FileOperation operation,
        std::shared_ptr<const FrozenInventory> inventory, std::span<const SourceTargetTopology> topologies,
        std::vector<PreservationIssue> issues = {}, std::vector<DestinationConflict> conflicts = {},
        OperationPlanLimits limits = {});

    FileOperation operation() const noexcept { return operation_; }
    const std::vector<FrozenOperationItem>& items() const noexcept { return items_; }
    const std::vector<PreservationIssue>& issues() const noexcept { return issues_; }
    const std::vector<DestinationConflict>& conflicts() const noexcept { return conflicts_; }
    PlanIdentity identity() const noexcept { return identity_; }
    bool requires_review() const noexcept { return !issues_.empty() || !conflicts_.empty(); }

    // A receipt is a review result only. It never authorizes a filesystem
    // mutation; a later execution engine must revalidate the frozen plan.
    Observation<ConsentReceipt> bind(ConsentAction action,
        std::vector<std::size_t> reviewed_issues = {},
        std::vector<std::size_t> resolved_conflicts = {},
        std::vector<NativeName> keep_both_names = {}) const;
    // Execution must compare fresh worker evidence before using a receipt.
    // Any source, topology, preservation or conflict change makes the
    // decision stale; this method performs no I/O or mutation.
    Observation<ConsentReceipt> revalidate(const ConsentReceipt& receipt,
        std::span<const FrozenOperationItem> items, std::span<const PreservationIssue> issues,
        std::span<const DestinationConflict> conflicts) const;
    bool receipt_matches(const ConsentReceipt& receipt) const noexcept;

private:
    OperationPlan(FileOperation operation, std::vector<FrozenOperationItem> items,
        std::vector<PreservationIssue> issues, std::vector<DestinationConflict> conflicts) noexcept;

    static bool all_indices_present(const std::vector<std::size_t>& selected, std::size_t count) noexcept;
    static bool indices_valid_and_unique(const std::vector<std::size_t>& selected, std::size_t count) noexcept;
    bool conflict_action_valid(ConsentAction action, const std::vector<std::size_t>& selected,
        const std::vector<NativeName>& keep_both_names) const noexcept;

    FileOperation operation_;
    std::vector<FrozenOperationItem> items_;
    std::vector<PreservationIssue> issues_;
    std::vector<DestinationConflict> conflicts_;
    PlanIdentity identity_;
};

} // namespace ck::files
