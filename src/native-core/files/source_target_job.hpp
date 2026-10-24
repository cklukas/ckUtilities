// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "destination_batch_job.hpp"
#include "held_child.hpp"

namespace ck::files {
struct SourceTargetLimits {
    std::size_t rows = 256;
    std::size_t component_bytes = 64 * 1024;
    MonotonicClock::Duration time_slice = std::chrono::milliseconds(20);
};
struct SourceTargetInspection {
    Observation<BasicMetadata> source_now, target_now;
    // Available only for qualified Same/Different of the simultaneously held
    // objects. Missing is NotApplicable, never Different. No ancestry/name or
    // content/snapshot/operation-permission assertion is implied.
    Observation<IdentityComparison> comparison;
};
class SourceTargetJob;
class CompletedSourceTargets final {
public:
    const std::vector<SourceTargetInspection>& entries() const noexcept { return entries_; }
    const std::vector<HeldChild::Pointer>& sources() const noexcept { return sources_; }
    const DestinationBatchJob::Result& destination() const noexcept { return destination_; }
private:
    friend class SourceTargetJob;
    CompletedSourceTargets(std::vector<HeldChild::Pointer> sources, DestinationBatchJob::Result destination,
        std::vector<SourceTargetInspection> entries) noexcept
        : sources_(std::move(sources)), destination_(std::move(destination)), entries_(std::move(entries)) {}
    std::vector<HeldChild::Pointer> sources_;
    DestinationBatchJob::Result destination_;
    std::vector<SourceTargetInspection> entries_;
};
enum class SourceTargetState { Collecting, Paused, Complete, Failed, Abandoned };
// Worker-only self-target evidence for source rows in a completed destination
// batch. All source and target handles remain held through comparisons and in
// issued results. No new lookup, path reopen, native writes or content reads.
class SourceTargetJob final {
public:
    using Result = std::shared_ptr<const CompletedSourceTargets>;
    static constexpr std::size_t steps_per_advance = 256;
    static constexpr std::size_t hard_component_bytes = 1024 * 1024;
    SourceTargetJob(std::vector<HeldChild::Pointer>&& sources, DestinationBatchJob::Result destination,
        SourceTargetLimits limits = {}, std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    SourceTargetJob(const SourceTargetJob&) = delete;
    SourceTargetJob& operator=(const SourceTargetJob&) = delete;
    SourceTargetState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    SourceTargetState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    enum class Phase { Validate, Source, Target, Compare };
    void fail(UnavailableObservation failure) noexcept;
    void release_staging() noexcept;
    void step();
    std::vector<HeldChild::Pointer> sources_;
    DestinationBatchJob::Result destination_;
    SourceTargetLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    std::vector<SourceTargetInspection> entries_;
    std::size_t next_validation_ = 0, next_ = 0;
    Phase phase_ = Phase::Validate;
    SourceTargetState state_ = SourceTargetState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};
} // namespace ck::files
