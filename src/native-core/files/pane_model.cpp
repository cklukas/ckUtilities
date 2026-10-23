// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_model.hpp"
#include <algorithm>
#include <set>
#include <map>
#include <stdexcept>

namespace ck::files {
namespace {
int name_compare(const NativeName& left, const NativeName& right) {
    if (left.encoding() != right.encoding()) return left.encoding() < right.encoding() ? -1 : 1;
    const auto compare = [](const auto& a, const auto& b) {
        const auto less = [](auto x, auto y) {
            using Unit = std::make_unsigned_t<decltype(x)>;
            return static_cast<Unit>(x) < static_cast<Unit>(y);
        };
        if (std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), less)) return -1;
        return std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end(), less) ? 1 : 0;
    };
    return left.posix_bytes() ? compare(*left.posix_bytes(), *right.posix_bytes()) : compare(*left.windows_units(), *right.windows_units());
}
template<class T, class Less> int observed_compare(const Observation<T>& a, const Observation<T>& b, bool descending, Less less) {
    // All unavailable states stay after available values in either direction.
    if (!a.value() || !b.value()) return a.value() ? -1 : b.value() ? 1 : 0;
    const int order = less(*a.value(), *b.value()) ? -1 : less(*b.value(), *a.value()) ? 1 : 0;
    return descending ? -order : order;
}
bool directory(const PaneRow& row) { return row.entry.metadata.kind.value() && *row.entry.metadata.kind.value() == ObjectKind::Directory; }
bool contains(std::string_view text, std::string_view needle, bool sensitive) {
    const auto fold = [sensitive](unsigned char ch) { return !sensitive && ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
    return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [&](char a, char b) {
        return fold(static_cast<unsigned char>(a)) == fold(static_cast<unsigned char>(b));
    }) != text.end() || needle.empty();
}
void validate_view(const PaneView& view) {
    switch (view.sort) {
    case PaneSortKey::Name: case PaneSortKey::Kind: case PaneSortKey::Size: case PaneSortKey::Modified: break;
    default: throw std::invalid_argument("invalid pane sort key");
    }
    if (view.display_substring.size() > 4096) throw std::invalid_argument("pane filter exceeds limit");
}
}
PaneEntry PaneEntry::from_held(HeldChild::Pointer child, Observation<bool> hidden_state) {
    if (!child) throw std::invalid_argument("null held child");
    PaneEntry result(child->name(), child->metadata(), std::move(hidden_state));
    result.held_ = std::move(child);
    return result;
}
PaneModel::PaneModel(PaneLimits limits) : limits_(limits) {
    if (limits.rows == 0 || limits.rows > 1'000'000 || limits.native_name_bytes == 0 || limits.native_name_bytes > 64 * 1024 * 1024 ||
        limits.display_name_bytes == 0 || limits.display_name_bytes > 128 * 1024 * 1024 || limits.held_entries == 0 || limits.held_entries > 4096)
        throw std::invalid_argument("invalid pane resource limits");
}
PaneListingRequest PaneModel::begin_listing(PaneListingPurpose purpose) {
    if (purpose != PaneListingPurpose::Navigate && purpose != PaneListingPurpose::Refresh) throw std::invalid_argument("invalid pane listing purpose");
    if (next_request_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("pane request handles exhausted");
    pending_ = next_request_++;
    purpose_ = purpose;
    error_.reset();
    return {scope_, pending_};
}
bool PaneModel::current(PaneListingRequest request) const noexcept { return pending_ != 0 && request.number == pending_ && request.scope == scope_; }
PanePublish PaneModel::publish(PaneListingRequest request, const std::vector<PaneEntry>& entries) {
    if (!current(request)) return PanePublish::Stale;
    if (entries.size() > limits_.rows) return PanePublish::LimitExceeded;
    std::size_t native_bytes = 0, display_bytes = 0, held_count = 0;
    auto next_row = next_row_;
    std::vector<PaneRow> prepared;
    prepared.reserve(entries.size());
    const auto less = [](const NativeName& a, const NativeName& b) { return name_compare(a, b) < 0; };
    std::set<NativeName, decltype(less)> names(less);
    std::map<NativeName, std::size_t, decltype(less)> old_by_name(less);
    if (purpose_ == PaneListingPurpose::Refresh)
        for (std::size_t i = 0; i < rows_.size(); ++i)
            if (rows_[i].entry.held_child()) old_by_name.emplace(rows_[i].entry.name, i);
    std::vector<std::optional<std::size_t>> predecessors;
    predecessors.reserve(entries.size());
    std::unordered_map<std::uint64_t, std::size_t> indexes;
    indexes.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.name.encoding() != entries.front().name.encoding())
            return PanePublish::InvalidListing;
        if (entry.name.windows_units() && entry.name.windows_units()->size() > limits_.native_name_bytes / sizeof(char16_t)) return PanePublish::LimitExceeded;
        const auto bytes = entry.name.posix_bytes() ? entry.name.posix_bytes()->size() : entry.name.windows_units()->size() * sizeof(char16_t);
        if (bytes > limits_.native_name_bytes - native_bytes) return PanePublish::LimitExceeded;
        native_bytes += bytes;
        if (!names.insert(entry.name).second) return PanePublish::InvalidListing;
        if (entry.held_child() && (++held_count > limits_.held_entries)) return PanePublish::LimitExceeded;
        if (entry.held_child() && entry.name != entry.held_child()->name()) return PanePublish::InvalidListing;
        auto display = entry.name.display_name();
        if (display.size() > limits_.display_name_bytes - display_bytes) return PanePublish::LimitExceeded;
        display_bytes += display.size();
        std::optional<std::size_t> predecessor;
        const auto old = old_by_name.find(entry.name);
        if (entry.held_child() && old != old_by_name.end() && entry.held_child()->continues(*rows_[old->second].entry.held_child())) predecessor = old->second;
        PaneRowId handle{scope_, 0};
        if (predecessor) handle = rows_[*predecessor].id;
        else {
            if (next_row == std::numeric_limits<std::uint64_t>::max()) return PanePublish::Exhausted;
            handle.number = next_row++;
        }
        auto canonical = entry;
        if (entry.held_child()) canonical.metadata = entry.held_child()->metadata();
        indexes.emplace(handle.number, prepared.size());
        predecessors.push_back(predecessor);
        prepared.push_back({std::move(handle), std::move(canonical), std::move(display)});
    }
    auto projection = project(prepared, view_);
    std::vector<bool> marks(prepared.size(), false);
    std::optional<PaneRowId> focus;
    PaneInvalidatedSelection invalidated;
    if (purpose_ == PaneListingPurpose::Refresh) {
        std::size_t retained_marks = 0;
        for (std::size_t i = 0; i < predecessors.size(); ++i) if (predecessors[i]) {
            const auto old = *predecessors[i];
            marks[i] = marks_[old];
            if (marks[i]) ++retained_marks;
            if (focus_ && rows_[old].id == *focus_) focus = prepared[i].id;
        }
        invalidated.marks = static_cast<std::size_t>(std::count(marks_.begin(), marks_.end(), true)) - retained_marks + invalidated_.marks;
        invalidated.cursor = (focus_.has_value() && !focus.has_value()) || invalidated_.cursor;
    } else if (!projection.visible.empty()) focus = prepared[projection.visible.front()].id;
    // Every allocating step is complete before publication. Old IDs survive
    // only through a sealed live successor; newly allocated IDs never recycle.
    next_row_ = next_row;
    indexes_.swap(indexes);
    rows_.swap(prepared); visible_.swap(projection.visible); positions_.swap(projection.positions); marks_.swap(marks);
    focus_ = std::move(focus); invalidated_ = invalidated; pending_ = 0; error_.reset();
    return PanePublish::Applied;
}
bool PaneModel::fail_listing(PaneListingRequest request, UnavailableObservation failure) {
    if (!current(request)) return false;
    if (failure.state == ObservationState::Available) throw std::invalid_argument("listing failure cannot be Available");
    // Validate arbitrary enum input through the shared observation invariant.
    (void)Observation<int>::unavailable(failure.state, failure.error);
    error_ = std::move(failure); pending_ = 0;
    return true;
}
PaneModel::Projection PaneModel::project(const std::vector<PaneRow>& rows, const PaneView& view) {
    Projection result;
    result.positions.resize(rows.size()); result.visible.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (!view.show_hidden && row.entry.hidden.value() && *row.entry.hidden.value()) continue;
        if (contains(row.display_name, view.display_substring, view.ascii_case_sensitive)) result.visible.push_back(i);
    }
    std::sort(result.visible.begin(), result.visible.end(), [&](std::size_t a, std::size_t b) {
        const auto& left = rows[a]; const auto& right = rows[b];
        if (view.folders_first && directory(left) != directory(right)) return directory(left);
        int order = 0;
        switch (view.sort) {
        case PaneSortKey::Name: order = name_compare(left.entry.name, right.entry.name); if (view.descending) order = -order; break;
        case PaneSortKey::Kind: order = observed_compare(left.entry.metadata.kind, right.entry.metadata.kind, view.descending, std::less<ObjectKind>{}); break;
        case PaneSortKey::Size: order = observed_compare(left.entry.metadata.logical_size, right.entry.metadata.logical_size, view.descending, std::less<std::uint64_t>{}); break;
        case PaneSortKey::Modified: order = observed_compare(left.entry.metadata.modified, right.entry.metadata.modified, view.descending, [](const FileTimestamp& x, const FileTimestamp& y) {
            return x.seconds() < y.seconds() || (x.seconds() == y.seconds() && x.nanoseconds() < y.nanoseconds());
        }); break;
        }
        return order != 0 ? order < 0 : name_compare(left.entry.name, right.entry.name) < 0;
    });
    for (std::size_t i = 0; i < result.visible.size(); ++i) result.positions[result.visible[i]] = i;
    return result;
}
void PaneModel::set_view(PaneView view) {
    validate_view(view);
    auto projection = project(rows_, view);
    view_ = std::move(view); visible_.swap(projection.visible); positions_.swap(projection.positions);
}
std::optional<std::size_t> PaneModel::index(PaneRowId id) const noexcept {
    if (id.scope != scope_) return std::nullopt;
    const auto found = indexes_.find(id.number);
    return found == indexes_.end() ? std::nullopt : std::optional<std::size_t>(found->second);
}
const PaneRow* PaneModel::row(PaneRowId id) const noexcept { const auto i = index(std::move(id)); return i ? &rows_[*i] : nullptr; }
const PaneRow* PaneModel::visible_row(std::size_t i) const noexcept { return i < visible_.size() ? &rows_[visible_[i]] : nullptr; }
std::optional<std::size_t> PaneModel::visible_index(PaneRowId id) const noexcept { const auto i = index(std::move(id)); return i ? positions_[*i] : std::nullopt; }
bool PaneModel::focus(PaneRowId id) noexcept { if (!visible_index(id)) return false; focus_ = std::move(id); invalidated_ = {}; return true; }
bool PaneModel::set_mark(PaneRowId id, bool value) noexcept { const auto i = index(std::move(id)); if (!i || !positions_[*i]) return false; marks_[*i] = value; invalidated_ = {}; return true; }
bool PaneModel::marked(PaneRowId id) const noexcept { const auto i = index(std::move(id)); return i && marks_[*i]; }
void PaneModel::mark_visible(bool value) noexcept { for (auto i : visible_) marks_[i] = value; if (!visible_.empty()) invalidated_ = {}; }
void PaneModel::invert_visible_marks() noexcept { for (auto i : visible_) marks_[i] = !marks_[i]; if (!visible_.empty()) invalidated_ = {}; }
bool PaneModel::mark_range(std::size_t first, std::size_t last, bool value) noexcept {
    if (first >= visible_.size() || last >= visible_.size()) return false;
    if (first > last) std::swap(first, last);
    for (auto i = first; i <= last; ++i) marks_[visible_[i]] = value;
    invalidated_ = {};
    return true;
}
void PaneModel::clear_marks() noexcept { std::fill(marks_.begin(), marks_.end(), false); invalidated_ = {}; }
PaneSources PaneModel::sources() const {
    PaneSources result;
    if (listing_pending()) { result.state = PaneSourcesState::ListingPending; return result; }
    if (invalidated_.marks != 0 || invalidated_.cursor) { result.state = PaneSourcesState::SelectionInvalidated; return result; }
    const auto append = [&](std::size_t i) {
        const auto& row = rows_[i];
        result.candidates.push_back({row.id, row.entry.name, row.entry.metadata.identity});
        if (positions_[i]) ++result.visible_count; else ++result.hidden_count;
    };
    for (std::size_t i = 0; i < rows_.size(); ++i) if (marks_[i]) { result.origin = PaneSourcesOrigin::Marks; append(i); }
    if (result.candidates.empty() && focus_) { const auto i = index(*focus_); if (i && positions_[*i]) append(*i); }
    if (!result.candidates.empty()) result.state = result.hidden_count ? PaneSourcesState::HiddenMarksNeedReview : PaneSourcesState::Ready;
    return result;
}
} // namespace ck::files
