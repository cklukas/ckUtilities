// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "recursive_inventory_job.hpp"

#include "held_child.hpp"

#include <stdexcept>

namespace ck::files {
namespace {
template <class T> Observation<T> unavailable_from(const UnavailableObservation& detail) {
    return Observation<T>::unavailable(detail.state, detail.error);
}

UnavailableObservation error(AdapterError code, const char* operation,
    ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

bool available_identity(const Observation<ObjectIdentity>& identity) noexcept {
    return identity.value() && identity.value()->scheme != IdentityScheme::Unknown &&
        identity.value()->volume != NativeIdentifier{} && identity.value()->object != NativeIdentifier{};
}

bool available_kind(const Observation<ObjectKind>& kind) noexcept {
    return kind.value() && *kind.value() >= ObjectKind::RegularFile && *kind.value() <= ObjectKind::Other;
}

bool available_mount(const Observation<std::uint64_t>& mount) noexcept { return mount.value() != nullptr; }

bool available_size(const Observation<std::uint64_t>& size) noexcept { return size.value() != nullptr; }
} // namespace

RecursiveInventoryJob::RecursiveInventoryJob(std::shared_ptr<DirectoryCursor> root,
    NativeAbsolutePath root_path, InventoryMountPolicy mount_policy,
    InventoryLinkPolicy link_policy, RecursiveInventoryLimits limits,
    std::shared_ptr<const MonotonicClock> clock, HeldChild::Pointer root_source)
    : root_(std::move(root)), root_source_(std::move(root_source)), root_path_(std::move(root_path)),
      link_policy_(link_policy), limits_(limits), clock_(std::move(clock)),
      builder_(std::make_unique<FrozenInventoryBuilder>(mount_policy, link_policy, limits.inventory)) {
    if (!root_ || !root_path_.is_valid() || !clock_ || limits_.max_depth == 0 ||
        limits_.max_depth > RecursiveInventoryLimits::hard_max_depth ||
        limits_.time_slice <= MonotonicClock::Duration::zero() || limits_.time_slice > std::chrono::seconds(1))
        throw std::invalid_argument("invalid recursive inventory job");
    if (builder_->state() == FrozenInventoryState::Failed) {
        failure_ = builder_->failure();
        state_ = RecursiveInventoryState::Failed;
    }
}

bool RecursiveInventoryJob::link_kind(ObjectKind kind) noexcept {
    return kind == ObjectKind::SymbolicLink || kind == ObjectKind::Junction || kind == ObjectKind::ReparsePoint;
}

bool RecursiveInventoryJob::fail(UnavailableObservation detail) noexcept {
    failure_ = std::move(detail);
    completed_.reset();
    if (builder_ && builder_->state() == FrozenInventoryState::Collecting) builder_->abandon();
    release_staging();
    state_ = RecursiveInventoryState::Failed;
    return false;
}

bool RecursiveInventoryJob::fail(AdapterError code, const char* operation, ObservationState state) noexcept {
    return fail(error(code, operation, state));
}

void RecursiveInventoryJob::release_staging() noexcept {
    frames_.clear();
    directory_identities_.clear();
    root_.reset();
    root_source_.reset();
    held_sources_.clear();
}

bool RecursiveInventoryJob::add_metadata(const NativeAbsolutePath& path, const BasicMetadata& metadata,
    LinkTraversalEvidence link) {
    if (!available_identity(metadata.identity)) return fail(AdapterError::UnqualifiedIdentity, "recursive_inventory.identity");
    if (!available_kind(metadata.kind)) return fail(AdapterError::InvalidNativeResult, "recursive_inventory.kind");
    if (!available_mount(metadata.mount_id)) {
        const auto& detail = *metadata.mount_id.unavailable_detail();
        return fail({detail.state, detail.error});
    }
    if (!available_size(metadata.logical_size)) return fail(AdapterError::InvalidNativeResult, "recursive_inventory.logical_size");
    FrozenInventoryEntry entry{path, *metadata.identity.value(), *metadata.kind.value(), metadata.mount_id,
        *metadata.logical_size.value(), link, {}};
    if (!builder_->add(std::move(entry))) {
        if (builder_->failure()) return fail(*builder_->failure());
        return fail(AdapterError::InvalidNativeResult, "recursive_inventory.builder");
    }
    return true;
}

bool RecursiveInventoryJob::stop_or_yield(std::stop_token stop, CooperativeTimeSlice& slice) {
    if (stop.stop_requested()) { state_ = RecursiveInventoryState::Paused; return true; }
    const auto expired = slice.expired();
    last_reading_ = slice.last_reading();
    if (!expired.value()) { fail(*expired.unavailable_detail()); return true; }
    if (*expired.value()) { state_ = RecursiveInventoryState::Collecting; return true; }
    return false;
}

RecursiveInventoryState RecursiveInventoryJob::advance(std::stop_token stop) {
    if (state_ == RecursiveInventoryState::Complete || state_ == RecursiveInventoryState::Failed ||
        state_ == RecursiveInventoryState::Abandoned)
        return state_;
    if (stop.stop_requested()) { state_ = RecursiveInventoryState::Paused; return state_; }
    try {
        CooperativeTimeSlice slice(*clock_, limits_.time_slice, last_reading_);
        for (std::size_t steps = 0; steps < DirectoryCursor::max_batch_size; ++steps) {
            if (stop_or_yield(stop, slice)) return state_;
            if (phase_ == Phase::Root) {
                auto root_metadata = root_->inspect_basic();
                if (!root_metadata.value()) { const auto& detail = *root_metadata.unavailable_detail(); fail(detail); return state_; }
                if (!available_identity(root_metadata.value()->identity) || !available_kind(root_metadata.value()->kind) ||
                    *root_metadata.value()->kind.value() != ObjectKind::Directory || !available_size(root_metadata.value()->logical_size)) {
                    fail(AdapterError::InvalidNativeResult, "recursive_inventory.root"); return state_;
                }
                if (!available_identity(root_->identity()) ||
                    compare_identity(*root_->identity().value(), *root_metadata.value()->identity.value()) != IdentityComparison::Same) {
                    fail(AdapterError::MetadataChanged, "recursive_inventory.root_identity", ObservationState::Changed); return state_;
                }
                if (root_source_) {
                    if (!root_source_->name().is_valid() || !root_source_->metadata().identity.value() ||
                        compare_identity(*root_source_->metadata().identity.value(), *root_metadata.value()->identity.value()) != IdentityComparison::Same ||
                        !root_source_->metadata().kind.value() || *root_source_->metadata().kind.value() != ObjectKind::Directory) {
                        fail(AdapterError::MetadataChanged, "recursive_inventory.root_source", ObservationState::Changed); return state_;
                    }
                    held_sources_.push_back(root_source_);
                }
                if (!add_metadata(root_path_, *root_metadata.value(), LinkTraversalEvidence::NotLink)) return state_;
                const auto identity = *root_metadata.value()->identity.value();
                frames_.push_back({root_, root_path_, identity, {}, 0});
                directory_identities_.push_back(identity);
                phase_ = Phase::Enumerate;
                continue;
            }
            if (phase_ == Phase::Seal) {
                if (!builder_->seal()) {
                    if (builder_->failure()) fail(*builder_->failure());
                    else fail(AdapterError::PlanInvalid, "recursive_inventory.seal");
                    return state_;
                }
                auto result = builder_->completed();
                if (!result.value()) { fail(*result.unavailable_detail()); return state_; }
                completed_ = std::move(result).take_value();
                if (root_source_) {
                    if (held_sources_.size() != completed_->entries().size()) {
                        fail(AdapterError::PlanInvalid, "recursive_inventory.source_shape"); return state_;
                    }
                    completed_sources_ = std::shared_ptr<const CompletedRecursiveInventory>(
                        new const CompletedRecursiveInventory(completed_, std::move(held_sources_)));
                }
                release_staging();
                // release_staging intentionally does not clear completed_ on success.
                state_ = RecursiveInventoryState::Complete;
                return state_;
            }

            if (frames_.empty()) { phase_ = Phase::Seal; continue; }
            auto& frame = frames_.back();
            if (!frame.pending) {
                auto read = frame.directory->read(1, stop);
                if (read.state == DirectoryReadState::Error) {
                    if (read.error) fail(*read.error);
                    else fail(AdapterError::InvalidNativeResult, "recursive_inventory.read");
                    return state_;
                }
                if (!read.names.empty()) {
                    if (read.names.size() != 1 || !read.names.front().is_valid()) {
                        fail(AdapterError::InvalidNativeResult, "recursive_inventory.name"); return state_;
                    }
                    frame.pending = std::move(read.names.front());
                }
                if (read.state == DirectoryReadState::Cancelled) {
                    state_ = RecursiveInventoryState::Paused; return state_;
                }
                if (read.state == DirectoryReadState::End && !frame.pending) {
                    const auto id = frame.identity;
                    frames_.pop_back();
                    if (!directory_identities_.empty()) directory_identities_.pop_back();
                    if (frames_.empty()) phase_ = Phase::Seal;
                    continue;
                }
                if (!frame.pending) continue;
            }

            const NativeName name = *frame.pending;
            auto path = frame.path.child_address(name);
            if (!path.value()) { fail(*path.unavailable_detail()); return state_; }
            auto held = HeldChild::observe(frame.directory, name, stop);
            if (!held.value()) {
                if (stop.stop_requested()) { state_ = RecursiveInventoryState::Paused; return state_; }
                fail(*held.unavailable_detail()); return state_;
            }
            auto held_source = std::move(held).take_value();
            const auto& metadata = held_source->metadata();
            const auto kind = metadata.kind.value();
            if (!available_kind(metadata.kind)) { fail(AdapterError::InvalidNativeResult, "recursive_inventory.child_kind"); return state_; }
            if (link_kind(*kind) && link_policy_ == InventoryLinkPolicy::FollowLinks) {
                fail(AdapterError::FinalLinkRejected, "recursive_inventory.follow_link", ObservationState::Unsupported); return state_;
            }
            if (frame.depth >= limits_.max_depth) {
                fail(AdapterError::InventoryLimit, "recursive_inventory.depth", ObservationState::Unknown); return state_;
            }
            auto child_path = std::move(path).take_value();
            if (!add_metadata(child_path, metadata,
                    link_kind(*kind) ? LinkTraversalEvidence::NotFollowed : LinkTraversalEvidence::NotLink)) return state_;
            if (root_source_) held_sources_.push_back(held_source);
            if (*kind != ObjectKind::Directory) { frame.pending.reset(); continue; }
            auto child = frame.directory->open_directory_child(name);
            if (!child.value()) {
                if (stop.stop_requested()) { state_ = RecursiveInventoryState::Paused; return state_; }
                fail(*child.unavailable_detail()); return state_;
            }
            auto child_cursor = std::move(child).take_value();
            if (!child_cursor || !available_identity(child_cursor->identity()) ||
                compare_identity(*metadata.identity.value(), *child_cursor->identity().value()) != IdentityComparison::Same) {
                fail(AdapterError::MetadataChanged, "recursive_inventory.child_identity", ObservationState::Changed); return state_;
            }
            const auto child_identity = *metadata.identity.value();
            for (const auto& prior : directory_identities_)
                if (compare_identity(prior, child_identity) == IdentityComparison::Same) {
                    fail(AdapterError::PlanTopologyUnsafe, "recursive_inventory.directory_cycle"); return state_;
                }
            std::shared_ptr<DirectoryCursor> child_shared(std::move(child_cursor));
            const auto child_depth = frame.depth + 1;
            frame.pending.reset();
            frames_.push_back({std::move(child_shared), std::move(child_path), child_identity, {}, child_depth});
            directory_identities_.push_back(child_identity);
        }
        return state_;
    } catch (...) {
        fail(error(AdapterError::InterruptedRead, "recursive_inventory.exception"));
        throw;
    }
}

bool RecursiveInventoryJob::abandon() noexcept {
    if (state_ == RecursiveInventoryState::Complete || state_ == RecursiveInventoryState::Failed ||
        state_ == RecursiveInventoryState::Abandoned)
        return false;
    if (builder_) builder_->abandon();
    release_staging();
    state_ = RecursiveInventoryState::Abandoned;
    return true;
}

Observation<RecursiveInventoryJob::Result> RecursiveInventoryJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return unavailable_from<Result>(*failure_);
    return {};
}

Observation<std::shared_ptr<const CompletedRecursiveInventory>> RecursiveInventoryJob::completed_sources() const {
    using ResultWithSources = std::shared_ptr<const CompletedRecursiveInventory>;
    if (completed_sources_) return Observation<ResultWithSources>::available(completed_sources_);
    if (failure_) return Observation<ResultWithSources>::unavailable(failure_->state, failure_->error);
    return {};
}

} // namespace ck::files
