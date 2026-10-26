// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "held_child.hpp"
#include "monotonic_clock.hpp"
namespace ck::files {
struct AncestorWalkLimits {
    std::size_t directories = 256; // includes destination and repeated endpoint
    MonotonicClock::Duration time_slice = std::chrono::milliseconds(20);
};
enum class AncestorWalkEnd { ObservedSource, RepeatedDirectory, DisjointBoundary };
struct AncestorWalkWitness {
    AncestorWalkEnd end;
    std::size_t directory;
    std::optional<std::size_t> repeated;
};
struct AncestorDirectory {
    Observation<BasicMetadata> metadata;
    // Initial directory is held by the separate destination cursor.
    std::shared_ptr<const OpenedEntry> owner;
};
class AncestorWalkJob;
class CompletedAncestorWalk final {
public:
    const HeldChild::Pointer& source() const noexcept { return source_; }
    const std::shared_ptr<const DirectoryCursor>& destination() const noexcept { return destination_; }
    const Observation<BasicMetadata>& source_now() const noexcept { return source_now_; }
    const std::vector<AncestorDirectory>& directories() const noexcept { return directories_; }
    const Observation<AncestorWalkWitness>& outcome() const noexcept { return outcome_; }
private:
    friend class AncestorWalkJob;
    CompletedAncestorWalk(HeldChild::Pointer source, std::shared_ptr<const DirectoryCursor> destination,
        Observation<BasicMetadata> source_now, std::vector<AncestorDirectory> directories, Observation<AncestorWalkWitness> outcome) noexcept
        : source_(std::move(source)), destination_(std::move(destination)), source_now_(std::move(source_now)),
          directories_(std::move(directories)), outcome_(std::move(outcome)) {}
    HeldChild::Pointer source_;
    std::shared_ptr<const DirectoryCursor> destination_;
    Observation<BasicMetadata> source_now_;
    std::vector<AncestorDirectory> directories_;
    Observation<AncestorWalkWitness> outcome_;
};
enum class AncestorWalkState { Collecting, Paused, Complete, Failed, Abandoned };
// Worker-only native parent observations. A DisjointBoundary result is reserved
// for a separate, platform-qualified provider-root proof; this walker does not
// infer it from a repeated identity. Every repeated directory remains ambiguous.
// Retains all observed owners;
// native calls can block. No paths, recursion or mutation.
class AncestorWalkJob final {
public:
    using Result = std::shared_ptr<const CompletedAncestorWalk>;
    static constexpr std::size_t steps_per_advance = 256, hard_directories = 4096;
    AncestorWalkJob(HeldChild::Pointer source, std::shared_ptr<const DirectoryCursor> destination,
        AncestorWalkLimits limits = {}, std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock(),
        std::shared_ptr<const DirectoryCursor> provider_root = {});
    AncestorWalkJob(const AncestorWalkJob&) = delete;
    AncestorWalkJob& operator=(const AncestorWalkJob&) = delete;
    AncestorWalkState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    AncestorWalkState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    enum class Phase { Source, CheckSource, Destination, Compare, Repeat, Parent, ParentMetadata, Seal };
    void release_staging() noexcept;
    void fail(UnavailableObservation failure) noexcept;
    void unavailable(UnavailableObservation failure);
    void step();
    HeldChild::Pointer source_;
    std::shared_ptr<const DirectoryCursor> destination_;
    std::shared_ptr<const DirectoryCursor> provider_root_;
    AncestorWalkLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    Observation<BasicMetadata> source_now_;
    std::vector<AncestorDirectory> directories_;
    std::shared_ptr<const OpenedEntry> pending_;
    std::size_t repeated_ = 0;
    Observation<AncestorWalkWitness> outcome_;
    Phase phase_ = Phase::Source;
    AncestorWalkState state_ = AncestorWalkState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};
} // namespace ck::files
