// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_sort_job.hpp"
#include "pane_listing_job.hpp"
namespace ck::files {
class DirectoryIndexListingJob;
// Worker-owned result: native End plus disk-based duplicate validation. Canonical
// native-name ascending order (no folders-first). No snapshot/freshness promise;
// retain bracket observations and a read-only pin for subsequent revalidation.
// None of these observations or cached row IDs authorize native mutation.
class CompletedDirectoryIndex final {
  public:
    const PaneListingRequest &request() const noexcept {
        return request_;
    }
    const AdapterScope &source_scope() const noexcept {
        return source_scope_;
    }
    const Observation<ObjectIdentity> &directory() const noexcept {
        return directory_;
    }
    const Observation<BasicMetadata> &before() const noexcept {
        return before_;
    }
    const Observation<BasicMetadata> &after() const noexcept {
        return after_;
    }
    const std::shared_ptr<DirectoryIndexStore> &store() const noexcept {
        return store_;
    }
    const std::shared_ptr<const DirectoryCursor> &directory_pin() const noexcept {
        return pin_;
    }

  private:
    friend class DirectoryIndexListingJob;
    CompletedDirectoryIndex(PaneListingRequest request, AdapterScope scope, Observation<ObjectIdentity> directory,
                            Observation<BasicMetadata> before, Observation<BasicMetadata> after,
                            std::shared_ptr<DirectoryIndexStore> store, std::shared_ptr<const DirectoryCursor> pin)
        : request_(std::move(request)), source_scope_(std::move(scope)), directory_(std::move(directory)),
          before_(std::move(before)), after_(std::move(after)), store_(std::move(store)), pin_(std::move(pin)) {
    }
    PaneListingRequest request_;
    AdapterScope source_scope_;
    Observation<ObjectIdentity> directory_;
    Observation<BasicMetadata> before_, after_;
    std::shared_ptr<DirectoryIndexStore> store_;
    std::shared_ptr<const DirectoryCursor> pin_;
};
enum class DirectoryIndexListingState {
    Collecting,
    SortingNames,
    CheckingNames,
    Complete,
    Failed,
    Cancelled,
    Abandoned
};
// Single worker only. Each advance reads/appends one bounded native listing
// batch, advances the external sorter once, or checks one bounded sorted page.
// No RAM set/vector proportional to directory size, no fallback or partial
// publication. Names/metadata remain momentary native observations.
class DirectoryIndexListingJob final {
  public:
    using Result = std::shared_ptr<const CompletedDirectoryIndex>;
    DirectoryIndexListingJob(PaneListingRequest request, std::unique_ptr<DirectoryListing> listing, StorageModel model,
                             std::shared_ptr<IndexFileFactory> factory, std::shared_ptr<IndexStorageBudget> budget,
                             std::array<std::byte, 16> generation, DirectoryIndexStoreLimits store_limits = {},
                             DirectoryIndexSortLimits sort_limits = {});
    DirectoryIndexListingJob(const DirectoryIndexListingJob &) = delete;
    DirectoryIndexListingJob &operator=(const DirectoryIndexListingJob &) = delete;
    DirectoryIndexListingState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    DirectoryIndexListingState state() const noexcept {
        return state_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    Observation<Result> completed() const;

  private:
    bool check_directory(const Observation<BasicMetadata> &value);
    void collect(std::stop_token stop);
    void check_names(std::stop_token stop);
    void fail(UnavailableObservation error);
    void cancel() noexcept;
    void release() noexcept;
    PaneListingRequest request_;
    std::unique_ptr<DirectoryListing> listing_;
    StorageModel model_;
    DirectoryIndexSortLimits sort_limits_;
    AdapterScope source_scope_;
    Observation<ObjectIdentity> directory_;
    Observation<BasicMetadata> before_;
    std::shared_ptr<const DirectoryCursor> pin_;
    std::shared_ptr<DirectoryIndexStore> store_;
    std::unique_ptr<DirectoryIndexSortJob> sorter_;
    std::optional<IndexScanCursor> cursor_;
    std::optional<NativeName> previous_;
    std::optional<NameEncoding> encoding_;
    DirectoryIndexListingState state_ = DirectoryIndexListingState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result result_;
};
} // namespace ck::files
