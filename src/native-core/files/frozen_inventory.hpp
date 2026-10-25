// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_path.hpp"
#include "operation_consent.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ck::files {

enum class InventoryMountPolicy { StayOnRootMount, AllowCrossMounts };
enum class InventoryLinkPolicy { PreserveLinks, FollowLinks };
enum class LinkTraversalEvidence { NotLink, NotFollowed, Followed, Cycle, UnknownTarget };

struct FrozenInventoryLimits final {
    std::size_t max_entries = 1'000'000;
    std::uint64_t max_logical_bytes = 1ULL << 50;
    static constexpr std::size_t hard_max_entries = 8'000'000;
    static constexpr std::uint64_t hard_max_logical_bytes = 1ULL << 60;
};

// A worker creates this record only after its held native object and exact
// path/name have been checked. The accumulator never reopens a path or follows
// a link itself.
struct FrozenInventoryEntry final {
    NativeAbsolutePath source_path;
    ObjectIdentity identity;
    ObjectKind kind = ObjectKind::Other;
    Observation<std::uint64_t> mount_id;
    std::uint64_t logical_size = 0;
    LinkTraversalEvidence link = LinkTraversalEvidence::NotLink;
    std::optional<ObjectIdentity> link_target;
};

class FrozenInventory final {
public:
    using Pointer = std::shared_ptr<const FrozenInventory>;

    FrozenInventory(const FrozenInventory&) = delete;
    FrozenInventory& operator=(const FrozenInventory&) = delete;

    const std::vector<FrozenInventoryEntry>& entries() const noexcept { return entries_; }
    const std::vector<std::size_t>& hard_link_groups() const noexcept { return hard_link_groups_; }
    InventoryMountPolicy mount_policy() const noexcept { return mount_policy_; }
    InventoryLinkPolicy link_policy() const noexcept { return link_policy_; }
    std::uint64_t logical_bytes() const noexcept { return logical_bytes_; }
    const ObjectIdentity& root_identity() const noexcept { return root_identity_; }

private:
    friend class FrozenInventoryBuilder;
    FrozenInventory(InventoryMountPolicy mount_policy, InventoryLinkPolicy link_policy,
        std::vector<FrozenInventoryEntry> entries, std::vector<std::size_t> hard_link_groups,
        ObjectIdentity root_identity, std::uint64_t logical_bytes) noexcept
        : mount_policy_(mount_policy), link_policy_(link_policy), entries_(std::move(entries)),
          hard_link_groups_(std::move(hard_link_groups)), root_identity_(std::move(root_identity)),
          logical_bytes_(logical_bytes) {}

    InventoryMountPolicy mount_policy_;
    InventoryLinkPolicy link_policy_;
    std::vector<FrozenInventoryEntry> entries_;
    std::vector<std::size_t> hard_link_groups_;
    ObjectIdentity root_identity_;
    std::uint64_t logical_bytes_ = 0;
};

enum class FrozenInventoryState { Collecting, Complete, Failed, Abandoned };

class FrozenInventoryBuilder final {
public:
    FrozenInventoryBuilder(InventoryMountPolicy mount_policy, InventoryLinkPolicy link_policy,
        FrozenInventoryLimits limits = {});
    FrozenInventoryBuilder(const FrozenInventoryBuilder&) = delete;
    FrozenInventoryBuilder& operator=(const FrozenInventoryBuilder&) = delete;

    bool add(FrozenInventoryEntry entry);
    bool seal() noexcept;
    bool abandon() noexcept;
    FrozenInventoryState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<FrozenInventory::Pointer> completed() const;

private:
    bool fail(AdapterError code, const char* operation, ObservationState state = ObservationState::Failed) noexcept;
    bool valid_entry(const FrozenInventoryEntry& entry) const noexcept;
    bool valid_link(const FrozenInventoryEntry& entry) const noexcept;
    bool valid_mount(const FrozenInventoryEntry& entry) const noexcept;
    std::size_t group_for(const ObjectIdentity& identity) const noexcept;

    InventoryMountPolicy mount_policy_;
    InventoryLinkPolicy link_policy_;
    FrozenInventoryLimits limits_;
    std::vector<FrozenInventoryEntry> entries_;
    std::vector<std::size_t> hard_link_groups_;
    std::optional<UnavailableObservation> failure_;
    std::optional<ObjectIdentity> root_identity_;
    std::optional<std::uint64_t> root_mount_id_;
    std::uint64_t logical_bytes_ = 0;
    FrozenInventoryState state_ = FrozenInventoryState::Collecting;
};

} // namespace ck::files
