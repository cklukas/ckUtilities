// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pane_listing_job.hpp"
#include <span>
#include <map>

namespace ck::files {
// Second worker phase after native End, before pane publication. The input must
// explicitly retain its scanned directory. Only old immutable HeldChild families
// can establish continuity; matching cached IDs/names alone cannot preserve marks.
// This bounded in-memory preparation is not a large-view index or scheduler.
class PaneRefreshJob final {
public:
    using Result = PaneListingJob::Result;
    static constexpr std::size_t steps_per_advance = 256;
    static constexpr std::size_t hard_component_bytes = 1024 * 1024;
    PaneRefreshJob(Result listing, std::span<const HeldChild::Pointer> previous, StorageModel model,
        PaneLimits limits = {}, std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock(),
        MonotonicClock::Duration time_slice = std::chrono::milliseconds(20));
    PaneRefreshJob(const PaneRefreshJob&) = delete;
    PaneRefreshJob& operator=(const PaneRefreshJob&) = delete;
    PaneListingJobState advance(std::stop_token stop = {});
    // Worker only after advance returns. Issued immutable results still require
    // receiver generation revocation. Failed jobs preserve their diagnostic.
    bool abandon() noexcept;
    PaneListingJobState state() const noexcept { return state_; }
    const PaneListingRequest& request() const noexcept { return request_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    struct Binding { std::optional<std::size_t> row; HeldChild::Pointer previous; };
    void release_staging() noexcept;
    void fail(UnavailableObservation error) noexcept;
    void issue(const NativeName& name, UnavailableObservation error);
    Result input_;
    PaneListingRequest request_;
    StorageModel model_;
    std::shared_ptr<const MonotonicClock> clock_;
    MonotonicClock::Duration time_slice_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    std::vector<PaneEntry> entries_;
    std::vector<HeldChild::Pointer> previous_;
    std::vector<Binding> bindings_;
    std::map<NativeName, std::size_t, NativeNameLess> indexes_;
    std::vector<PaneBindingIssue> issues_;
    std::size_t next_ = 0, next_previous_ = 0, next_row_ = 0;
    std::size_t previous_bytes_ = 0, native_bytes_ = 0, display_bytes_ = 0;
    PaneLimits limits_;
    PaneListingJobState state_ = PaneListingJobState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};
} // namespace ck::files
