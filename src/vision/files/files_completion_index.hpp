// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "files_location_text.hpp"
#include "directory_index_listing_job.hpp"
namespace ck::vision {
struct FilesCompletionIndexLimits {
    ck::files::DirectoryIndexScanLimits scan{256, 4 * 1024 * 1024};
    std::size_t candidates = 64, text_bytes = 32768, native_name_bytes = 64 * 1024;
    std::size_t candidate_text_bytes = 32768;
    bool show_hidden = true;
};
class FilesCompletionIndexJob;
// Owner-free, same-lifetime continuation. Contains only a scan position and
// immutable query values. No directory, index file, identity or operation owner.
// The same query, canonical dataset, scan bounds, hidden policy and candidate
// eligibility limit are required. Aggregate output page bounds may change.
class FilesCompletionCursor final {
  private:
    friend class FilesCompletionIndexJob;
    FilesCompletionCursor(ck::files::AdapterScope scope, ck::files::IndexScanCursor scan, std::size_t skip,
                          std::shared_ptr<const FilesLocationCompletionQuery> query, FilesCompletionIndexLimits limits)
        : scope_(std::move(scope)), scan_(std::move(scan)), skip_(skip), query_(std::move(query)), limits_(limits) {}
    ck::files::AdapterScope scope_;
    ck::files::IndexScanCursor scan_;
    std::size_t skip_;
    std::shared_ptr<const FilesLocationCompletionQuery> query_;
    FilesCompletionIndexLimits limits_;
};
struct FilesCompletionEntry {
    std::uint64_t row; // original ordinal in index_scope; never native authority
    ck::files::NativeName name;
    std::string text;
};
struct FilesCompletionPage {
    ck::files::AdapterScope index_scope;
    std::vector<FilesCompletionEntry> entries;
    std::optional<FilesCompletionCursor> next;
    std::size_t text_bytes = 0, native_name_bytes = 0;
    std::uint64_t examined = 0, unknown_kind = 0, unknown_hidden = 0, unavailable_proposals = 0;
    std::optional<ck::files::UnavailableObservation> first_proposal_error;
    // Exhaustion covers the indexed observations, not a live filesystem or
    // source snapshot. A resumed page is a subset even when exhausted; neither
    // it nor an uncertain page can become an apparent unique/full match set.
    bool exhausted = false, has_previous = false;
    bool complete() const noexcept {
        return exhausted && !has_previous && !unknown_kind && !unknown_hidden && !unavailable_proposals;
    }
};
enum class FilesCompletionIndexState { Running, Complete, Failed, Cancelled, Abandoned };
// Worker-only query against the canonical native-name order certified by
// CompletedDirectoryIndex. No new enumeration/sort and no RAM source copy.
// The composition root must bind query.parent() to the source it actually
// opened. A page is address discovery, never freshness/identity or permission.
// Each advance reads at most one bounded index page and examines <=scan.rows
// records. Retained output is <=64 candidates plus explicit byte bounds.
// Continuations reread one bounded page and skip its already consumed prefix;
// they never omit the remainder or restart a native enumeration.
// Cancellation/failed I/O/exception withdraw all staged results; native read
// failures retain their meaning even if stop arrives during that bounded read.
// All owner retirement, including destruction, belongs to the worker.
class FilesCompletionIndexJob final {
  public:
    using Result = std::shared_ptr<const FilesCompletionPage>;
    FilesCompletionIndexJob(std::shared_ptr<const ck::files::CompletedDirectoryIndex> source,
                            FilesLocationCompletionQuery query, FilesCompletionIndexLimits limits = {},
                            std::optional<FilesCompletionCursor> after = {});
    FilesCompletionIndexJob(const FilesCompletionIndexJob &) = delete;
    FilesCompletionIndexJob &operator=(const FilesCompletionIndexJob &) = delete;
    FilesCompletionIndexState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    FilesCompletionIndexState state() const noexcept { return state_; }
    const std::optional<ck::files::UnavailableObservation> &failure() const noexcept { return failure_; }
    ck::files::Observation<Result> completed() const;

  private:
    static bool same_query(const FilesLocationCompletionQuery &a, const FilesLocationCompletionQuery &b);
    void release() noexcept;
    void fail(ck::files::UnavailableObservation error);
    void cancel() noexcept;
    void finish(bool exhausted);
    int compare_prefix(const ck::files::NativeName &name) const;
    std::shared_ptr<const ck::files::CompletedDirectoryIndex> source_;
    std::shared_ptr<const FilesLocationCompletionQuery> query_;
    FilesCompletionIndexLimits limits_;
    std::optional<ck::files::IndexScanCursor> cursor_;
    std::size_t skip_ = 0;
    FilesCompletionPage staging_;
    Result result_;
    FilesCompletionIndexState state_ = FilesCompletionIndexState::Running;
    std::optional<ck::files::UnavailableObservation> failure_;
};
} // namespace ck::vision
