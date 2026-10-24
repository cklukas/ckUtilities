// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_store.hpp"
#include "pane_order.hpp"
#include <array>
namespace ck::files {
struct DirectoryIndexSortLimits {
    DirectoryIndexScanLimits block{1024, 4 * 1024 * 1024};
    std::size_t merge_rows = 256; // successful output records per advance
};
enum class DirectoryIndexSortState { Running, Complete, Failed, Cancelled, Abandoned };
// Worker-only external merge sort. One bounded source block, then at most two
// bounded input pages and one output record during a merge. Binary carry runs
// use a fixed 64-level table, never an array proportional to the source rows.
// All scratch runs use the source factory/shared ledger, preserving row IDs,
// observations and row_scope. No filtering, native acquisition or source writes.
class DirectoryIndexSortJob final {
  public:
    DirectoryIndexSortJob(std::shared_ptr<DirectoryIndexStore> source, PaneOrder order,
                          DirectoryIndexSortLimits limits = {});
    ~DirectoryIndexSortJob();
    DirectoryIndexSortJob(const DirectoryIndexSortJob &) = delete;
    DirectoryIndexSortJob &operator=(const DirectoryIndexSortJob &) = delete;
    // One bounded generation/insertion step or <=merge_rows merged records.
    // Source reads finish a bounded page before observing cancellation, keeping
    // an otherwise healthy source usable. OS calls cannot be interrupted.
    DirectoryIndexSortState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    DirectoryIndexSortState state() const noexcept {
        return state_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    Observation<std::shared_ptr<DirectoryIndexStore>> completed() const;

  private:
    struct Stream;
    struct Merge;
    std::unique_ptr<DirectoryIndexStore> new_run(std::stop_token stop);
    bool generate(std::stop_token stop);
    bool start_merge(std::unique_ptr<DirectoryIndexStore> a, std::unique_ptr<DirectoryIndexStore> b,
                     std::stop_token stop);
    bool merge_step(std::stop_token stop);
    const DirectoryIndexRecord *head(Stream &stream, std::stop_token stop);
    int compare(const DirectoryIndexRecord &a, const DirectoryIndexRecord &b) const;
    void fail(UnavailableObservation error, bool stopped = false);
    void cancel() noexcept;
    void release() noexcept;
    std::shared_ptr<DirectoryIndexStore> source_, result_;
    PaneOrder order_;
    DirectoryIndexSortLimits limits_;
    std::optional<IndexScanCursor> source_cursor_;
    std::array<std::unique_ptr<DirectoryIndexStore>, 64> levels_;
    std::unique_ptr<DirectoryIndexStore> carry_;
    std::unique_ptr<Merge> merge_;
    std::size_t level_ = 0;
    bool source_end_ = false, finalizing_ = false;
    DirectoryIndexSortState state_ = DirectoryIndexSortState::Running;
    std::optional<UnavailableObservation> failure_;
};
} // namespace ck::files
