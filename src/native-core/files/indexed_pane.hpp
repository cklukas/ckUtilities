// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_listing_job.hpp"
#include "directory_index_lookup.hpp"
#include "pane_page.hpp"
#include "indexed_pane_marks.hpp"
#include "indexed_pane_sources.hpp"
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
    const PaneView &view() const noexcept {
        return view_;
    }
    std::uint64_t visible_rows() const noexcept {
        return lookup_->rows();
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
    friend class IndexedPaneMarkJob;
    friend class IndexedPaneSourceJob;
    CompletedIndexedPaneData(PaneListingRequest request, PaneOrder order, PaneView view, IndexedPaneDataOrigin origin,
                             AdapterScope scope, std::optional<AdapterScope> predecessor,
                             Observation<ObjectIdentity> directory, Observation<BasicMetadata> before,
                             Observation<BasicMetadata> after, BasicMetadata baseline,
                             std::shared_ptr<const DirectoryCursor> pin, std::shared_ptr<DirectoryIndexStore> store,
                             std::shared_ptr<DirectoryIndexLookup> lookup, std::optional<AdapterScope> marks_revision,
                             std::uint64_t visible_marks)
        : request_(std::move(request)), order_(order), view_(std::move(view)), origin_(origin),
          source_scope_(std::move(scope)), predecessor_(std::move(predecessor)), directory_(std::move(directory)),
          before_(std::move(before)), after_(std::move(after)), baseline_(std::move(baseline)), pin_(std::move(pin)),
          store_(std::move(store)), lookup_(std::move(lookup)), marks_revision_(std::move(marks_revision)),
          visible_marks_(visible_marks) {
    }
    PaneListingRequest request_;
    PaneOrder order_;
    PaneView view_;
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
    std::optional<AdapterScope> marks_revision_;
    std::uint64_t visible_marks_ = 0;
};
enum class IndexedPaneDataJobState { Sorting, Indexing, CountingMarks, Complete, Failed, Cancelled, Abandoned };
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
                       std::uint64_t lookup_bytes = 256ULL * 1024 * 1024,
                       std::shared_ptr<DirectoryIndexMarks> marks = {});
    IndexedPaneDataJob(std::shared_ptr<const CompletedDirectoryIndex> listing, const PaneView &view,
                       DirectoryIndexSortLimits sort_limits = {}, DirectoryIndexScanLimits lookup_limits = {},
                       std::uint64_t lookup_bytes = 256ULL * 1024 * 1024);
    IndexedPaneDataJob(Result data, PaneListingRequest target, const PaneView &view,
                       DirectoryIndexSortLimits sort_limits = {}, DirectoryIndexScanLimits lookup_limits = {},
                       std::uint64_t lookup_bytes = 256ULL * 1024 * 1024,
                       std::shared_ptr<DirectoryIndexMarks> marks = {});
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
    PaneView view_;
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
    std::shared_ptr<DirectoryIndexMarks> marks_;
    std::unique_ptr<DirectoryMarksRecountJob> recount_;
    std::uint64_t counted_visible_ = 0;
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
    std::uint64_t visible_rows() const noexcept {
        return visible_;
    }
    const PaneView &view() const noexcept {
        return view_;
    }
    std::optional<std::uint64_t> focused_position() const noexcept {
        return focused_position_;
    }
    std::size_t text_bytes() const noexcept {
        return text_bytes_;
    }
    bool end() const noexcept {
        return next_ == visible_;
    }
    PaneOrder order() const noexcept {
        return order_;
    }
    const IndexedPaneSelection &selection() const noexcept {
        return selection_;
    }
    bool marking_pending() const noexcept {
        return marking_pending_;
    }
    const std::optional<UnavailableObservation> &mark_error() const noexcept {
        return mark_error_;
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
    std::uint64_t first_ = 0, next_ = 0, total_ = 0, visible_ = 0;
    std::size_t text_bytes_ = 0;
    PaneOrder order_;
    PaneView view_;
    std::optional<std::uint64_t> focused_position_;
    bool pending_ = false, marking_pending_ = false;
    IndexedPaneSelection selection_;
    std::optional<UnavailableObservation> mark_error_;
    std::optional<UnavailableObservation> error_;
};
enum class IndexedPaneCaptureState { Ready, Stale, OutOfRange, LimitExceeded, Failed, Cancelled };
struct IndexedPaneCapture {
    IndexedPaneCaptureState state;
    std::shared_ptr<const IndexedPanePage> page;
};
enum class IndexedPanePublish { Applied, Stale, InvalidData, Failed, Cancelled };
enum class IndexedPaneFocus { Applied, Stale, Unavailable, InvalidTarget, Failed, Cancelled };
struct IndexedDirectoryOpenLimits {
    std::size_t wire_bytes = DirectoryIndexScanLimits::hard_wire_bytes;
    std::size_t native_name_bytes = 64 * 1024;
    std::size_t diagnostic_bytes = 64 * 1024;
};
// Worker-owned live child binding. The name is exact native data, not a path.
// This result has native owners: never publish it to the UI. No cached listing
// identity, even a matching one, proves continuity with an unheld discovery.
struct OpenedIndexedDirectory {
    NativeName name;
    std::unique_ptr<DirectoryCursor> directory;
};
struct OpenedIndexedRow {
    NativeName name;
    HeldChild::Pointer child;
};
// Serialized worker state. Navigation/reorder requests and all successful state
// changes revoke previously issued stamps. Existing data survives navigation
// failures. Capture returns only bounded owner-free immutable display payloads.
// Filtering and marks use bounded disk views; native acquisition remains a
// separate service. Hidden marks block operation candidates until reviewed.
// No directory-sized RAM fallback or partially accepted unsupported commands.
class IndexedPane final {
  public:
    using Data = IndexedPaneDataJob::Result;
    IndexedPane() = default;
    ~IndexedPane();
    IndexedPane(const IndexedPane &) = delete;
    IndexedPane &operator=(const IndexedPane &) = delete;
    PaneStateStamp state_stamp() const;
    bool accepts(const PaneStateStamp &stamp) const noexcept;
    // Order-only navigation intentionally prepares an unfiltered dataset.
    // PaneView navigation uses its explicit hidden/substr/case flags.
    PaneListingRequest begin_listing(PaneOrder order);
    Observation<PaneListingRequest> begin_reorder(const PaneStateStamp &stamp, PaneOrder order);
    PaneListingRequest begin_listing(const PaneView &view);
    Observation<PaneListingRequest> begin_view_change(const PaneStateStamp &stamp, const PaneView &view);
    IndexedPanePublish publish(Data data, std::stop_token stop = {});
    bool fail_listing(PaneListingRequest request, UnavailableObservation failure);
    bool cancel_listing(PaneListingRequest request) noexcept;
    IndexedPaneCapture capture_page(const PaneStateStamp &stamp, std::uint64_t first, IndexedPanePageLimits limits = {},
                                    std::stop_token stop = {});
    // Open exactly one visible row, independent of cursor/marks. Bounded indexed
    // lookup, held-parent relative no-follow open, live retained-child identity
    // checks and parent observations. Unknown/contradictory identities refuse;
    // discovery-ID equality is only a contradiction check, never snapshot or
    // mutation authority. Native errors take precedence over concurrent cancel.
    // Does not change this pane or enumerate the opened directory.
    Observation<OpenedIndexedDirectory> open_directory_row(const PaneStateStamp &stamp, PaneRowId row,
                                                           IndexedDirectoryOpenLimits limits = {},
                                                           std::stop_token stop = {});
    // Worker-only destination handoff. Retains the already held directory
    // cursor after the stamp and sealed dataset are checked; it performs no
    // path reopen and publishes no native owner to UI state.
    Observation<std::shared_ptr<const DirectoryCursor>> hold_directory(const PaneStateStamp &stamp) const;
    Observation<OpenedIndexedRow> open_row(const PaneStateStamp &stamp, PaneRowId row, EntryAccess access,
                                           IndexedDirectoryOpenLimits limits = {}, std::stop_token stop = {});
    IndexedPaneFocus focus(const PaneStateStamp &stamp, PaneRowId row, std::stop_token stop = {});
    Observation<std::unique_ptr<IndexedPaneMarkJob>>
    begin_mark_change(const PaneStateStamp &stamp, const IndexedMarkCommand &command, std::size_t rows_per_step = 256,
                      std::uint64_t max_bytes = 64ULL * 1024 * 1024, std::stop_token stop = {});
    IndexedPanePublish publish_marks(IndexedPaneMarkJob::Result result);
    bool fail_marks(const PaneStateStamp &stamp, UnavailableObservation failure);
    bool cancel_marks(const PaneStateStamp &stamp) noexcept;
    IndexedPaneSelection selection() const noexcept;
    Observation<std::unique_ptr<IndexedPaneSourceJob>> begin_source_acquisition(const PaneStateStamp &stamp,
                                                                                IndexedSourceLimits limits = {});
    // Review delivery only; never an operation permission or continuity proof.
    bool accepts_sources(const CompletedIndexedSources &sources) const noexcept;
    const std::shared_ptr<DirectoryIndexMarks> &marks() const noexcept {
        return marks_;
    } // worker only
    bool marking_pending() const noexcept {
        return mark_pending_;
    }
    bool listing_pending() const noexcept {
        return pending_ != 0;
    }
    std::uint64_t rows() const noexcept {
        return data_ ? data_->rows() : 0;
    }
    std::uint64_t visible_rows() const noexcept {
        return data_ ? data_->visible_rows() : 0;
    }
    std::optional<std::uint64_t> focused_position() const noexcept {
        return focus_position_;
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
    PaneListingRequest begin(IndexedPaneDataOrigin kind, const PaneView &view);
    bool current(PaneListingRequest request) const noexcept;
    void invalidate() noexcept;
    void io_failure(UnavailableObservation error);
    AdapterScope scope_;
    std::uint64_t next_request_ = 1, pending_ = 0;
    IndexedPaneDataOrigin pending_kind_ = IndexedPaneDataOrigin::Listing;
    PaneOrder pending_order_;
    PaneView pending_view_;
    std::optional<AdapterScope> pending_dataset_, pending_revision_;
    mutable std::shared_ptr<PaneStateStamp::Token> stamp_;
    Data data_;
    std::optional<PaneRowId> focus_;
    std::optional<std::uint64_t> focus_position_;
    std::optional<UnavailableObservation> error_;
    std::shared_ptr<DirectoryIndexMarks> marks_;
    std::uint64_t visible_marks_ = 0;
    bool mark_pending_ = false;
    std::optional<UnavailableObservation> mark_error_;
};
} // namespace ck::files
