// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_listing.hpp"
#include "pane_model.hpp"
#include <set>

namespace ck::files {
// Only a job that reached native End can construct this immutable result.
// Generation and directory observations travel with the rows, not separately.
class PaneRefreshJob;
class PaneAcquireJob;
Observation<bool> pane_hidden_state(const NativeName& name, const Observation<BasicMetadata>& metadata, StorageModel model);
enum class ListingDirectoryRetention { ReleaseAtEnd, RetainForRefresh };
class CompletedPaneListing final {
public:
    const PaneListingRequest& request() const noexcept { return request_; }
    const Observation<ObjectIdentity>& directory() const noexcept { return directory_; }
    const std::vector<PaneEntry>& entries() const noexcept { return entries_; }
    const std::vector<PaneBindingIssue>& binding_issues() const noexcept { return issues_; }
    bool retains_scan_directory() const noexcept { return directory_pin_ != nullptr; }
private:
    friend class PaneListingJob;
    friend class PaneRefreshJob;
    friend class PaneAcquireJob;
    CompletedPaneListing(PaneListingRequest request, Observation<ObjectIdentity> directory, std::vector<PaneEntry> entries,
        std::shared_ptr<const DirectoryCursor> pin = {}, std::vector<PaneBindingIssue> issues = {})
        : request_(std::move(request)), directory_(std::move(directory)), entries_(std::move(entries)),
          directory_pin_(std::move(pin)), issues_(std::move(issues)) {}
    PaneListingRequest request_;
    Observation<ObjectIdentity> directory_;
    std::vector<PaneEntry> entries_;
    std::shared_ptr<const DirectoryCursor> directory_pin_;
    std::vector<PaneBindingIssue> issues_;
};
enum class PaneListingJobState { Collecting, Paused, Complete, Failed, Abandoned };
// Worker-only bounded staging for the current in-memory pane. Never publishes
// partial scans; the immutable result is applied through the pane generation gate.
// This is not the disk-backed index required for larger listings.
class PaneListingJob final {
public:
    using Result = std::shared_ptr<const CompletedPaneListing>;
    PaneListingJob(PaneListingRequest request, std::unique_ptr<DirectoryListing> listing,
        StorageModel model, PaneLimits limits = {}, ListingDirectoryRetention retention = ListingDirectoryRetention::ReleaseAtEnd);
    PaneListingJob(const PaneListingJob&) = delete;
    PaneListingJob& operator=(const PaneListingJob&) = delete;
    PaneListingJobState advance(std::stop_token stop = {});
    // Owning worker only, after advance has returned. Releases discovery and
    // private results; cannot interrupt a native call. Issued immutable payloads
    // are revoked separately through the receiving pane's generation gate.
    // A failed job retains its diagnostic; repeated abandonment is a no-op.
    bool abandon() noexcept;
    PaneListingJobState state() const noexcept { return state_; }
    const PaneListingRequest& request() const noexcept { return request_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    void fail(UnavailableObservation failure);
    void release_staging() noexcept;
    PaneListingRequest request_;
    std::unique_ptr<DirectoryListing> listing_;
    std::shared_ptr<const DirectoryCursor> directory_pin_;
    Observation<ObjectIdentity> directory_;
    StorageModel model_;
    PaneLimits limits_;
    std::vector<PaneEntry> entries_;
    std::set<NativeName, NativeNameLess> names_;
    std::optional<NameEncoding> encoding_;
    std::size_t native_bytes_ = 0, display_bytes_ = 0;
    PaneListingJobState state_ = PaneListingJobState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};
} // namespace ck::files
