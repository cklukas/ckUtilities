// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "recursive_inventory_job.hpp"

#include "cvision/testing/cktest.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace ck::files;

template <class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("recursive inventory fixture unavailable");
    return std::move(value).take_value();
}

NativeAbsolutePath path(const char* value) { return take(NativeAbsolutePath::from_posix(value)); }

struct Node final {
    ObjectIdentity identity;
    ObjectKind kind;
    std::uint64_t mount;
    std::uint64_t size;
    std::vector<std::pair<NativeName, std::shared_ptr<Node>>> children;
};

BasicMetadata metadata(const Node& node) {
    BasicMetadata result;
    result.identity = Observation<ObjectIdentity>::available(node.identity);
    result.kind = Observation<ObjectKind>::available(node.kind);
    result.mount_id = Observation<std::uint64_t>::available(node.mount);
    result.logical_size = Observation<std::uint64_t>::available(node.size);
    return result;
}

std::shared_ptr<Node> node(const AdapterScope& scope, std::uint64_t object, ObjectKind kind,
    std::uint64_t mount = 4, std::uint64_t size = 0) {
    return std::make_shared<Node>(Node{{scope, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(7), NativeIdentifier::from_u64(object), {}}, kind, mount, size, {}});
}

class Entry final : public OpenedEntry {
public:
    explicit Entry(std::shared_ptr<Node> node) : node_(std::move(node)) {}
    Observation<BasicMetadata> inspect_basic() const override {
        return Observation<BasicMetadata>::available(metadata(*node_));
    }
private:
    std::shared_ptr<Node> node_;
};

class Cursor final : public DirectoryCursor {
public:
    explicit Cursor(std::shared_ptr<Node> node)
        : DirectoryCursor(Observation<ObjectIdentity>::available(node->identity)), node_(std::move(node)) {}

    Observation<BasicMetadata> inspect_basic() const override {
        return Observation<BasicMetadata>::available(metadata(*node_));
    }

    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& name, EntryAccess access) const override {
        using Result = Observation<std::unique_ptr<OpenedEntry>>;
        if (access != EntryAccess::MetadataOnly)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "fixture.access"});
        for (const auto& child : node_->children)
            if (child.first == name) return Result::available(std::make_unique<Entry>(child.second));
        return Result::unavailable(ObservationState::Gone,
            {ErrorDomain::Posix, 2, "fixture.child_missing"});
    }

    Observation<std::unique_ptr<DirectoryCursor>> open_directory_child(const NativeName& name) const override {
        using Result = Observation<std::unique_ptr<DirectoryCursor>>;
        for (const auto& child : node_->children) {
            if (child.first != name) continue;
            if (child.second->kind != ObjectKind::Directory)
                return Result::unavailable(ObservationState::Unsupported,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::FinalLinkRejected), "fixture.not_directory"});
            return Result::available(std::make_unique<Cursor>(child.second));
        }
        return Result::unavailable(ObservationState::Gone,
            {ErrorDomain::Posix, 2, "fixture.directory_missing"});
    }

protected:
    Observation<DirectoryStep> next_step() override {
        if (position_ == node_->children.size()) return Observation<DirectoryStep>::available(DirectoryEnd{});
        return Observation<DirectoryStep>::available(node_->children[position_++].first);
    }

private:
    std::shared_ptr<Node> node_;
    std::size_t position_ = 0;
};

std::shared_ptr<Node> fixture() {
    const AdapterScope scope;
    auto root = node(scope, 1, ObjectKind::Directory);
    auto directory = node(scope, 2, ObjectKind::Directory);
    auto nested = node(scope, 3, ObjectKind::RegularFile, 4, 12);
    auto file = node(scope, 4, ObjectKind::RegularFile, 4, 7);
    auto link = node(scope, 5, ObjectKind::SymbolicLink);
    directory->children.emplace_back(*NativeName::from_posix("nested"), nested);
    root->children.emplace_back(*NativeName::from_posix("dir"), directory);
    root->children.emplace_back(*NativeName::from_posix("file"), file);
    root->children.emplace_back(*NativeName::from_posix("alias"), file);
    root->children.emplace_back(*NativeName::from_posix("link"), link);
    return root;
}

RecursiveInventoryJob::Result finish(RecursiveInventoryJob& job) {
    for (int attempt = 0; attempt != 100; ++attempt) {
        const auto state = job.advance();
        if (state == RecursiveInventoryState::Complete) return take(job.completed());
        if (state == RecursiveInventoryState::Failed)
            throw std::runtime_error("recursive inventory failed " +
                (job.failure() ? job.failure()->error.operation : "missing"));
    }
    throw std::runtime_error("recursive inventory fixture did not complete");
}
} // namespace

