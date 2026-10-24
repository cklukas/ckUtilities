// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "held_child.hpp"
#include "native_name.hpp"
#include <limits>
#include <atomic>
#include <vector>
#include <unordered_map>

namespace ck::files {
// A stamp owns only a revocation bit, never a model or native resource. It is
// safe to query on the UI thread; all model access remains worker-serialized.
class PaneStateStamp final {
public:
    PaneStateStamp() = default;
    bool current() const noexcept { return token_ && token_->current.load(std::memory_order_acquire); }
private:
    struct Token { std::atomic<bool> current{true}; };
    std::shared_ptr<Token> token_;
    friend class PaneModel;
};
struct ListedEntry;
class CompletedPaneListing;
void validate_pane_limits(struct PaneLimits limits);
// Pane handles name prepared rows, never native objects or mutation authority.
// A scope prevents another pane's handle from accidentally selecting this pane.
struct PaneRowId {
    AdapterScope scope;
    std::uint64_t number = 0;
    friend bool operator==(const PaneRowId&, const PaneRowId&) = default;
};
struct PaneListingRequest {
    AdapterScope scope;
    std::uint64_t number = 0;
};
struct PaneEntry {
    NativeName name;
    BasicMetadata metadata;
    Observation<bool> hidden; // unknown/denied entries remain visible
    PaneEntry(NativeName native_name, BasicMetadata observed, Observation<bool> hidden_state)
        : name(std::move(native_name)), metadata(std::move(observed)), hidden(std::move(hidden_state)) {}
    static PaneEntry from_listed(ListedEntry entry, Observation<bool> hidden_state);
    const std::optional<UnavailableObservation>& inspection_error() const noexcept { return inspection_error_; }
    static PaneEntry from_held(HeldChild::Pointer child, Observation<bool> hidden_state);
    const HeldChild::Pointer& held_child() const noexcept { return held_; }
private:
    HeldChild::Pointer held_;
    std::optional<UnavailableObservation> inspection_error_;
};
struct PaneBindingIssue {
    NativeName name;
    UnavailableObservation detail;
};
struct PaneRow {
    PaneRowId id;
    PaneEntry entry;
    std::string display_name;
};
enum class PaneSortKey { Name, Kind, Size, Modified };
struct PaneView {
    PaneSortKey sort = PaneSortKey::Name;
    bool descending = false;
    bool folders_first = true; // only observed Directory, never followed links
    bool show_hidden = false;
    // Substring of prepared escaped display text; exact UTF-8 bytes, optional
    // ASCII-only folding. No locale/Unicode normalization or native-name decode.
    std::string display_substring;
    bool ascii_case_sensitive = false;
};
struct PaneLimits {
    std::size_t rows = 1'000'000;
    std::size_t native_name_bytes = 64 * 1024 * 1024;
    std::size_t display_name_bytes = 128 * 1024 * 1024;
    std::size_t held_entries = 1024; // model bound, not a promise about OS handle limits
};
enum class PaneListingPurpose { Navigate, Refresh };
struct PaneInvalidatedSelection {
    std::size_t marks = 0;
    bool cursor = false;
};
enum class PanePublish { Applied, Stale, InvalidListing, LimitExceeded, Exhausted };
enum class PaneSourcesState { Empty, Ready, HiddenMarksNeedReview, ListingPending, SelectionInvalidated };
enum class PaneSourcesOrigin { Cursor, Marks };
struct PaneMarkCounts {
    std::size_t total = 0;
    std::size_t visible = 0;
    std::size_t hidden = 0;
};
struct PaneSelectionSummary {
    PaneSourcesState state = PaneSourcesState::Empty;
    PaneSourcesOrigin origin = PaneSourcesOrigin::Cursor;
    std::size_t visible_count = 0; // exact candidate counts; zero while blocked
    std::size_t hidden_count = 0;
    PaneMarkCounts marks; // retained marks, also available during listing/review
};
struct PaneSource {
    PaneRowId row;
    NativeName name;
    Observation<ObjectIdentity> observed_identity;
    std::optional<UnavailableObservation> inspection_error;
};
struct PaneSources {
    PaneSourcesState state = PaneSourcesState::Empty;
    PaneSourcesOrigin origin = PaneSourcesOrigin::Cursor;
    std::size_t visible_count = 0;
    std::size_t hidden_count = 0;
    // Exact candidates only, not a frozen inventory or permission to operate.
    // HiddenMarksNeedReview must enter the operation review flow, not execute.
    std::vector<PaneSource> candidates;
};

// Single-owner domain state. Sorting/filtering/publishing are worker-side work;
// rendering consumes an immutable prepared projection, not these mutators.
// No filesystem I/O and no process cwd changes occur in this model.
class PaneModel final {
public:
    explicit PaneModel(PaneLimits limits = {});
    ~PaneModel();
    // Worker-only capture/validation. A current stamp prevents stale model
    // commands; it is not a filesystem snapshot or operation permission.
    PaneStateStamp state_stamp() const;
    bool accepts(const PaneStateStamp& stamp) const noexcept;
    PaneModel(const PaneModel&) = delete;
    PaneModel& operator=(const PaneModel&) = delete;
    // Caller must distinguish navigation from refresh. Newer requests supersede
    // older ones. Refresh retains only entries with sealed held-child successor
    // observations; other old selections are explicitly invalidated.
    PaneListingRequest begin_listing(PaneListingPurpose purpose);
    PanePublish publish(const CompletedPaneListing& listing);
    const Observation<ObjectIdentity>& listing_directory() const noexcept { return listing_directory_; }
    PanePublish publish(PaneListingRequest request, const std::vector<PaneEntry>& entries);
    bool fail_listing(PaneListingRequest request, UnavailableObservation failure);
    // Revoke this pending generation, preserving published rows/context and
    // selection diagnostics. Does not stop or free a worker's in-flight I/O.
    bool cancel_listing(PaneListingRequest request) noexcept;
    bool listing_pending() const noexcept { return pending_ != 0; }
    const std::optional<UnavailableObservation>& listing_error() const noexcept { return error_; }
    const std::vector<PaneBindingIssue>& binding_issues() const noexcept { return binding_issues_; }
    // Worker-side capture; no I/O. Includes hidden/unmarked held rows as well.
    std::vector<HeldChild::Pointer> held_entries() const;
    const PaneInvalidatedSelection& invalidated_selection() const noexcept { return invalidated_; }
    const PaneView& view() const noexcept { return view_; }
    void set_view(const PaneView& view); // throws invalid_argument for invalid enum/oversize filter
    std::size_t size() const noexcept { return rows_.size(); }
    std::size_t visible_size() const noexcept { return visible_.size(); }
    const PaneRow* row(PaneRowId id) const noexcept;
    const PaneRow* visible_row(std::size_t index) const noexcept;
    std::optional<std::size_t> visible_index(PaneRowId id) const noexcept;
    bool focus(PaneRowId id) noexcept; // visible rows only; failure keeps focus
    std::optional<PaneRowId> focused() const { return focus_; }
    bool set_mark(PaneRowId id, bool marked) noexcept; // visible rows only
    bool marked(PaneRowId id) const noexcept;
    void mark_visible(bool marked) noexcept;
    void invert_visible_marks() noexcept;
    bool mark_range(std::size_t first, std::size_t last, bool marked) noexcept;
    void clear_marks() noexcept;
    // No allocation, native ownership or row traversal; uses cached counts and
    // the existing row-ID index for cursor visibility.
    PaneSelectionSummary selection_summary() const noexcept;
    PaneSources sources() const;
private:
    struct Projection { std::vector<std::size_t> visible; std::vector<std::optional<std::size_t>> positions; };
    static Projection project(const std::vector<PaneRow>& rows, const PaneView& view);
    std::optional<std::size_t> index(PaneRowId id) const noexcept;
    bool current(PaneListingRequest request) const noexcept;
    void invalidate_stamp() noexcept;
    void set_index_mark(std::size_t index, bool marked) noexcept;
    mutable std::shared_ptr<PaneStateStamp::Token> stamp_;
    PaneLimits limits_;
    AdapterScope scope_;
    std::uint64_t next_row_ = 1;
    std::uint64_t next_request_ = 1;
    std::uint64_t pending_ = 0;
    std::unordered_map<std::uint64_t, std::size_t> indexes_;
    std::vector<PaneRow> rows_;
    std::vector<std::size_t> visible_;
    std::vector<std::optional<std::size_t>> positions_;
    std::vector<bool> marks_;
    std::size_t marked_count_ = 0, visible_marked_count_ = 0;
    std::optional<PaneRowId> focus_;
    std::optional<UnavailableObservation> error_;
    PaneView view_;
    PaneListingPurpose purpose_ = PaneListingPurpose::Navigate;
    PaneInvalidatedSelection invalidated_;
    Observation<ObjectIdentity> listing_directory_;
    std::vector<PaneBindingIssue> binding_issues_;
};
} // namespace ck::files
