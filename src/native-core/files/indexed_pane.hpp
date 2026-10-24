// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_listing_job.hpp"
#include "directory_index_lookup.hpp"
#include "pane_page.hpp"
namespace ck::files {
enum class IndexedPaneDataOrigin { Listing, Reorder };
class IndexedPaneDataJob;
class IndexedPane;
// Worker-owned certificate binding completed collection, order, lookup and
// directory pin. Construction is private to the complete preparation job.
// Pages never retain this object or any of its native/file owners.
class CompletedIndexedPaneData final {
  public:
    const PaneListingRequest &request() const noexcept {
        return request_;
    }
    PaneOrder order() const noexcept {
        return order_;
    }
    std::uint64_t rows() const noexcept {
        return store_->rows();
    }
    const AdapterScope &row_scope() const noexcept {
        return store_->row_scope();
    }
    const Observation<BasicMetadata> &before() const noexcept {
        return before_;
    }
    const Observation<BasicMetadata> &after() const noexcept {
        return after_;
    }

  private:
    friend class IndexedPaneDataJob;
    friend class IndexedPane;
    CompletedIndexedPaneData(PaneListingRequest request, PaneOrder order, IndexedPaneDataOrigin origin,
                             AdapterScope scope, std::optional<AdapterScope> predecessor,
                             Observation<ObjectIdentity> directory, Observation<BasicMetadata> before,
                             Observation<BasicMetadata> after, BasicMetadata baseline,
                             std::shared_ptr<const DirectoryCursor> pin, std::shared_ptr<DirectoryIndexStore> store,
                             std::shared_ptr<DirectoryIndexLookup> lookup)
        : request_(std::move(request)), order_(order), origin_(origin), source_scope_(std::move(scope)),
          predecessor_(std::move(predecessor)), directory_(std::move(directory)), before_(std::move(before)),
          after_(std::move(after)), baseline_(std::move(baseline)), pin_(std::move(pin)), store_(std::move(store)),
          lookup_(std::move(lookup)) {
    }
    PaneListingRequest request_;
    PaneOrder order_;
    IndexedPaneDataOrigin origin_;
    AdapterScope source_scope_;
    AdapterScope revision_;
    std::optional<AdapterScope> predecessor_;
    Observation<ObjectIdentity> directory_;
    Observation<BasicMetadata> before_, after_;
    BasicMetadata baseline_; // retained available continuity fields, not a snapshot
    std::shared_ptr<const DirectoryCursor> pin_;
    std::shared_ptr<DirectoryIndexStore> store_;
    std::shared_ptr<DirectoryIndexLookup> lookup_;
};
enum class IndexedPaneDataJobState { Sorting, Indexing, Complete, Failed, Cancelled, Abandoned };
class IndexedPaneDataJob final {
  public:
    using Result = std::shared_ptr<const CompletedIndexedPaneData>;
    IndexedPaneDataJob(std::shared_ptr<const CompletedDirectoryIndex> listing, PaneOrder order,
                       DirectoryIndexSortLimits sort_limits = {}, DirectoryIndexScanLimits lookup_limits = {},
                       std::uint64_t lookup_bytes = 256ULL * 1024 * 1024);
    // Reorders an existing dataset, retaining original row IDs and directory pin.
    // Target must be a later request in the same pane namespace.
    IndexedPaneDataJob(Result data, PaneListingRequest target, PaneOrder order,
                       DirectoryIndexSortLimits sort_limits = {}, DirectoryIndexScanLimits lookup_limits = {},
                       std::uint64_t lookup_bytes = 256ULL * 1024 * 1024);
    IndexedPaneDataJob(const IndexedPaneDataJob &) = delete;
    IndexedPaneDataJob &operator=(const IndexedPaneDataJob &) = delete;
    IndexedPaneDataJobState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    IndexedPaneDataJobState state() const noexcept {
        return state_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    Observation<Result> completed() const;

  private:
    void initialize(PaneOrder current, DirectoryIndexSortLimits limits);
    void release() noexcept;
    void fail(UnavailableObservation detail, bool stopped = false);
    void cancel() noexcept;
    PaneListingRequest request_;
    PaneOrder order_;
    IndexedPaneDataOrigin origin_;
    AdapterScope source_scope_;
    std::optional<AdapterScope> predecessor_;
    Observation<ObjectIdentity> directory_;
    Observation<BasicMetadata> before_, previous_after_;
    BasicMetadata baseline_;
    std::shared_ptr<const DirectoryCursor> pin_;
    std::shared_ptr<DirectoryIndexStore> store_;
    DirectoryIndexScanLimits lookup_limits_;
    std::uint64_t lookup_bytes_;
    std::unique_ptr<DirectoryIndexSortJob> sort_;
    std::unique_ptr<DirectoryIndexLookupJob> lookup_;
    IndexedPaneDataJobState state_ = IndexedPaneDataJobState::Sorting;
    std::optional<UnavailableObservation> failure_;
    Result result_;
};
struct IndexedPanePageLimits {
    PanePageLimits display;
    std::size_t wire_bytes = 4 * 1024 * 1024;
};
class IndexedPanePage final {
  public:
    const PaneStateStamp &stamp() const noexcept {
        return stamp_;
    }
    const std::vector<PaneDisplayRow> &rows() const noexcept {
        return rows_;
    }
    std::uint64_t first() const noexcept {
        return first_;
    }
    std::uint64_t next() const noexcept {
        return next_;
    }
    std::uint64_t total_rows() const noexcept {
        return total_;
    }
    std::size_t text_bytes() const noexcept {
        return text_bytes_;
    }
    bool end() const noexcept {
        return next_ == total_;
    }
    PaneOrder order() const noexcept {
        return order_;
    }
    bool listing_pending() const noexcept {
        return pending_;
    }
    const std::optional<UnavailableObservation> &listing_error() const noexcept {
        return error_;
    }

