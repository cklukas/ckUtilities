// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "indexed_pane_marks.hpp"
namespace ck::files {
class CompletedIndexedPaneData;
class IndexedPane;
struct IndexedSourceLimits {
    std::size_t held_entries = 1024;
    std::size_t native_name_bytes = 4 * 1024 * 1024;
    std::size_t diagnostic_bytes = 4 * 1024 * 1024;
    std::size_t scan_rows = 256;
    std::size_t wire_bytes = DirectoryIndexScanLimits::hard_wire_bytes;
};
class IndexedPaneSourceJob;
// Worker-only fresh binding. Discovery keys/metadata remain observations, never
// continuity proof. Binding keys have a new namespace, even for matching IDs.
class IndexedBoundSource final {
  public:
    const PaneRowId &discovery_row() const noexcept {
        return discovery_;
    }
    const PaneRowId &binding_row() const noexcept {
        return binding_;
    }
    const HeldChild::Pointer &held() const noexcept {
        return held_;
    }
    const Observation<BasicMetadata> &discovery_metadata() const noexcept {
        return cached_;
    }
    IdentityComparison cached_identity_match() const noexcept;

  private:
    friend class IndexedPaneSourceJob;
    IndexedBoundSource(PaneRowId discovery, PaneRowId binding, HeldChild::Pointer held,
                       Observation<BasicMetadata> cached)
        : discovery_(std::move(discovery)), binding_(std::move(binding)), held_(std::move(held)),
          cached_(std::move(cached)) {
    }
    PaneRowId discovery_, binding_;
    HeldChild::Pointer held_;
    Observation<BasicMetadata> cached_;
};
class CompletedIndexedSources final {
  public:
    const PaneStateStamp &stamp() const noexcept {
        return stamp_;
    }
    const std::vector<IndexedBoundSource> &sources() const noexcept {
        return sources_;
    }
    const BasicMetadata &directory_before() const noexcept {
        return before_;
    }
    const BasicMetadata &directory_after() const noexcept {
        return after_;
    }
    std::size_t native_name_bytes() const noexcept {
        return names_;
    }
    std::size_t diagnostic_bytes() const noexcept {
        return diagnostics_;
    }
    // New unheld-discovery bindings always require explicit source review.
    // Neither cached-ID equality nor this batch authorizes a native operation.
    bool review_required() const noexcept {
        return true;
    }

  private:
    friend class IndexedPaneSourceJob;
    friend class IndexedPane;
    CompletedIndexedSources(PaneStateStamp stamp, AdapterScope dataset, std::optional<AdapterScope> marks,
                            PaneSourcesOrigin origin, BasicMetadata before, BasicMetadata after,
                            std::vector<IndexedBoundSource> sources, std::size_t names, std::size_t diagnostics)
        : stamp_(std::move(stamp)), dataset_(std::move(dataset)), marks_(std::move(marks)), origin_(origin),
          before_(std::move(before)), after_(std::move(after)), sources_(std::move(sources)), names_(names),
          diagnostics_(diagnostics) {
    }
    PaneStateStamp stamp_;
    AdapterScope dataset_;
    std::optional<AdapterScope> marks_;
    PaneSourcesOrigin origin_;
    BasicMetadata before_, after_;
    std::vector<IndexedBoundSource> sources_;
    std::size_t names_, diagnostics_;
};
enum class IndexedSourceJobState { Running, Complete, Failed, Cancelled, Stale, Abandoned };
// Serialized worker job; no path/display parsing, content access or mutation.
// Full candidate count is checked before I/O. At most one native child opens
// per advance; bitmap scanning retains one block and examines <=scan_rows IDs.
// No prefix result is exposed after error/cancellation/staleness. Held-resource
// limits reject explicitly; the caller cannot silently truncate or reopen pins.
class IndexedPaneSourceJob final {
  public:
    using Result = std::shared_ptr<const CompletedIndexedSources>;
    IndexedPaneSourceJob(const IndexedPaneSourceJob &) = delete;
    IndexedPaneSourceJob &operator=(const IndexedPaneSourceJob &) = delete;
    IndexedSourceJobState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    IndexedSourceJobState state() const noexcept {
        return state_;
    }
    const std::optional<UnavailableObservation> &failure() const noexcept {
        return failure_;
    }
    Observation<Result> completed() const;

  private:
    friend class IndexedPane;
    IndexedPaneSourceJob(std::shared_ptr<const CompletedIndexedPaneData> data,
                         std::shared_ptr<DirectoryIndexMarks> marks, std::optional<PaneRowId> focus,
                         PaneStateStamp stamp, PaneSourcesOrigin origin, std::uint64_t expected,
                         IndexedSourceLimits limits);
    enum class Phase { Before, Scan, Acquire, After };
    void release() noexcept;
    void fail(UnavailableObservation error);
    void finish_without_result(IndexedSourceJobState state) noexcept;
    bool boundary(std::stop_token stop);
    Observation<BasicMetadata> inspect_directory(std::stop_token stop);
    void scan(std::stop_token stop);
    void acquire(std::stop_token stop);
    std::shared_ptr<const CompletedIndexedPaneData> data_;
    std::shared_ptr<DirectoryIndexMarks> marks_;
    std::optional<PaneRowId> candidate_;
    PaneStateStamp stamp_;
    PaneSourcesOrigin origin_;
    std::uint64_t expected_, next_ = 0, counted_ = 0;
    IndexedSourceLimits limits_;
    AdapterScope bindings_scope_;
    std::optional<DirectoryIndexMarks::Bits> bits_;
    std::uint64_t block_ = 0;
    BasicMetadata before_;
    std::vector<IndexedBoundSource> sources_;
    std::size_t names_ = 0, diagnostics_ = 0;
    Phase phase_ = Phase::Before;
    IndexedSourceJobState state_ = IndexedSourceJobState::Running;
    std::optional<UnavailableObservation> failure_;
    Result result_;
};
} // namespace ck::files
