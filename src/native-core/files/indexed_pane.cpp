// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "indexed_pane.hpp"
#include "native_path.hpp"
#include "directory_index_observation.hpp"
#include "pane_page_budget.hpp"
#include <limits>
namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char *op, ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op}};
}
bool same(PaneOrder a, PaneOrder b) noexcept {
    return a.key == b.key && a.descending == b.descending && a.folders_first == b.folders_first;
}
PaneOrder order_of(const PaneView &v) {
    return {v.sort, v.descending, v.folders_first};
}
void validate_view(const PaneView &v) {
    if (!valid_pane_order(order_of(v)) || v.display_substring.size() > 4096)
        throw std::invalid_argument("invalid indexed pane view");
}
PaneView with_order(PaneView view, PaneOrder order) {
    view.sort = order.key;
    view.descending = order.descending;
    view.folders_first = order.folders_first;
    return view;
}
PaneView unfiltered(PaneOrder order) {
    PaneView v;
    v.show_hidden = true;
    return with_order(std::move(v), order);
}
bool same_view(const PaneView &a, const PaneView &b) {
    return same(order_of(a), order_of(b)) && a.show_hidden == b.show_hidden &&
           a.display_substring == b.display_substring && a.ascii_case_sensitive == b.ascii_case_sensitive;
}
void retain_available(BasicMetadata &baseline, const Observation<BasicMetadata> &value) {
    if (!value.value())
        return;
    const auto &m = *value.value();
    if (m.identity.value())
        baseline.identity = m.identity;
    if (m.kind.value())
        baseline.kind = m.kind;
    if (m.modified.value())
        baseline.modified = m.modified;
    if (m.changed.value())
        baseline.changed = m.changed;
}
} // namespace
IndexedPaneDataJob::IndexedPaneDataJob(std::shared_ptr<const CompletedDirectoryIndex> listing, PaneOrder order,
                                       DirectoryIndexSortLimits sort_limits, DirectoryIndexScanLimits lookup_limits,
                                       std::uint64_t lookup_bytes)
    : IndexedPaneDataJob(std::move(listing), unfiltered(order), sort_limits, lookup_limits, lookup_bytes) {
}
IndexedPaneDataJob::IndexedPaneDataJob(Result data, PaneListingRequest target, PaneOrder order,
                                       DirectoryIndexSortLimits sort_limits, DirectoryIndexScanLimits lookup_limits,
                                       std::uint64_t lookup_bytes, std::shared_ptr<DirectoryIndexMarks> marks)
    : IndexedPaneDataJob(data, std::move(target), data ? with_order(data->view_, order) : unfiltered(order),
                         sort_limits, lookup_limits, lookup_bytes, std::move(marks)) {
}
IndexedPaneDataJob::IndexedPaneDataJob(std::shared_ptr<const CompletedDirectoryIndex> listing, const PaneView &view,
                                       DirectoryIndexSortLimits sort_limits, DirectoryIndexScanLimits lookup_limits,
                                       std::uint64_t lookup_bytes)
    : order_(order_of(view)), origin_(IndexedPaneDataOrigin::Listing), lookup_limits_(lookup_limits),
      lookup_bytes_(lookup_bytes) {
    validate_view(view);
    view_ = view;
    if (listing) {
        request_ = listing->request();
        source_scope_ = listing->source_scope();
        directory_ = listing->directory();
        before_ = listing->before();
        previous_after_ = listing->after();
        pin_ = listing->directory_pin();
        store_ = listing->store();
        retain_available(baseline_, before_);
        retain_available(baseline_, previous_after_);
    }
    initialize({PaneSortKey::Name, false, false}, sort_limits);
}
IndexedPaneDataJob::IndexedPaneDataJob(Result data, PaneListingRequest target, const PaneView &view,
                                       DirectoryIndexSortLimits sort_limits, DirectoryIndexScanLimits lookup_limits,
                                       std::uint64_t lookup_bytes, std::shared_ptr<DirectoryIndexMarks> marks)
    : request_(std::move(target)), order_(order_of(view)), origin_(IndexedPaneDataOrigin::Reorder),
      lookup_limits_(lookup_limits), lookup_bytes_(lookup_bytes), marks_(std::move(marks)) {
    validate_view(view);
    view_ = view;
    PaneOrder current = order_;
    if (data && request_.scope == data->request_.scope && request_.number > data->request_.number) {
        predecessor_ = data->revision_;
        source_scope_ = data->source_scope_;
        directory_ = data->directory_;
        before_ = data->before_;
        previous_after_ = data->after_;
        baseline_ = data->baseline_;
        pin_ = data->pin_;
        store_ = data->store_;
        current = data->order_;
    }
    initialize(current, sort_limits);
}
void IndexedPaneDataJob::initialize(PaneOrder current, DirectoryIndexSortLimits limits) {
    if (!valid_pane_order(order_) || !limits.block.rows || limits.block.rows > limits.block.hard_rows ||
        limits.block.wire_bytes < 16 || limits.block.wire_bytes > limits.block.hard_wire_bytes || !limits.merge_rows ||
        limits.merge_rows > DirectoryIndexScanLimits::hard_rows || !lookup_limits_.rows ||
        lookup_limits_.rows > lookup_limits_.hard_rows || lookup_limits_.wire_bytes < 16 ||
        lookup_limits_.wire_bytes > lookup_limits_.hard_wire_bytes || !lookup_bytes_ ||
        lookup_bytes_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("invalid indexed pane preparation limits/order");
    if (!store_ || !pin_ || !request_.scope.is_valid() || !request_.number || !source_scope_.is_valid() ||
        store_->state() != DirectoryIndexStoreState::Sealed) {
        fail(error(AdapterError::InvalidAccess, "indexed_pane.source"));
        return;
    }
    if (same(current, order_))
        state_ = IndexedPaneDataJobState::Indexing;
    else
        sort_ = std::make_unique<DirectoryIndexSortJob>(store_, order_, limits);
}
void IndexedPaneDataJob::release() noexcept {
    recount_.reset();
    marks_.reset();
    sort_.reset();
    lookup_.reset();
    store_.reset();
    pin_.reset();
}
void IndexedPaneDataJob::fail(UnavailableObservation detail, bool stopped) {
    state_ = stopped && detail.error.domain == ErrorDomain::Adapter &&
                     detail.error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead)
                 ? IndexedPaneDataJobState::Cancelled
                 : IndexedPaneDataJobState::Failed;
    failure_ = std::move(detail);
    release();
}
void IndexedPaneDataJob::cancel() noexcept {
    state_ = IndexedPaneDataJobState::Cancelled;
    release();
    try {
        failure_ = error(AdapterError::InterruptedRead, "indexed_pane.cancelled", ObservationState::Unknown);
    } catch (...) {
        failure_.reset();
    }
}
bool IndexedPaneDataJob::abandon() noexcept {
    if (state_ == IndexedPaneDataJobState::Failed || state_ == IndexedPaneDataJobState::Cancelled ||
        state_ == IndexedPaneDataJobState::Abandoned)
        return false;
    release();
    result_.reset();
    state_ = IndexedPaneDataJobState::Abandoned;
    return true;
}
IndexedPaneDataJobState IndexedPaneDataJob::advance(std::stop_token stop) {
    if (state_ != IndexedPaneDataJobState::Sorting && state_ != IndexedPaneDataJobState::Indexing &&
        state_ != IndexedPaneDataJobState::CountingMarks)
        return state_;
    try {
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        if (store_->state() != DirectoryIndexStoreState::Sealed) {
            fail(store_->failure().value_or(error(AdapterError::InvalidAccess, "indexed_pane.source_state")));
            return state_;
        }
        if (state_ == IndexedPaneDataJobState::Sorting) {
            const auto state = sort_->advance(stop);
            if (state == DirectoryIndexSortState::Complete) {
                store_ = *sort_->completed().value();
                sort_.reset();
                state_ = IndexedPaneDataJobState::Indexing;
            } else if (state == DirectoryIndexSortState::Cancelled)
                cancel();
            else if (state != DirectoryIndexSortState::Running) {
                auto result = sort_->completed();
                fail(*result.unavailable_detail());
            }
            return state_;
        }
        if (state_ == IndexedPaneDataJobState::Indexing && !lookup_)
            lookup_ = std::make_unique<DirectoryIndexLookupJob>(
                store_, lookup_limits_, lookup_bytes_, stop,
                PaneFilter{view_.show_hidden, view_.display_substring, view_.ascii_case_sensitive});
        else if (state_ == IndexedPaneDataJobState::Indexing)
            lookup_->advance(stop);
        const auto state = lookup_->state();
        if (state == DirectoryIndexLookupJobState::Cancelled) {
            cancel();
            return state_;
        }
        if (state == DirectoryIndexLookupJobState::Failed) {
            fail(*lookup_->completed().unavailable_detail(), stop.stop_requested());
            return state_;
        }
        if (state != DirectoryIndexLookupJobState::Complete)
            return state_;
        auto lookup = *lookup_->completed().value();
        if (marks_) {
            if (!recount_) {
                recount_ = std::make_unique<DirectoryMarksRecountJob>(marks_, lookup, lookup_limits_.rows);
                state_ = IndexedPaneDataJobState::CountingMarks;
            }
            const auto counted = recount_->advance(stop);
            if (counted == MarksRecountState::Cancelled) {
                cancel();
                return state_;
            }
            if (counted == MarksRecountState::Failed) {
                fail(*recount_->completed().unavailable_detail(), stop.stop_requested());
                return state_;
            }
            if (counted != MarksRecountState::Complete)
                return state_;
            counted_visible_ = recount_->completed().value()->visible;
        }
        auto scope = pin_->source_scope();
        if (!scope.value()) {
            fail(*scope.unavailable_detail());
            return state_;
        }
        if (*scope.value() != source_scope_) {
            fail(error(AdapterError::InvalidIdentifier, "indexed_pane.scope_changed"));
            return state_;
        }
        auto after = pin_->inspect_basic();
        auto checked = detail::check_index_directory(source_scope_, directory_,
                                                     Observation<BasicMetadata>::available(baseline_), after);
        if (!checked.value()) {
            fail(*checked.unavailable_detail());
            return state_;
        }
        retain_available(baseline_, after);
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        result_ = Result(new const CompletedIndexedPaneData(
            request_, order_, view_, origin_, source_scope_, predecessor_, directory_, before_, std::move(after),
            baseline_, pin_, store_, std::move(lookup),
            marks_ ? std::optional<AdapterScope>(marks_->revision()) : std::nullopt, counted_visible_));
        state_ = IndexedPaneDataJobState::Complete;
        release();
        return state_;
    } catch (...) {
        state_ = IndexedPaneDataJobState::Failed;
        failure_.reset();
        release();
        throw;
    }
}
Observation<IndexedPaneDataJob::Result> IndexedPaneDataJob::completed() const {
    if (state_ == IndexedPaneDataJobState::Complete && result_)
        return Observation<Result>::available(result_);
    if (failure_)
        return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
IndexedPane::~IndexedPane() {
    invalidate();
    mark_pending_ = false;
}
void IndexedPane::invalidate() noexcept {
    if (stamp_)
        stamp_->current.store(false, std::memory_order_release);
    stamp_.reset();
}
PaneStateStamp IndexedPane::state_stamp() const {
    if (!stamp_ || !stamp_->current.load(std::memory_order_acquire))
        stamp_ = std::make_shared<PaneStateStamp::Token>();
    PaneStateStamp result;
    result.token_ = stamp_;
    return result;
}
bool IndexedPane::accepts(const PaneStateStamp &stamp) const noexcept {
    return stamp_ && stamp.token_ == stamp_ && stamp.current();
}
bool IndexedPane::current(PaneListingRequest request) const noexcept {
    return pending_ && request.scope == scope_ && request.number == pending_;
}
PaneListingRequest IndexedPane::begin(IndexedPaneDataOrigin kind, const PaneView &view) {
    validate_view(view);
    auto prepared_view = view;
    if (!next_request_)
        throw std::overflow_error("indexed pane requests exhausted");
    invalidate();
    pending_ = next_request_++;
    pending_kind_ = kind;
    pending_order_ = order_of(view);
    pending_view_ = std::move(prepared_view);
    mark_pending_ = false;
    error_.reset();
    if (kind == IndexedPaneDataOrigin::Reorder) {
        pending_dataset_ = data_->row_scope();
        pending_revision_ = data_->revision_;
    } else {
        pending_dataset_.reset();
        pending_revision_.reset();
    }
    return {scope_, pending_};
}
PaneListingRequest IndexedPane::begin_listing(PaneOrder order) {
    return begin_listing(unfiltered(order));
}
PaneListingRequest IndexedPane::begin_listing(const PaneView &view) {
    return begin(IndexedPaneDataOrigin::Listing, view);
}
Observation<PaneListingRequest> IndexedPane::begin_reorder(const PaneStateStamp &stamp, PaneOrder order) {
    return begin_view_change(stamp, data_ ? with_order(data_->view_, order) : unfiltered(order));
}
Observation<PaneListingRequest> IndexedPane::begin_view_change(const PaneStateStamp &stamp, const PaneView &view) {
    if (!accepts(stamp) || !data_ || (pending_ && pending_kind_ == IndexedPaneDataOrigin::Listing)) {
        const auto e = error(AdapterError::InvalidAccess, "indexed_pane.reorder_state");
        return Observation<PaneListingRequest>::unavailable(e.state, e.error);
    }
    return Observation<PaneListingRequest>::available(begin(IndexedPaneDataOrigin::Reorder, view));
}
bool IndexedPane::fail_listing(PaneListingRequest request, UnavailableObservation failure) {
    if (!current(request))
        return false;
    (void)Observation<int>::unavailable(failure.state, failure.error);
    invalidate();
    error_ = std::move(failure);
    pending_ = 0;
    pending_dataset_.reset();
    pending_revision_.reset();
    return true;
}
bool IndexedPane::cancel_listing(PaneListingRequest request) noexcept {
    if (!current(request))
        return false;
    invalidate();
    pending_ = 0;
    pending_dataset_.reset();
    pending_revision_.reset();
    error_.reset();
    return true;
}
void IndexedPane::io_failure(UnavailableObservation detail) {
    invalidate();
    error_ = std::move(detail);
}
IndexedPanePublish IndexedPane::publish(Data data, std::stop_token stop) {
    if (!data)
        return IndexedPanePublish::InvalidData;
    if (!current(data->request_))
        return IndexedPanePublish::Stale;
    if (data->origin_ != pending_kind_ || !same_view(data->view_, pending_view_) ||
        (pending_dataset_ && *pending_dataset_ != data->row_scope()) || data->predecessor_ != pending_revision_)
        return IndexedPanePublish::InvalidData;
    if (pending_kind_ == IndexedPaneDataOrigin::Reorder &&
        data->marks_revision_ != (marks_ ? std::optional<AdapterScope>(marks_->revision()) : std::nullopt))
        return IndexedPanePublish::InvalidData;
    if (stop.stop_requested())
        return IndexedPanePublish::Cancelled;
    if (data->lookup_->state() != DirectoryIndexLookupState::Ready ||
        data->store_->state() != DirectoryIndexStoreState::Sealed) {
        fail_listing(data->request_, data->lookup_->failure().value_or(data->store_->failure().value_or(
                                         error(AdapterError::InvalidAccess, "indexed_pane.data_state"))));
        return IndexedPanePublish::Failed;
    }
    PaneRowId next_focus{data->row_scope(), 0};
    bool have_focus = false;
    std::uint64_t next_position = 0;
    bool have_position = false;
    if (pending_kind_ == IndexedPaneDataOrigin::Listing) {
        if (data->visible_rows()) {
            auto first = [&] {
                try {
                    return data->lookup_->read_page(0, {1, DirectoryIndexScanLimits::hard_wire_bytes});
                } catch (...) {
                    if (current(data->request_))
                        invalidate();
                    throw;
                }
            }();
            if (!current(data->request_))
                return IndexedPanePublish::Stale;
            if (!first.value()) {
                fail_listing(data->request_, *first.unavailable_detail());
                return IndexedPanePublish::Failed;
            }
            if (stop.stop_requested())
                return IndexedPanePublish::Cancelled;
            next_focus.number = first.value()->rows.front().record.row_id;
            have_focus = true;
            have_position = true;
        }
    }
    if (pending_kind_ == IndexedPaneDataOrigin::Reorder && focus_) {
        next_focus = *focus_;
        have_focus = true;
        auto position = [&] {
            try {
                return data->lookup_->locate_row({focus_->scope, focus_->number});
            } catch (...) {
                if (current(data->request_))
                    invalidate();
                throw;
            }
        }();
        if (!current(data->request_))
            return IndexedPanePublish::Stale;
        if (!position.value()) {
            fail_listing(data->request_, *position.unavailable_detail());
            return IndexedPanePublish::Failed;
        }
        if (stop.stop_requested())
            return IndexedPanePublish::Cancelled;
        if (*position.value()) {
            next_position = **position.value();
            have_position = true;
        }
    }
    if (!current(data->request_))
        return IndexedPanePublish::Stale;
    invalidate();
    if (pending_kind_ == IndexedPaneDataOrigin::Listing) {
        marks_.reset();
        mark_error_.reset();
    }
    visible_marks_ = data->visible_marks_;
    data_ = std::move(data);
    if (have_focus)
        focus_.emplace(std::move(next_focus));
    else
        focus_.reset();
    if (have_position)
        focus_position_.emplace(next_position);
    else
        focus_position_.reset();
    pending_ = 0;
    pending_dataset_.reset();
    pending_revision_.reset();
    error_.reset();
    return IndexedPanePublish::Applied;
}
IndexedPaneCapture IndexedPane::capture_page(const PaneStateStamp &stamp, std::uint64_t first,
                                             IndexedPanePageLimits limits, std::stop_token stop) {
    if (!limits.display.rows || limits.display.rows > 4096 || !limits.display.text_bytes ||
        limits.display.text_bytes > 16 * 1024 * 1024 || limits.wire_bytes < 16 ||
        limits.wire_bytes > DirectoryIndexScanLimits::hard_wire_bytes)
        throw std::invalid_argument("invalid indexed pane page limits");
    if (!accepts(stamp))
        return {IndexedPaneCaptureState::Stale, {}};
    if (first > visible_rows())
        return {IndexedPaneCaptureState::OutOfRange, {}};
    if (stop.stop_requested())
        return {IndexedPaneCaptureState::Cancelled, {}};
    std::size_t used = 0;
    const auto &view = data_ ? data_->view_ : pending_view_;
    if (!detail::charge(view.display_substring.size(), used, limits.display.text_bytes) ||
        !detail::charge(mark_error_, used, limits.display.text_bytes) ||
        !detail::charge(error_, used, limits.display.text_bytes))
        return {IndexedPaneCaptureState::LimitExceeded, {}};
    auto page = std::shared_ptr<IndexedPanePage>(new IndexedPanePage);
    page->first_ = first;
    page->total_ = rows();
    page->visible_ = visible_rows();
    page->view_ = view;
    page->focused_position_ = focus_position_;
    page->pending_ = listing_pending();
    page->marking_pending_ = mark_pending_;
    page->selection_ = selection();
    page->mark_error_ = mark_error_;
    page->error_ = error_;
    const auto data = data_; // retain the exact dataset through this bounded request
    page->order_ = data ? data->order_ : pending_order_;
    if (data) {
        auto read = [&] {
            try {
                return data->lookup_->read_page(first, {limits.display.rows, limits.wire_bytes});
            } catch (...) {
                if (accepts(stamp))
                    invalidate();
                throw;
            }
        }();
        if (!accepts(stamp))
            return {IndexedPaneCaptureState::Stale, {}};
        if (!read.value()) {
            if (data->lookup_->state() == DirectoryIndexLookupState::Ready &&
                data->store_->state() == DirectoryIndexStoreState::Sealed)
                return {IndexedPaneCaptureState::LimitExceeded, {}};
            io_failure(*read.unavailable_detail());
            return {IndexedPaneCaptureState::Failed, {}};
        }
        if (stop.stop_requested())
            return {IndexedPaneCaptureState::Cancelled, {}};
        auto raw = std::move(read).take_value();
        page->next_ = raw.next.position();
        page->order_ = data->order_;
        page->rows_.reserve(raw.rows.size());
        for (auto &row : raw.rows) {
            if (stop.stop_requested())
                return {IndexedPaneCaptureState::Cancelled, {}};
            auto name = row.record.name.display_name();
            BasicMetadata metadata;
            std::optional<UnavailableObservation> inspected;
            if (row.record.metadata.value())
                metadata = std::move(row.record.metadata).take_value();
            else
                inspected = *row.record.metadata.unavailable_detail();
            if (!detail::charge(name.size(), used, limits.display.text_bytes) ||
                !detail::charge(metadata, used, limits.display.text_bytes) ||
                !detail::charge(row.record.hidden, used, limits.display.text_bytes) ||
                !detail::charge(inspected, used, limits.display.text_bytes))
                return {IndexedPaneCaptureState::LimitExceeded, {}};
            PaneRowId id{data->row_scope(), row.record.row_id};
            bool marked = false;
            if (marks_) {
                const auto marks = marks_;
                auto checked = [&] {
                    try {
                        return marks->contains({id.scope, id.number});
                    } catch (...) {
                        if (accepts(stamp))
                            invalidate();
                        throw;
                    }
                }();
                if (!accepts(stamp))
                    return {IndexedPaneCaptureState::Stale, {}};
                if (!checked.value()) {
                    auto detail = *checked.unavailable_detail();
                    invalidate();
                    mark_error_ = std::move(detail);
                    return {IndexedPaneCaptureState::Failed, {}};
                }
                marked = *checked.value();
                if (stop.stop_requested())
                    return {IndexedPaneCaptureState::Cancelled, {}};
            }
            page->rows_.push_back({id, std::move(name), std::move(metadata), std::move(row.record.hidden),
                                   std::move(inspected), marked, focus_ && *focus_ == id});
        }
    }
    if (!accepts(stamp))
        return {IndexedPaneCaptureState::Stale, {}};
    page->text_bytes_ = used;
    page->stamp_ = stamp;
    return {IndexedPaneCaptureState::Ready, std::move(page)};
}
Observation<OpenedIndexedDirectory> IndexedPane::open_directory_row(const PaneStateStamp &stamp, PaneRowId row,
                                                                    IndexedDirectoryOpenLimits limits,
                                                                    std::stop_token stop) {
    using Result = Observation<OpenedIndexedDirectory>;
    const auto reject = [](const UnavailableObservation &e) { return Result::unavailable(e.state, e.error); };
    if (limits.wire_bytes < 16 || limits.wire_bytes > DirectoryIndexScanLimits::hard_wire_bytes ||
        !limits.native_name_bytes || limits.native_name_bytes > NativePathLimits::hard_native_bytes ||
        !limits.diagnostic_bytes || limits.diagnostic_bytes > 16 * 1024 * 1024)
        throw std::invalid_argument("invalid indexed directory open limits");
    if (!accepts(stamp) || !data_ || pending_ || mark_pending_)
        return reject(error(AdapterError::InvalidAccess, "indexed_directory.state"));
    const auto data = data_;
    if (row.scope != data->row_scope() || !row.number || row.number > data->rows())
        return reject(error(AdapterError::InvalidIdentifier, "indexed_directory.row"));
    const auto boundary = [&](bool check_stop = true) -> std::optional<UnavailableObservation> {
        if (!accepts(stamp))
            return error(AdapterError::InvalidAccess, "indexed_directory.stale", ObservationState::Changed);
        if (data->lookup_->state() != DirectoryIndexLookupState::Ready ||
            data->store_->state() != DirectoryIndexStoreState::Sealed) {
            auto e = data->lookup_->failure().value_or(
                data->store_->failure().value_or(error(AdapterError::InvalidAccess, "indexed_directory.dataset")));
            io_failure(e);
            return e;
        }
        if (check_stop && stop.stop_requested())
            return error(AdapterError::InterruptedRead, "indexed_directory.cancelled", ObservationState::Unknown);
        return {};
    };
    if (auto e = boundary())
        return reject(*e);
    try {
        auto found = data->lookup_->locate_row({row.scope, row.number});
        if (auto e = boundary(false))
            return reject(*e);
        if (!found.value())
            return reject(*found.unavailable_detail());
        if (!*found.value())
            return reject(error(AdapterError::InvalidIdentifier, "indexed_directory.excluded_row"));
        if (auto e = boundary())
            return reject(*e);
        auto read = data->lookup_->read_page(**found.value(), {1, limits.wire_bytes});
        if (auto e = boundary(false))
            return reject(*e);
        if (!read.value())
            return reject(*read.unavailable_detail());
        if (auto e = boundary())
            return reject(*e);
        if (read.value()->rows.size() != 1 || read.value()->rows.front().record.row_id != row.number) {
            auto e = error(AdapterError::InvalidNativeResult, "indexed_directory.record");
            data->lookup_->fail(e);
            io_failure(e);
            return reject(e);
        }
        auto raw = std::move(read).take_value();
        auto record = std::move(raw.rows.front().record);
        const auto units =
            record.name.posix_bytes() ? record.name.posix_bytes()->size() : record.name.windows_units()->size();
        const auto width = record.name.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
        if (units > limits.native_name_bytes / width)
            return reject(error(AdapterError::InventoryLimit, "indexed_directory.name_bytes"));
        if (!record.metadata.value())
            return reject(*record.metadata.unavailable_detail());
        std::size_t diagnostics = 0;
        const auto qualify = [&](const BasicMetadata &m) -> std::optional<UnavailableObservation> {
            if (!detail::charge(m, diagnostics, limits.diagnostic_bytes))
                return error(AdapterError::InventoryLimit, "indexed_directory.diagnostics");
            if (!m.kind.value())
                return *m.kind.unavailable_detail();
            if (*m.kind.value() != ObjectKind::Directory)
                return error(AdapterError::NotDirectory, "indexed_directory.kind");
            if (!m.identity.value())
                return *m.identity.unavailable_detail();
            const auto &id = *m.identity.value();
            if (id.scope != data->source_scope_ || compare_identity(id, id) != IdentityComparison::Same)
                return error(AdapterError::UnqualifiedIdentity, "indexed_directory.identity",
                             ObservationState::Unknown);
            return {};
        };
        const auto compare = [&](const Observation<ObjectIdentity> &a,
                                 const Observation<ObjectIdentity> &b) -> std::optional<UnavailableObservation> {
            for (const auto *id : {&a, &b})
                if (!id->value())
                    return *id->unavailable_detail();
            const auto match = compare_identity(*a.value(), *b.value());
            if (match != IdentityComparison::Same)
                return error(AdapterError::UnqualifiedIdentity, "indexed_directory.identity_changed",
                             match == IdentityComparison::Different ? ObservationState::Changed
                                                                    : ObservationState::Unknown);
            return {};
        };
        const auto inspect_parent = [&]() -> Observation<BasicMetadata> {
            auto scope = data->pin_->source_scope();
            if (!scope.value()) {
                const auto &e = *scope.unavailable_detail();
                return Observation<BasicMetadata>::unavailable(e.state, e.error);
            }
            if (*scope.value() != data->source_scope_) {
                const auto e = error(AdapterError::InvalidIdentifier, "indexed_directory.parent_scope");
                return Observation<BasicMetadata>::unavailable(e.state, e.error);
            }
            if (auto e = boundary())
                return Observation<BasicMetadata>::unavailable(e->state, e->error);
            auto observed = data->pin_->inspect_basic();
            if (!observed.value())
                return observed;
            if (auto e = qualify(*observed.value()))
                return Observation<BasicMetadata>::unavailable(e->state, e->error);
            if (auto e = compare(data->directory_, observed.value()->identity))
                return Observation<BasicMetadata>::unavailable(e->state, e->error);
            auto checked =
                detail::check_index_directory(data->source_scope_, data->directory_,
                                              Observation<BasicMetadata>::available(data->baseline_), observed);
            if (!checked.value()) {
                const auto &e = *checked.unavailable_detail();
                return Observation<BasicMetadata>::unavailable(e.state, e.error);
            }
            return observed;
        };
        if (auto e = qualify(*record.metadata.value()))
            return reject(*e);
        if (auto e = boundary())
            return reject(*e);
        auto before = inspect_parent();
        if (!before.value())
            return reject(*before.unavailable_detail());
        if (auto e = boundary())
            return reject(*e);
        // Retain a fresh metadata-only child while opening the enumeration
        // cursor. This prevents identity recycling between these live checks.
        auto held = HeldChild::observe(data->pin_, record.name);
        if (!held.value())
            return reject(*held.unavailable_detail());
        if (auto e = qualify((*held.value())->metadata()))
            return reject(*e);
        if (auto e = compare(record.metadata.value()->identity, (*held.value())->metadata().identity))
            return reject(*e);
        if (auto e = boundary())
            return reject(*e);
        auto opened = data->pin_->open_directory_child(record.name);
        if (!opened.value())
            return reject(*opened.unavailable_detail());
        auto cursor = std::move(opened).take_value();
        if (!cursor)
            return reject(error(AdapterError::InvalidNativeResult, "indexed_directory.null_cursor"));
        if (auto e = boundary())
            return reject(*e);
        auto child_scope = cursor->source_scope();
        if (!child_scope.value())
            return reject(*child_scope.unavailable_detail());
        if (*child_scope.value() != data->source_scope_)
            return reject(error(AdapterError::InvalidIdentifier, "indexed_directory.child_scope"));
        if (auto e = boundary())
            return reject(*e);
        auto metadata = cursor->inspect_basic();
        if (!metadata.value())
            return reject(*metadata.unavailable_detail());
        if (auto e = qualify(*metadata.value()))
            return reject(*e);
        if (auto e = compare(cursor->identity(), metadata.value()->identity))
            return reject(*e);
        if (auto e = compare((*held.value())->metadata().identity, metadata.value()->identity))
            return reject(*e);
        if (auto e = boundary())
            return reject(*e);
        auto retained = (*held.value())->inspect_basic();
        if (!retained.value())
            return reject(*retained.unavailable_detail());
        if (auto e = qualify(*retained.value()))
            return reject(*e);
        if (auto e = compare(retained.value()->identity, metadata.value()->identity))
            return reject(*e);
        if (auto e = boundary())
            return reject(*e);
        auto after = inspect_parent();
        if (!after.value())
            return reject(*after.unavailable_detail());
        auto checked = detail::check_index_directory(data->source_scope_, before.value()->identity, before, after);
        if (!checked.value())
            return reject(*checked.unavailable_detail());
        if (auto e = boundary())
            return reject(*e);
        return Result::available({std::move(record.name), std::move(cursor)});
    } catch (...) {
        if (accepts(stamp) && (data->lookup_->state() != DirectoryIndexLookupState::Ready ||
                               data->store_->state() != DirectoryIndexStoreState::Sealed))
            invalidate();
        throw;
    }
}
IndexedPaneFocus IndexedPane::focus(const PaneStateStamp &stamp, PaneRowId row, std::stop_token stop) {
    if (!accepts(stamp))
        return IndexedPaneFocus::Stale;
    if (pending_ || mark_pending_ || !data_)
        return IndexedPaneFocus::Unavailable;
    if (row.scope != data_->row_scope() || !row.number || row.number > data_->rows())
        return IndexedPaneFocus::InvalidTarget;
    if (stop.stop_requested())
        return IndexedPaneFocus::Cancelled;
    const auto data = data_;
    auto found = [&] {
        try {
            return data->lookup_->locate_row({row.scope, row.number});
        } catch (...) {
            if (accepts(stamp))
                invalidate();
            throw;
        }
    }();
    if (!accepts(stamp))
        return IndexedPaneFocus::Stale;
    if (!found.value()) {
        io_failure(*found.unavailable_detail());
        return IndexedPaneFocus::Failed;
    }
    if (!*found.value())
        return IndexedPaneFocus::InvalidTarget;
    if (stop.stop_requested())
        return IndexedPaneFocus::Cancelled;
    if (!accepts(stamp))
        return IndexedPaneFocus::Stale;
    if (focus_ && *focus_ == row)
        return IndexedPaneFocus::Applied;
    invalidate();
    focus_ = std::move(row);
    focus_position_ = **found.value();
    return IndexedPaneFocus::Applied;
}
Observation<std::unique_ptr<IndexedPaneMarkJob>>
IndexedPane::begin_mark_change(const PaneStateStamp &stamp, const IndexedMarkCommand &command,
                               std::size_t rows_per_step, std::uint64_t max_bytes, std::stop_token stop) {
    using Result = Observation<std::unique_ptr<IndexedPaneMarkJob>>;
    const auto rejected = [](const UnavailableObservation &e) { return Result::unavailable(e.state, e.error); };
    if (!accepts(stamp) || !data_ || pending_ || mark_pending_)
        return rejected(error(AdapterError::InvalidAccess, "indexed_pane.mark_state"));
    if (!rows_per_step || rows_per_step > 4096 || !max_bytes ||
        max_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("invalid mark limits");
    if (stop.stop_requested())
        return rejected(error(AdapterError::InterruptedRead, "indexed_pane.mark_cancelled", ObservationState::Unknown));
    const bool clear = std::holds_alternative<IndexedClearMarks>(command);
    if (!clear && (data_->lookup_->state() != DirectoryIndexLookupState::Ready ||
                   data_->store_->state() != DirectoryIndexStoreState::Sealed))
        return rejected(data_->lookup_->failure().value_or(
            data_->store_->failure().value_or(error(AdapterError::InvalidAccess, "indexed_pane.mark_data"))));
    auto token = std::make_shared<PaneStateStamp::Token>();
    PaneStateStamp next;
    next.token_ = token;
    const auto data = data_;
    const auto marks = marks_;
    auto prepared = [&] {
        try {
            return std::unique_ptr<IndexedPaneMarkJob>(
                new IndexedPaneMarkJob(data, marks, visible_marks_, next, command, rows_per_step, max_bytes));
        } catch (...) {
            if (accepts(stamp))
                invalidate();
            throw;
        }
    }();
    if (!accepts(stamp))
        return rejected(error(AdapterError::InvalidAccess, "indexed_pane.mark_stale"));
    if (prepared->state() != IndexedMarkJobState::Running) {
        if (data->lookup_->state() != DirectoryIndexLookupState::Ready ||
            (marks && marks->state() != DirectoryIndexMarksState::Ready))
            invalidate();
        return rejected(*prepared->failure());
    }
    if (stop.stop_requested())
        return rejected(error(AdapterError::InterruptedRead, "indexed_pane.mark_cancelled", ObservationState::Unknown));
    invalidate();
    stamp_ = std::move(token);
    mark_pending_ = true;
    mark_error_.reset();
    return Result::available(std::move(prepared));
}
IndexedPanePublish IndexedPane::publish_marks(IndexedPaneMarkJob::Result result) {
    if (!result)
        return IndexedPanePublish::InvalidData;
    if (!accepts(result->stamp_) || !mark_pending_)
        return IndexedPanePublish::Stale;
    if (!data_ || result->dataset_ != data_->revision_ ||
        result->base_ != (marks_ ? std::optional<AdapterScope>(marks_->revision()) : std::nullopt))
        return IndexedPanePublish::InvalidData;
    const auto &target = result->target_;
    const auto &counts = result->counts_;
    if ((target && (target->state() != DirectoryIndexMarksState::Ready || target->row_scope() != data_->row_scope() ||
                    target->rows() != data_->rows() || target->marked() != counts.total)) ||
        (!target && counts.total) || counts.visible > counts.total || counts.visible > data_->visible_rows() ||
        counts.hidden != counts.total - counts.visible)
        return IndexedPanePublish::InvalidData;
    invalidate();
    marks_ = target;
    visible_marks_ = counts.visible;
    mark_pending_ = false;
    mark_error_.reset();
    return IndexedPanePublish::Applied;
}
bool IndexedPane::fail_marks(const PaneStateStamp &stamp, UnavailableObservation failure) {
    if (!accepts(stamp) || !mark_pending_)
        return false;
    (void)Observation<int>::unavailable(failure.state, failure.error);
    invalidate();
    mark_pending_ = false;
    mark_error_ = std::move(failure);
    return true;
}
bool IndexedPane::cancel_marks(const PaneStateStamp &stamp) noexcept {
    if (!accepts(stamp) || !mark_pending_)
        return false;
    invalidate();
    mark_pending_ = false;
    mark_error_.reset();
    return true;
}
IndexedPaneSelection IndexedPane::selection() const noexcept {
    IndexedPaneSelection result;
    if ((data_ && (data_->lookup_->state() != DirectoryIndexLookupState::Ready ||
                   data_->store_->state() != DirectoryIndexStoreState::Sealed)) ||
        (marks_ && marks_->state() != DirectoryIndexMarksState::Ready)) {
        result.state = PaneSourcesState::SelectionInvalidated;
        return result;
    }
    const auto total = marks_ ? marks_->marked() : 0;
    result.marks = IndexedPaneMarkCounts{total, visible_marks_, total - visible_marks_};
    if (total)
        result.origin = PaneSourcesOrigin::Marks;
    if (pending_ || mark_pending_)
        result.state = PaneSourcesState::ListingPending;
    else if (total) {
        result.state = total > visible_marks_ ? PaneSourcesState::HiddenMarksNeedReview : PaneSourcesState::Ready;
        if (result.state == PaneSourcesState::Ready)
            result.candidates = total;
    } else if (focus_ && focus_position_) {
        result.state = PaneSourcesState::Ready;
        result.candidates = 1;
    }
    return result;
}
Observation<std::unique_ptr<IndexedPaneSourceJob>> IndexedPane::begin_source_acquisition(const PaneStateStamp &stamp,
                                                                                         IndexedSourceLimits limits) {
    using Result = Observation<std::unique_ptr<IndexedPaneSourceJob>>;
    const auto selected = selection();
    if (!accepts(stamp) || selected.state != PaneSourcesState::Ready || !selected.candidates) {
        const auto e = error(AdapterError::InvalidAccess, "indexed_sources.selection");
        return Result::unavailable(e.state, e.error);
    }
    auto job = std::unique_ptr<IndexedPaneSourceJob>(
        new IndexedPaneSourceJob(data_, marks_, focus_, stamp, selected.origin, selected.candidates, limits));
    if (job->state() != IndexedSourceJobState::Running) {
        const auto &e = *job->failure();
        return Result::unavailable(e.state, e.error);
    }
    return Result::available(std::move(job));
}
bool IndexedPane::accepts_sources(const CompletedIndexedSources &sources) const noexcept {
    const auto selected = selection();
    return accepts(sources.stamp_) && data_ && sources.dataset_ == data_->revision_ &&
           sources.marks_ == (marks_ ? std::optional<AdapterScope>(marks_->revision()) : std::nullopt) &&
           selected.state == PaneSourcesState::Ready && selected.origin == sources.origin_ &&
           selected.candidates == sources.sources_.size();
}
} // namespace ck::files