  private:
    friend class IndexedPane;
    IndexedPanePage() = default;
    PaneStateStamp stamp_;
    std::vector<PaneDisplayRow> rows_;
    std::uint64_t first_ = 0, next_ = 0, total_ = 0;
    std::size_t text_bytes_ = 0;
    PaneOrder order_;
    bool pending_ = false;
    std::optional<UnavailableObservation> error_;
};
enum class IndexedPaneCaptureState { Ready, Stale, OutOfRange, LimitExceeded, Failed, Cancelled };
struct IndexedPaneCapture {
    IndexedPaneCaptureState state;
    std::shared_ptr<const IndexedPanePage> page;
};
enum class IndexedPanePublish { Applied, Stale, InvalidData, Failed, Cancelled };
enum class IndexedPaneFocus { Applied, Stale, Unavailable, InvalidTarget, Failed, Cancelled };
// Serialized worker state. Navigation/reorder requests and all successful state
// changes revoke previously issued stamps. Existing data survives navigation
// failures. Capture returns only bounded owner-free immutable display payloads.
// Filtering/marking/acquisition are subsequent indexed services, not silently
// emulated through an in-memory directory vector or partially accepted commands.
class IndexedPane final {
  public:
    using Data = IndexedPaneDataJob::Result;
    IndexedPane() = default;
    ~IndexedPane();
    IndexedPane(const IndexedPane &) = delete;
    IndexedPane &operator=(const IndexedPane &) = delete;
    PaneStateStamp state_stamp() const;
    bool accepts(const PaneStateStamp &stamp) const noexcept;
    PaneListingRequest begin_listing(PaneOrder order);
    Observation<PaneListingRequest> begin_reorder(const PaneStateStamp &stamp, PaneOrder order);
    IndexedPanePublish publish(Data data, std::stop_token stop = {});
    bool fail_listing(PaneListingRequest request, UnavailableObservation failure);
    bool cancel_listing(PaneListingRequest request) noexcept;
    IndexedPaneCapture capture_page(const PaneStateStamp &stamp, std::uint64_t first, IndexedPanePageLimits limits = {},
                                    std::stop_token stop = {});
    IndexedPaneFocus focus(const PaneStateStamp &stamp, PaneRowId row, std::stop_token stop = {});
    bool listing_pending() const noexcept {
        return pending_ != 0;
    }
    std::uint64_t rows() const noexcept {
        return data_ ? data_->rows() : 0;
    }
    const Data &data() const noexcept {
        return data_;
    } // owning worker only
    const std::optional<PaneRowId> &focused() const noexcept {
        return focus_;
    }
    const std::optional<UnavailableObservation> &listing_error() const noexcept {
        return error_;
    }

  private:
    PaneListingRequest begin(IndexedPaneDataOrigin kind, PaneOrder order);
    bool current(PaneListingRequest request) const noexcept;
    void invalidate() noexcept;
    void io_failure(UnavailableObservation error);
    AdapterScope scope_;
    std::uint64_t next_request_ = 1, pending_ = 0;
    IndexedPaneDataOrigin pending_kind_ = IndexedPaneDataOrigin::Listing;
    PaneOrder pending_order_;
    std::optional<AdapterScope> pending_dataset_, pending_revision_;
    mutable std::shared_ptr<PaneStateStamp::Token> stamp_;
    Data data_;
    std::optional<PaneRowId> focus_;
    std::optional<UnavailableObservation> error_;
};
} // namespace ck::files
