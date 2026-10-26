// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frozen_inventory.hpp"

#include <limits>

namespace ck::files {
namespace {

bool link_kind(ObjectKind kind) noexcept {
    return kind == ObjectKind::SymbolicLink || kind == ObjectKind::Junction || kind == ObjectKind::ReparsePoint;
}

} // namespace

FrozenInventoryBuilder::FrozenInventoryBuilder(InventoryMountPolicy mount_policy,
    InventoryLinkPolicy link_policy, FrozenInventoryLimits limits)
    : mount_policy_(mount_policy), link_policy_(link_policy), limits_(limits) {
    if (!limits_.max_entries || limits_.max_entries > FrozenInventoryLimits::hard_max_entries ||
        !limits_.max_logical_bytes || limits_.max_logical_bytes > FrozenInventoryLimits::hard_max_logical_bytes)
        fail(AdapterError::InvalidLimit, "frozen_inventory.limits");
}

bool FrozenInventoryBuilder::fail(AdapterError code, const char* operation, ObservationState state) noexcept {
    failure_ = UnavailableObservation{state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
    state_ = FrozenInventoryState::Failed;
    entries_.clear();
    hard_link_groups_.clear();
    root_identity_.reset();
    root_mount_id_.reset();
    logical_bytes_ = 0;
    return false;
}

bool FrozenInventoryBuilder::valid_entry(const FrozenInventoryEntry& entry) const noexcept {
    return entry.source_path.is_valid() && entry.identity.scheme != IdentityScheme::Unknown &&
        entry.identity.volume != NativeIdentifier{} && entry.identity.object != NativeIdentifier{};
}

bool FrozenInventoryBuilder::valid_link(const FrozenInventoryEntry& entry) const noexcept {
    if (!link_kind(entry.kind)) return entry.link == LinkTraversalEvidence::NotLink && !entry.link_target;
    if (link_policy_ == InventoryLinkPolicy::PreserveLinks)
        return entry.link == LinkTraversalEvidence::NotFollowed && !entry.link_target;
    if (entry.link != LinkTraversalEvidence::Followed || !entry.link_target ||
        entry.link_target->scope != entry.identity.scope || entry.link_target->scheme == IdentityScheme::Unknown ||
        entry.link_target->volume == NativeIdentifier{} || entry.link_target->object == NativeIdentifier{} ||
        compare_identity(*entry.link_target, entry.identity) == IdentityComparison::Same)
        return false;
    return true;
}

bool FrozenInventoryBuilder::valid_mount(const FrozenInventoryEntry& entry) const noexcept {
    if (mount_policy_ == InventoryMountPolicy::DirectObjects) return true;
    if (mount_policy_ == InventoryMountPolicy::AllowCrossMounts) return entry.mount_id.state() == ObservationState::Available;
    if (entry.mount_id.state() != ObservationState::Available) return false;
    const auto value = *entry.mount_id.value();
    return !root_mount_id_ || *root_mount_id_ == value;
}

std::size_t FrozenInventoryBuilder::group_for(const ObjectIdentity& identity) const noexcept {
    for (std::size_t index = 0; index < entries_.size(); ++index)
        if (compare_identity(entries_[index].identity, identity) == IdentityComparison::Same)
            return hard_link_groups_[index];
    return entries_.size();
}

bool FrozenInventoryBuilder::add(FrozenInventoryEntry entry) {
    if (state_ != FrozenInventoryState::Collecting) return false;
    if (entries_.size() >= limits_.max_entries) return fail(AdapterError::InventoryLimit, "frozen_inventory.entries", ObservationState::Unknown);
    if (!valid_entry(entry)) return fail(AdapterError::UnqualifiedIdentity, "frozen_inventory.entry");
    if (!valid_link(entry)) return fail(AdapterError::PlanTopologyUnsafe, "frozen_inventory.link");
    if (!valid_mount(entry)) return fail(AdapterError::MetadataChanged, "frozen_inventory.mount", ObservationState::Unknown);
    if (entry.logical_size > limits_.max_logical_bytes - logical_bytes_)
        return fail(AdapterError::InventoryLimit, "frozen_inventory.bytes", ObservationState::Unknown);
    if (!root_identity_) {
        root_identity_ = entry.identity;
        if (mount_policy_ != InventoryMountPolicy::DirectObjects)
            root_mount_id_ = *entry.mount_id.value();
    } else if (entry.identity.scope != root_identity_->scope) {
        return fail(AdapterError::UnqualifiedIdentity, "frozen_inventory.scope");
    }

    const std::size_t group = group_for(entry.identity);
    entries_.push_back(std::move(entry));
    hard_link_groups_.push_back(group);
    logical_bytes_ += entries_.back().logical_size;
    return true;
}

bool FrozenInventoryBuilder::seal() noexcept {
    if (state_ != FrozenInventoryState::Collecting || entries_.empty()) return false;
    state_ = FrozenInventoryState::Complete;
    return true;
}

bool FrozenInventoryBuilder::abandon() noexcept {
    if (state_ == FrozenInventoryState::Complete || state_ == FrozenInventoryState::Failed ||
        state_ == FrozenInventoryState::Abandoned)
        return false;
    state_ = FrozenInventoryState::Abandoned;
    entries_.clear();
    hard_link_groups_.clear();
    root_identity_.reset();
    root_mount_id_.reset();
    logical_bytes_ = 0;
    return true;
}

Observation<FrozenInventory::Pointer> FrozenInventoryBuilder::completed() const {
    using Result = Observation<FrozenInventory::Pointer>;
    if (state_ != FrozenInventoryState::Complete)
        return failure_ ? Result::unavailable(failure_->state, failure_->error) :
            Result::unavailable(ObservationState::Unknown, {ErrorDomain::Adapter,
                static_cast<std::int64_t>(AdapterError::ProviderReviewRequired), "frozen_inventory.incomplete"});
    return Result::available(FrozenInventory::Pointer(new FrozenInventory(mount_policy_, link_policy_, entries_,
        hard_link_groups_, *root_identity_, logical_bytes_)));
}

} // namespace ck::files
