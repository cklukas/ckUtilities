// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_marks.hpp"
#include "pane_page.hpp"
#include <variant>
namespace ck::files {
class CompletedIndexedPaneData;
class IndexedPane;
struct IndexedMarkRow {
    PaneRowId row;
    bool marked;
};
struct IndexedMarkRange {
    PaneRowId first, last;
    bool marked;
};
struct IndexedMarkVisible {
    bool marked;
};
struct IndexedInvertVisible {};
struct IndexedClearMarks {};
using IndexedMarkCommand =
    std::variant<IndexedMarkRow, IndexedMarkRange, IndexedMarkVisible, IndexedInvertVisible, IndexedClearMarks>;
class CompletedIndexedMarkChange final {
  public:
    const PaneStateStamp &stamp() const noexcept {
        return stamp_;
    }
    const IndexedPaneMarkCounts &counts() const noexcept {
        return counts_;
    }

  private:
    friend class IndexedPaneMarkJob;
    friend class IndexedPane;
    CompletedIndexedMarkChange(PaneStateStamp stamp, AdapterScope dataset, std::optional<AdapterScope> base,
                               std::shared_ptr<DirectoryIndexMarks> target, IndexedPaneMarkCounts counts)
        : stamp_(std::move(stamp)), dataset_(std::move(dataset)), base_(std::move(base)), target_(std::move(target)),
          counts_(counts) {
    }
    PaneStateStamp stamp_;
    AdapterScope dataset_;
    std::optional<AdapterScope> base_;
    std::shared_ptr<DirectoryIndexMarks> target_;
    IndexedPaneMarkCounts counts_;
};
enum class IndexedMarkJobState { Running, Complete, Failed, Cancelled, Abandoned };
// Prepared only by the serialized pane. Copy-on-write bitmap blocks provide
// atomic publication, not in-place partial mutation. Individual marks copy
// bitmap blocks and update a validated visible-count delta; bulk/range commands
// inspect at most rows_per_step original IDs per advance through the disk view.
class IndexedPaneMarkJob final {
  public:
    using Result = std::shared_ptr<const CompletedIndexedMarkChange>;
    IndexedPaneMarkJob(const IndexedPaneMarkJob &) = delete;
    IndexedPaneMarkJob &operator=(const IndexedPaneMarkJob &) = delete;
    IndexedMarkJobState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    IndexedMarkJobState state() const noexcept {
        return state_;
    }
    const PaneStateStamp &stamp() const noexcept {
        return stamp_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    Observation<Result> completed() const;

  private:
    friend class IndexedPane;
    IndexedPaneMarkJob(std::shared_ptr<const CompletedIndexedPaneData> data,
                       std::shared_ptr<DirectoryIndexMarks> previous, std::uint64_t visible, PaneStateStamp stamp,
                       const IndexedMarkCommand &command, std::size_t rows_per_step, std::uint64_t max_bytes);
    void release() noexcept;
    void fail(UnavailableObservation detail);
    void cancel() noexcept;
    std::shared_ptr<const CompletedIndexedPaneData> data_;
    std::shared_ptr<DirectoryIndexMarks> previous_, target_;
    PaneStateStamp stamp_;
    IndexedMarkCommand command_;
    AdapterScope dataset_;
    std::optional<AdapterScope> base_;
    std::optional<DirectoryIndexMarks::Bits> bits_;
    std::uint64_t block_ = 0, previous_total_ = 0, total_ = 0, visible_ = 0, low_ = 0, high_ = 0;
    std::size_t at_ = 0, step_;
    IndexedMarkJobState state_ = IndexedMarkJobState::Running;
    std::optional<UnavailableObservation> failure_;
    Result result_;
};
struct IndexedPaneSelection {
    PaneSourcesState state = PaneSourcesState::Empty;
    PaneSourcesOrigin origin = PaneSourcesOrigin::Cursor;
    std::uint64_t candidates = 0;
    std::optional<IndexedPaneMarkCounts> marks;
};
} // namespace ck::files