CK_TEST(recursive_inventory_seals_exact_native_tree_and_preserves_hard_link_groups) {
    const auto root = fixture();
    RecursiveInventoryJob job(std::make_shared<Cursor>(root), path("/root"),
        InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    const auto result = finish(job);
    CK_CHECK(result->entries().size() == 6 && result->hard_link_groups().size() == 6);
    CK_CHECK(result->entries()[0].source_path == path("/root"));
    CK_CHECK(result->entries()[1].source_path == path("/root/dir"));
    CK_CHECK(result->entries()[2].source_path == path("/root/dir/nested"));
    CK_CHECK(result->entries()[3].source_path == path("/root/file"));
    CK_CHECK(result->entries()[4].source_path == path("/root/alias"));
    CK_CHECK(result->entries()[5].source_path == path("/root/link"));
    CK_CHECK(result->hard_link_groups()[3] == result->hard_link_groups()[4]);
    CK_CHECK(result->entries()[5].link == LinkTraversalEvidence::NotFollowed);
    CK_CHECK(job.advance() == RecursiveInventoryState::Complete);
}

CK_TEST(recursive_inventory_refuses_followed_links_without_target_evidence) {
    RecursiveInventoryJob job(std::make_shared<Cursor>(fixture()), path("/root"),
        InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::FollowLinks);
    CK_CHECK(job.advance() == RecursiveInventoryState::Failed);
    CK_CHECK(job.failure() && job.failure()->state == ObservationState::Unsupported &&
        job.failure()->error.code == static_cast<std::int64_t>(AdapterError::FinalLinkRejected));
    CK_CHECK(!job.completed().value());
}

CK_TEST(recursive_inventory_rejects_mount_crossing_depth_overflow_and_directory_cycles) {
    const AdapterScope scope;
    auto root = node(scope, 1, ObjectKind::Directory);
    auto cross = node(scope, 2, ObjectKind::RegularFile, 9, 1);
    root->children.emplace_back(*NativeName::from_posix("cross"), cross);
    RecursiveInventoryJob mount_job(std::make_shared<Cursor>(root), path("/root"),
        InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(mount_job.advance() == RecursiveInventoryState::Failed && mount_job.failure());
    CK_CHECK(mount_job.failure()->error.code == static_cast<std::int64_t>(AdapterError::MetadataChanged));

    auto deep_root = fixture();
    RecursiveInventoryLimits shallow; shallow.max_depth = 1;
    RecursiveInventoryJob depth_job(std::make_shared<Cursor>(deep_root), path("/root"),
        InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks, shallow);
    CK_CHECK(depth_job.advance() == RecursiveInventoryState::Failed && depth_job.failure());
    CK_CHECK(depth_job.failure()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));

    auto cycle_root = node(scope, 10, ObjectKind::Directory);
    auto cycle = node(scope, 10, ObjectKind::Directory);
    cycle_root->children.emplace_back(*NativeName::from_posix("cycle"), cycle);
    RecursiveInventoryJob cycle_job(std::make_shared<Cursor>(cycle_root), path("/root"),
        InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(cycle_job.advance() == RecursiveInventoryState::Failed && cycle_job.failure());
    CK_CHECK(cycle_job.failure()->error.code == static_cast<std::int64_t>(AdapterError::PlanTopologyUnsafe));
}

CK_TEST(recursive_inventory_cancellation_before_native_work_is_resumable_and_abandonment_releases_state) {
    auto root = fixture();
    RecursiveInventoryJob job(std::make_shared<Cursor>(root), path("/root"),
        InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    std::stop_source stop; stop.request_stop();
    CK_CHECK(job.advance(stop.get_token()) == RecursiveInventoryState::Paused && !job.completed().value());
    CK_CHECK(job.abandon() && job.state() == RecursiveInventoryState::Abandoned && !job.completed().value());
    CK_CHECK(!job.abandon());

    RecursiveInventoryJob resumable(std::make_shared<Cursor>(fixture()), path("/root"),
        InventoryMountPolicy::StayOnRootMount, InventoryLinkPolicy::PreserveLinks);
    CK_CHECK(resumable.advance(stop.get_token()) == RecursiveInventoryState::Paused);
    CK_CHECK(resumable.advance() == RecursiveInventoryState::Complete && resumable.completed().value());
}
