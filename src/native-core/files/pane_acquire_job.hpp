// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pane_listing_job.hpp"
#include <map>

namespace ck::files {
// Initial binding stage between native End and publication. Requested names
// match discovery rows. Retained refresh output can carry existing sealed
// bindings and issues through this stage. Existing bindings are never reopened.
// Each newly successful
// binding describes the object opened NOW, never continuity with cached IDs.
// New binding creates new row IDs and cannot preserve old unheld selection.
// Use PaneRefreshJob for existing held families. Worker only; no mutation.
class PaneAcquireJob final {
public:
    using Result = PaneListingJob::Result;
    static constexpr std::size_t steps_per_advance = 256;
    static constexpr std::size_t hard_component_bytes = 1024 * 1024;
    PaneAcquireJob(Result listing, std::vector<NativeName>&& requested, StorageModel model,
        PaneLimits limits = {}, std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock(),
        MonotonicClock::Duration time_slice = std::chrono::milliseconds(20));
    PaneAcquireJob(const PaneAcquireJob&) = delete;
    PaneAcquireJob& operator=(const PaneAcquireJob&) = delete;
    PaneListingJobState advance(std::stop_token stop = {});
    bool abandon() noexcept; // worker only after advance; receiver must revoke generation separately
    PaneListingJobState state() const noexcept { return state_; }
    const PaneListingRequest& request() const noexcept { return request_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    void release_staging() noexcept;
    void fail(UnavailableObservation failure) noexcept;
    Result input_, completed_;
    PaneListingRequest request_;
    std::vector<NativeName> requested_;
    std::vector<std::optional<std::size_t>> matches_;
    std::map<NativeName, std::size_t, NativeNameLess> indexes_;
    std::vector<PaneEntry> entries_;
    std::vector<PaneBindingIssue> issues_;
    StorageModel model_;
    PaneLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    MonotonicClock::Duration time_slice_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    std::size_t next_request_ = 0, next_row_ = 0, next_binding_ = 0, next_issue_ = 0;
    std::size_t requested_bytes_ = 0, native_bytes_ = 0, display_bytes_ = 0, issue_bytes_ = 0, held_count_ = 0;
    PaneListingJobState state_ = PaneListingJobState::Collecting;
    std::optional<UnavailableObservation> failure_;
};
} // namespace ck::files
