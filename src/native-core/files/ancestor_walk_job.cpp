// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ancestor_walk_job.hpp"
#include <stdexcept>
#include <type_traits>
namespace ck::files {
namespace {
static_assert(std::is_nothrow_move_constructible_v<UnavailableObservation>);
UnavailableObservation error(ObservationState state, AdapterError code, const char* operation) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
std::optional<UnavailableObservation> directory_metadata(const Observation<BasicMetadata>& metadata, bool observed_directory = false) {
    if (!metadata.value()) return *metadata.unavailable_detail();
    if (!metadata.value()->kind.value()) return *metadata.value()->kind.unavailable_detail();
    if (*metadata.value()->kind.value() < ObjectKind::RegularFile || *metadata.value()->kind.value() > ObjectKind::Other)
        return error(ObservationState::Failed, AdapterError::InvalidNativeResult, "ancestor.object_kind");
    if (*metadata.value()->kind.value() != ObjectKind::Directory)
        return error(observed_directory ? ObservationState::Changed : ObservationState::Unsupported,
            observed_directory ? AdapterError::MetadataChanged : AdapterError::NotApplicable, "ancestor.object_kind");
    if (!metadata.value()->identity.value()) return *metadata.value()->identity.unavailable_detail();
    const auto& id = *metadata.value()->identity.value();
    if (compare_identity(id, id) != IdentityComparison::Same)
        return error(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "ancestor.identity");
    return {};
}
std::optional<UnavailableObservation> same_identity(const Observation<ObjectIdentity>& expected, const ObjectIdentity& now) {
    if (!expected.value()) return *expected.unavailable_detail();
    const auto compared = compare_identity(*expected.value(), now);
    if (compared == IdentityComparison::Same) return {};
    return error(compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
        compared == IdentityComparison::Different ? AdapterError::MetadataChanged : AdapterError::UnqualifiedIdentity, "ancestor.changed_identity");
}
}
AncestorWalkJob::AncestorWalkJob(HeldChild::Pointer source, std::shared_ptr<const DirectoryCursor> destination,
    AncestorWalkLimits limits, std::shared_ptr<const MonotonicClock> clock,
    std::shared_ptr<const DirectoryCursor> provider_root)
    : source_(std::move(source)), destination_(std::move(destination)), provider_root_(std::move(provider_root)),
      limits_(limits), clock_(std::move(clock)) {
    if (!source_ || !destination_ || !clock_ || limits.directories == 0 || limits.directories > hard_directories ||
        limits.time_slice <= MonotonicClock::Duration::zero() || limits.time_slice > std::chrono::seconds(1))
        throw std::invalid_argument("invalid ancestor walk");
    directories_.reserve(limits.directories);
}
void AncestorWalkJob::release_staging() noexcept {
    source_.reset(); destination_.reset(); provider_root_.reset(); pending_.reset();
    std::vector<AncestorDirectory>{}.swap(directories_);
    source_now_ = {}; outcome_ = {}; repeated_ = 0;
}
void AncestorWalkJob::fail(UnavailableObservation failure) noexcept {
    failure_.emplace(std::move(failure)); state_ = AncestorWalkState::Failed; release_staging();
}
void AncestorWalkJob::unavailable(UnavailableObservation failure) {
    outcome_ = Observation<AncestorWalkWitness>::unavailable(failure.state, std::move(failure.error)); phase_ = Phase::Seal;
}
bool AncestorWalkJob::abandon() noexcept {
    if (state_ == AncestorWalkState::Failed || state_ == AncestorWalkState::Abandoned) return false;
    completed_.reset(); release_staging(); state_ = AncestorWalkState::Abandoned; return true;
}
Observation<AncestorWalkJob::Result> AncestorWalkJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
void AncestorWalkJob::step() {
    switch (phase_) {
    case Phase::Source: source_now_ = source_->inspect_basic(); phase_ = Phase::CheckSource; break;
    case Phase::CheckSource: {
        const auto original = Observation<BasicMetadata>::available(source_->metadata());
        if (const auto failed = directory_metadata(original)) { unavailable(*failed); break; }
        if (const auto failed = directory_metadata(source_now_, true)) { unavailable(*failed); break; }
        if (const auto changed = same_identity(source_->metadata().identity, *source_now_.value()->identity.value())) { unavailable(*changed); break; }
        phase_ = Phase::Destination; break;
    }
    case Phase::Destination:
        directories_.push_back({destination_->inspect_basic(), {}}); phase_ = Phase::Compare; break;
    case Phase::Compare: {
        const auto& current = directories_.back().metadata;
        if (const auto failed = directory_metadata(current, true)) { unavailable(*failed); break; }
        if (directories_.size() == 1)
            if (const auto changed = same_identity(destination_->identity(), *current.value()->identity.value())) { unavailable(*changed); break; }
        const auto compared = compare_identity(*source_now_.value()->identity.value(), *current.value()->identity.value());
        if (compared == IdentityComparison::Same) {
            outcome_ = Observation<AncestorWalkWitness>::available({AncestorWalkEnd::ObservedSource, directories_.size() - 1, {}}); phase_ = Phase::Seal;
        } else if (compared == IdentityComparison::Unknown) unavailable(error(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "ancestor.comparison"));
        else { repeated_ = 0; phase_ = Phase::Repeat; }
        break;
    }
    case Phase::Repeat: {
        const auto index = directories_.size() - 1;
        if (repeated_ == index) {
            if (directories_.size() == limits_.directories) unavailable(error(ObservationState::Unknown, AdapterError::InventoryLimit, "ancestor.directory_limit"));
            else phase_ = Phase::Parent;
            break;
        }
        const auto compared = compare_identity(*directories_[index].metadata.value()->identity.value(), *directories_[repeated_].metadata.value()->identity.value());
        if (compared == IdentityComparison::Same) {
            auto end = AncestorWalkEnd::RepeatedDirectory;
            if (provider_root_) {
                if (!provider_root_->identity().value()) {
                    unavailable(provider_root_->identity().unavailable_detail()
                        ? *provider_root_->identity().unavailable_detail()
                        : error(ObservationState::Unknown, AdapterError::UnqualifiedIdentity,
                                "ancestor.provider_root_identity"));
                    break;
                }
                const auto root_compared = compare_identity(
                    *provider_root_->identity().value(), *directories_[index].metadata.value()->identity.value());
                if (root_compared == IdentityComparison::Same)
                    end = AncestorWalkEnd::DisjointBoundary;
                else if (root_compared == IdentityComparison::Unknown) {
                    unavailable(error(ObservationState::Unknown, AdapterError::UnqualifiedIdentity,
                                      "ancestor.provider_root_comparison"));
                    break;
                }
            }
            outcome_ = Observation<AncestorWalkWitness>::available({end, index, repeated_});
            phase_ = Phase::Seal;
        } else if (compared == IdentityComparison::Unknown) unavailable(error(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "ancestor.repeat_identity"));
        else ++repeated_;
        break;
    }
    case Phase::Parent: {
        auto opened = directories_.size() == 1 ? destination_->open_parent_directory() : directories_.back().owner->open_parent_directory();
        if (!opened.value()) { unavailable(*opened.unavailable_detail()); break; }
        auto owner = std::move(opened).take_value();
        if (!owner) { fail(error(ObservationState::Failed, AdapterError::InvalidNativeResult, "ancestor.null_parent")); break; }
        pending_ = std::shared_ptr<const OpenedEntry>(std::move(owner)); phase_ = Phase::ParentMetadata; break;
    }
    case Phase::ParentMetadata:
        directories_.push_back({pending_->inspect_basic(), pending_}); pending_.reset(); phase_ = Phase::Compare; break;
    case Phase::Seal:
        completed_ = Result(new const CompletedAncestorWalk(std::move(source_), std::move(destination_), std::move(source_now_), std::move(directories_), std::move(outcome_)));
        release_staging(); state_ = AncestorWalkState::Complete; break;
    }
}
AncestorWalkState AncestorWalkJob::advance(std::stop_token stop) {
    if (state_ == AncestorWalkState::Complete || state_ == AncestorWalkState::Failed || state_ == AncestorWalkState::Abandoned) return state_;
    if (stop.stop_requested()) { state_ = AncestorWalkState::Paused; return state_; }
    try {
        CooperativeTimeSlice slice(*clock_, limits_.time_slice, last_reading_); state_ = AncestorWalkState::Collecting;
        for (std::size_t step_number = 0; step_number < steps_per_advance; ++step_number) {
            if (stop.stop_requested()) { state_ = AncestorWalkState::Paused; return state_; }
            const auto expired = slice.expired(); last_reading_ = slice.last_reading();
            if (!expired.value()) { fail(*expired.unavailable_detail()); return state_; }
            if (*expired.value()) return state_;
            step(); if (state_ != AncestorWalkState::Collecting) return state_;
        }
        return state_;
    } catch (...) { fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw; }
}
} // namespace ck::files
