// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "indexed_pane.hpp"
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
    : order_(order), origin_(IndexedPaneDataOrigin::Listing), lookup_limits_(lookup_limits),
      lookup_bytes_(lookup_bytes) {
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
IndexedPaneDataJob::IndexedPaneDataJob(Result data, PaneListingRequest target, PaneOrder order,
                                       DirectoryIndexSortLimits sort_limits, DirectoryIndexScanLimits lookup_limits,
                                       std::uint64_t lookup_bytes)
    : request_(std::move(target)), order_(order), origin_(IndexedPaneDataOrigin::Reorder),
      lookup_limits_(lookup_limits), lookup_bytes_(lookup_bytes) {
    PaneOrder current = order;
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
    if (state_ != IndexedPaneDataJobState::Sorting && state_ != IndexedPaneDataJobState::Indexing)
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
        if (!lookup_)
            lookup_ = std::make_unique<DirectoryIndexLookupJob>(store_, lookup_limits_, lookup_bytes_, stop);
        else
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
        result_ = Result(new const CompletedIndexedPaneData(request_, order_, origin_, source_scope_, predecessor_,
                                                            directory_, before_, std::move(after), baseline_, pin_,
                                                            store_, std::move(lookup)));
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
}
void IndexedPane::invalidate() noexcept {
    if (stamp_)
        stamp_->current.store(false, std::memory_order_release);
    stamp_.reset();
}
PaneStateStamp IndexedPane::state_stamp() const {
    if (!stamp_)
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
PaneListingRequest IndexedPane::begin(IndexedPaneDataOrigin kind, PaneOrder order) {
    if (!valid_pane_order(order))
        throw std::invalid_argument("invalid indexed pane order");
    if (!next_request_)
        throw std::overflow_error("indexed pane requests exhausted");
    invalidate();
    pending_ = next_request_++;
    pending_kind_ = kind;
    pending_order_ = order;
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
    return begin(IndexedPaneDataOrigin::Listing, order);
}
Observation<PaneListingRequest> IndexedPane::begin_reorder(const PaneStateStamp &stamp, PaneOrder order) {
    if (!accepts(stamp) || !data_ || (pending_ && pending_kind_ == IndexedPaneDataOrigin::Listing)) {
        const auto e = error(AdapterError::InvalidAccess, "indexed_pane.reorder_state");
        return Observation<PaneListingRequest>::unavailable(e.state, e.error);
    }
    return Observation<PaneListingRequest>::available(begin(IndexedPaneDataOrigin::Reorder, order));
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
    if (data->origin_ != pending_kind_ || !same(data->order_, pending_order_) ||
        (pending_dataset_ && *pending_dataset_ != data->row_scope()) || data->predecessor_ != pending_revision_)
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
    if (pending_kind_ == IndexedPaneDataOrigin::Listing) {
        if (data->rows()) {
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
        }
    }
    if (pending_kind_ == IndexedPaneDataOrigin::Reorder && focus_) {
        next_focus = *focus_;
        have_focus = true;
    }
    if (!current(data->request_))
        return IndexedPanePublish::Stale;
    invalidate();
    data_ = std::move(data);
    if (have_focus)
        focus_.emplace(std::move(next_focus));
    else
        focus_.reset();
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
    if (first > rows())
        return {IndexedPaneCaptureState::OutOfRange, {}};
    if (stop.stop_requested())
        return {IndexedPaneCaptureState::Cancelled, {}};
    std::size_t used = 0;
    if (!detail::charge(error_, used, limits.display.text_bytes))
        return {IndexedPaneCaptureState::LimitExceeded, {}};
    auto page = std::shared_ptr<IndexedPanePage>(new IndexedPanePage);
    page->first_ = first;
    page->total_ = rows();
    page->pending_ = listing_pending();
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
            page->rows_.push_back({id, std::move(name), std::move(metadata), std::move(row.record.hidden),
                                   std::move(inspected), false, focus_ && *focus_ == id});
        }
    }
    if (!accepts(stamp))
        return {IndexedPaneCaptureState::Stale, {}};
    page->text_bytes_ = used;
    page->stamp_ = stamp;
    return {IndexedPaneCaptureState::Ready, std::move(page)};
}
IndexedPaneFocus IndexedPane::focus(const PaneStateStamp &stamp, PaneRowId row, std::stop_token stop) {
    if (!accepts(stamp))
        return IndexedPaneFocus::Stale;
    if (pending_ || !data_)
        return IndexedPaneFocus::Unavailable;
    if (row.scope != data_->row_scope() || !row.number || row.number > data_->rows())
        return IndexedPaneFocus::InvalidTarget;
    if (stop.stop_requested())
        return IndexedPaneFocus::Cancelled;
    const auto data = data_;
    auto found = [&] {
        try {
            return data->lookup_->find_row({row.scope, row.number});
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
    if (stop.stop_requested())
        return IndexedPaneFocus::Cancelled;
    if (!accepts(stamp))
        return IndexedPaneFocus::Stale;
    if (focus_ && *focus_ == row)
        return IndexedPaneFocus::Applied;
    invalidate();
    focus_ = std::move(row);
    return IndexedPaneFocus::Applied;
}
} // namespace ck::files
