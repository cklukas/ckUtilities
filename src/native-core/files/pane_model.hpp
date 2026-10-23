// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "held_child.hpp"
#include "native_name.hpp"
#include <limits>
#include <vector>
#include <unordered_map>

namespace ck::files {
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
    static PaneEntry from_held(HeldChild::Pointer child, Observation<bool> hidden_state);
    const HeldChild::Pointer& held_child() const noexcept { return held_; }
private:
    HeldChild::Pointer held_;
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
struct PaneSource {
    PaneRowId row;
    NativeName name;
    Observation<ObjectIdentity> observed_identity;
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
    PaneModel(const PaneModel&) = delete;
    PaneModel& operator=(const PaneModel&) = delete;
    // Caller must distinguish navigation from refresh. Newer requests supersede
    // older ones. Refresh retains only entries with sealed held-child successor
    // observations; other old selections are explicitly invalidated.
    PaneListingRequest begin_listing(PaneListingPurpose purpose);
    PanePublish publish(PaneListingRequest request, const std::vector<PaneEntry>& entries);
    bool fail_listing(PaneListingRequest request, UnavailableObservation failure);
    bool listing_pending() const noexcept { return pending_ != 0; }
    const std::optional<UnavailableObservation>& listing_error() const noexcept { return error_; }
    const PaneInvalidatedSelection& invalidated_selection() const noexcept { return invalidated_; }
    const PaneView& view() const noexcept { return view_; }
    void set_view(PaneView view); // throws invalid_argument for invalid enum/oversize filter
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
    PaneSources sources() const;
private:
    struct Projection { std::vector<std::size_t> visible; std::vector<std::optional<std::size_t>> positions; };
    static Projection project(const std::vector<PaneRow>& rows, const PaneView& view);
    std::optional<std::size_t> index(PaneRowId id) const noexcept;
    bool current(PaneListingRequest request) const noexcept;
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
    std::optional<PaneRowId> focus_;
    std::optional<UnavailableObservation> error_;
    PaneView view_;
    PaneListingPurpose purpose_ = PaneListingPurpose::Navigate;
    PaneInvalidatedSelection invalidated_;
};
} // namespace ck::files
