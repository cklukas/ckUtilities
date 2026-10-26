// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frozen_inventory.hpp"

#include "cvision/testing/cktest.hpp"

#include <stdexcept>
#include <utility>

namespace {
using namespace ck::files;

NativeAbsolutePath path(const char* value) {
    auto result = NativeAbsolutePath::from_posix(value);
    if (!result.value()) throw std::runtime_error("frozen inventory path fixture unavailable");
    return std::move(result).take_value();
}

ObjectIdentity identity(const AdapterScope& scope, std::uint64_t object) {
    return {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(7),
        NativeIdentifier::from_u64(object), {}};
}

FrozenInventoryEntry entry(const AdapterScope& scope, const char* value, std::uint64_t object,
    ObjectKind kind = ObjectKind::RegularFile, std::uint64_t mount = 4,
    LinkTraversalEvidence link = LinkTraversalEvidence::NotLink, std::optional<ObjectIdentity> target = {}) {
    return {path(value), identity(scope, object), kind, Observation<std::uint64_t>::available(mount), 10, link,
        std::move(target)};
}

FrozenInventory::Pointer take(FrozenInventoryBuilder& builder) {
    auto result = builder.completed();
    if (!result.value()) throw std::runtime_error("frozen inventory fixture incomplete");
    return std::move(result).take_value();
}
} // namespace

CK_TEST(frozen_inventory_seals_worker_records_and_retains_hard_link_groups) {
    const AdapterScope scope;
    FrozenInventoryBuilder builder(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(builder.add(entry(scope, "/root/a", 1)));
    CK_CHECK(builder.add(entry(scope, "/root/alias", 1)));
    CK_CHECK(builder.seal() && builder.state() == FrozenInventoryState::Complete);
    const auto result = take(builder);
    CK_CHECK(result->entries().size() == 2 && result->hard_link_groups().size() == 2 &&
        result->hard_link_groups()[0] == result->hard_link_groups()[1] && result->logical_bytes() == 20);
    CK_CHECK(!builder.add(entry(scope, "/root/late", 2)));
}

CK_TEST(frozen_inventory_mount_policy_refuses_crossing_or_unknown_mounts) {
    const AdapterScope scope;
    FrozenInventoryBuilder stay(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(stay.add(entry(scope, "/root/a", 1, ObjectKind::RegularFile, 4)));
    CK_CHECK(!stay.add(entry(scope, "/other/b", 2, ObjectKind::RegularFile, 5)));
    CK_CHECK(stay.failure()->error.code == static_cast<std::int64_t>(AdapterError::MetadataChanged));

    FrozenInventoryBuilder cross(InventoryMountPolicy::AllowCrossMounts, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(cross.add(entry(scope, "/root/a", 1, ObjectKind::RegularFile, 4)));
    CK_CHECK(cross.add(entry(scope, "/other/b", 2, ObjectKind::RegularFile, 5)));
    auto unknown = entry(scope, "/unknown/c", 3);
    unknown.mount_id = Observation<std::uint64_t>::unavailable(ObservationState::Unknown,
        {ErrorDomain::Posix, 5, "fixture.mount"});
    CK_CHECK(!cross.add(std::move(unknown)));
}

CK_TEST(frozen_inventory_direct_objects_preserve_unknown_mount_without_traversal) {
    const AdapterScope scope;
    FrozenInventoryBuilder direct(InventoryMountPolicy::DirectObjects, InventoryLinkPolicy::PreserveLinks);
    auto value = entry(scope, "/direct/file", 1);
    value.mount_id = Observation<std::uint64_t>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "fixture.mount_not_applicable"});
    CK_CHECK(direct.add(std::move(value)) && direct.seal());
    const auto result = take(direct);
    CK_CHECK(result->entries().size() == 1 && !result->entries().front().mount_id.value());
}

CK_TEST(frozen_inventory_link_policy_never_follows_without_explicit_target_evidence) {
    const AdapterScope scope;
    FrozenInventoryBuilder preserve(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(preserve.add(entry(scope, "/root/link", 1, ObjectKind::SymbolicLink, 4,
        LinkTraversalEvidence::NotFollowed)));

    FrozenInventoryBuilder follow_invalid(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::FollowLinks);
    CK_CHECK(!follow_invalid.add(entry(scope, "/root/link", 1, ObjectKind::SymbolicLink, 4,
        LinkTraversalEvidence::NotFollowed)));
    FrozenInventoryBuilder follow(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::FollowLinks);
    CK_CHECK(follow.add(entry(scope, "/root/link", 1, ObjectKind::SymbolicLink, 4,
        LinkTraversalEvidence::Followed, identity(scope, 2))));
    CK_CHECK(!follow.add(entry(scope, "/root/self", 4, ObjectKind::SymbolicLink, 4,
        LinkTraversalEvidence::Followed, identity(scope, 4))));
    CK_CHECK(!follow.add(entry(scope, "/root/cycle", 3, ObjectKind::SymbolicLink, 4,
        LinkTraversalEvidence::Cycle)));
}

CK_TEST(frozen_inventory_limits_and_identity_scope_fail_closed) {
    const AdapterScope scope;
    FrozenInventoryBuilder limited(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks,
        {1, 10});
    CK_CHECK(limited.add(entry(scope, "/root/a", 1)));
    CK_CHECK(!limited.add(entry(scope, "/root/b", 2)));
    CK_CHECK(limited.failure()->state == ObservationState::Unknown &&
        limited.failure()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));

    FrozenInventoryBuilder scoped(InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(scoped.add(entry(scope, "/root/a", 1)));
    const AdapterScope foreign_scope;
    CK_CHECK(!scoped.add(entry(foreign_scope, "/root/foreign", 2)));
    CK_CHECK(scoped.failure()->error.code == static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity));
}
