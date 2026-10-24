// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "indexed_pane_sources.hpp"
#include "indexed_pane.hpp"
#include "directory_index_observation.hpp"
#include "pane_page_budget.hpp"
namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char *op, ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op}};
}
template <class T> Observation<T> unavailable(const UnavailableObservation &e) {
    return Observation<T>::unavailable(e.state, e.error);
}
} // namespace
IdentityComparison IndexedBoundSource::cached_identity_match() const noexcept {
    if (!cached_.value() || !cached_.value()->identity.value() || !held_->metadata().identity.value())
        return IdentityComparison::Unknown;
    return compare_identity(*cached_.value()->identity.value(), *held_->metadata().identity.value());
}
IndexedPaneSourceJob::IndexedPaneSourceJob(std::shared_ptr<const CompletedIndexedPaneData> data,
                                           std::shared_ptr<DirectoryIndexMarks> marks, std::optional<PaneRowId> focus,
                                           PaneStateStamp stamp, PaneSourcesOrigin origin, std::uint64_t expected,
                                           IndexedSourceLimits limits)
    : data_(std::move(data)), marks_(std::move(marks)), candidate_(std::move(focus)), stamp_(std::move(stamp)),
      origin_(origin), expected_(expected), limits_(limits) {
    if (!limits.held_entries || limits.held_entries > 4096 || !limits.native_name_bytes ||
        limits.native_name_bytes > 64 * 1024 * 1024 || !limits.diagnostic_bytes ||
        limits.diagnostic_bytes > 16 * 1024 * 1024 || !limits.scan_rows || limits.scan_rows > 4096 ||
        limits.wire_bytes < 16 || limits.wire_bytes > DirectoryIndexScanLimits::hard_wire_bytes ||
        !valid_entry_access(limits.access))
        throw std::invalid_argument("invalid indexed source limits");
    if (!expected_ || expected_ > limits.held_entries) {
        fail(error(AdapterError::InventoryLimit, "indexed_sources.held_limit"));
        return;
    }
    if (origin_ == PaneSourcesOrigin::Marks)
        candidate_.reset();
    sources_.reserve(static_cast<std::size_t>(expected_));
}
void IndexedPaneSourceJob::release() noexcept {
    data_.reset();
    marks_.reset();
    bits_.reset();
    candidate_.reset();
    std::vector<IndexedBoundSource>{}.swap(sources_);
    before_ = {};
}
void IndexedPaneSourceJob::fail(UnavailableObservation e) {
    state_ = IndexedSourceJobState::Failed;
    failure_ = std::move(e);
    release();
}
void IndexedPaneSourceJob::finish_without_result(IndexedSourceJobState state) noexcept {
    state_ = state;
    failure_.reset();
    release();
}
bool IndexedPaneSourceJob::boundary(std::stop_token stop) {
    if (!stamp_.current()) {
        finish_without_result(IndexedSourceJobState::Stale);
        return false;
    }
    if (data_->store_->state() != DirectoryIndexStoreState::Sealed ||
        data_->lookup_->state() != DirectoryIndexLookupState::Ready ||
        (marks_ && marks_->state() != DirectoryIndexMarksState::Ready)) {
        auto e = marks_ && marks_->state() != DirectoryIndexMarksState::Ready
                     ? marks_->failure().value_or(error(AdapterError::InvalidAccess, "indexed_sources.marks"))
                     : data_->lookup_->failure().value_or(data_->store_->failure().value_or(
                           error(AdapterError::InvalidAccess, "indexed_sources.dataset")));
        stamp_.token_->current.store(false, std::memory_order_release);
        fail(std::move(e));
        return false;
    }
    if (stop.stop_requested()) {
        finish_without_result(IndexedSourceJobState::Cancelled);
        return false;
    }
    return true;
}
Observation<BasicMetadata> IndexedPaneSourceJob::inspect_directory(std::stop_token stop) {
    auto scope = data_->pin_->source_scope();
    if (!boundary({}))
        return {};
    if (!scope.value())
        return unavailable<BasicMetadata>(*scope.unavailable_detail());
    if (*scope.value() != data_->source_scope_)
        return unavailable<BasicMetadata>(error(AdapterError::InvalidIdentifier, "indexed_sources.directory_scope"));
    if (!boundary(stop))
        return {};
    auto observed = data_->pin_->inspect_basic();
    if (!boundary({}))
        return {};
    if (!observed.value())
        return observed;
    const auto &m = *observed.value();
    for (const auto *id : {&data_->directory_, &m.identity}) {
        if (!id->value())
            return unavailable<BasicMetadata>(*id->unavailable_detail());
        if (id->value()->scope != data_->source_scope_ ||
            compare_identity(*id->value(), *id->value()) != IdentityComparison::Same)
            return unavailable<BasicMetadata>(
                error(AdapterError::UnqualifiedIdentity, "indexed_sources.parent_identity"));
    }
    const auto identity_match = compare_identity(*data_->directory_.value(), *m.identity.value());
    if (identity_match != IdentityComparison::Same)
        return unavailable<BasicMetadata>(error(
            AdapterError::UnqualifiedIdentity, "indexed_sources.parent_continuity",
            identity_match == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown));
    if (!m.kind.value())
        return unavailable<BasicMetadata>(*m.kind.unavailable_detail());
    if (*m.kind.value() != ObjectKind::Directory)
        return unavailable<BasicMetadata>(error(AdapterError::NotDirectory, "indexed_sources.parent_kind"));
    auto checked = detail::check_index_directory(data_->source_scope_, data_->directory_,
                                                 Observation<BasicMetadata>::available(data_->baseline_), observed);
    if (!checked.value())
        return unavailable<BasicMetadata>(*checked.unavailable_detail());
    return observed;
}
void IndexedPaneSourceJob::scan(std::stop_token stop) {
    for (std::size_t n = 0; next_ < data_->rows() && n < limits_.scan_rows; ++n) {
        if (!boundary(stop))
            return;
        const auto block = next_ / 4096;
        if (!bits_ || block_ != block) {
            auto read = marks_->read_block(block);
            if (!boundary({}))
                return;
            if (!read.value()) {
                fail(*read.unavailable_detail());
                return;
            }
            bits_ = std::move(read).take_value();
            block_ = block;
            counted_ += DirectoryIndexMarks::count(*bits_);
            if (counted_ > expected_)
                break;
        }
        const auto at = static_cast<std::size_t>(next_ % 4096);
        const auto number = ++next_;
        if (DirectoryIndexMarks::bit(*bits_, at)) {
            candidate_.emplace(PaneRowId{data_->row_scope(), number});
            phase_ = Phase::Acquire;
            return;
        }
    }
    if (counted_ > expected_ || (next_ == data_->rows() && counted_ != expected_)) {
        auto e = error(AdapterError::InvalidNativeResult, "indexed_sources.mark_count");
        marks_->fail(e);
        stamp_.token_->current.store(false, std::memory_order_release);
        fail(std::move(e));
        return;
    }
    if (!boundary(stop))
        return;
    if (next_ == data_->rows()) {
        if (sources_.size() != expected_) {
            fail(error(AdapterError::InvalidNativeResult, "indexed_sources.source_count"));
            return;
        }
        bits_.reset();
        phase_ = Phase::After;
    }
}
void IndexedPaneSourceJob::acquire(std::stop_token stop) {
    const auto key = *candidate_;
    auto position = data_->lookup_->locate_row({key.scope, key.number});
    if (!boundary({}))
        return;
    if (!position.value()) {
        fail(*position.unavailable_detail());
        return;
    }
    if (!*position.value()) {
        auto e = error(AdapterError::InvalidNativeResult, "indexed_sources.excluded_selection");
        if (marks_)
            marks_->fail(e);
        else
            data_->lookup_->fail(e);
        stamp_.token_->current.store(false, std::memory_order_release);
        fail(std::move(e));
        return;
    }
    if (!boundary(stop))
        return;
    auto page = data_->lookup_->read_page(**position.value(), {1, limits_.wire_bytes});
    if (!boundary({}))
        return;
    if (!page.value()) {
        fail(*page.unavailable_detail());
        return;
    }
    if (!boundary(stop))
        return;
    if (page.value()->rows.size() != 1 || page.value()->rows.front().record.row_id != key.number) {
        auto e = error(AdapterError::InvalidNativeResult, "indexed_sources.row");
        data_->lookup_->fail(e);
        stamp_.token_->current.store(false, std::memory_order_release);
        fail(std::move(e));
        return;
    }
    auto raw = std::move(page).take_value();
    auto row = std::move(raw.rows.front().record);
    const auto units = row.name.posix_bytes() ? row.name.posix_bytes()->size() : row.name.windows_units()->size();
    const auto width = row.name.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    if (units > (limits_.native_name_bytes - names_) / width ||
        !(row.metadata.value() ? detail::charge(*row.metadata.value(), diagnostics_, limits_.diagnostic_bytes)
                               : detail::charge(row.metadata, diagnostics_, limits_.diagnostic_bytes))) {
        fail(error(AdapterError::InventoryLimit, "indexed_sources.bytes"));
        return;
    }
    names_ += units * width;
    // No stop token passed into the native helper: inspect its real failure
    // before honoring a concurrent stop. Never mask EACCES/other native errors.
    auto opened = HeldChild::observe_with_access(data_->pin_, std::move(row.name), limits_.access);
    if (!boundary({}))
        return;
    if (!opened.value()) {
        fail(*opened.unavailable_detail());
        return;
    }
    const auto &fresh = (*opened.value())->metadata();
    if (!fresh.identity.value()) {
        fail(*fresh.identity.unavailable_detail());
        return;
    }
    if (fresh.identity.value()->scope != data_->source_scope_ ||
        compare_identity(*fresh.identity.value(), *fresh.identity.value()) != IdentityComparison::Same) {
        fail(error(AdapterError::UnqualifiedIdentity, "indexed_sources.child_identity"));
        return;
    }
    if (!fresh.kind.value()) {
        fail(*fresh.kind.unavailable_detail());
        return;
    }
    if (*fresh.kind.value() < ObjectKind::RegularFile || *fresh.kind.value() > ObjectKind::Other) {
        fail(error(AdapterError::InvalidNativeResult, "indexed_sources.child_kind"));
        return;
    }
    if (!boundary(stop))
        return;
    if (!detail::charge(fresh, diagnostics_, limits_.diagnostic_bytes)) {
        fail(error(AdapterError::InventoryLimit, "indexed_sources.diagnostics"));
        return;
    }
    if (sources_.size() == expected_) {
        fail(error(AdapterError::InvalidNativeResult, "indexed_sources.extra_source"));
        return;
    }
    sources_.push_back(IndexedBoundSource(key, {bindings_scope_, sources_.size() + 1}, std::move(opened).take_value(),
                                          std::move(row.metadata)));
    candidate_.reset();
    phase_ = origin_ == PaneSourcesOrigin::Marks ? Phase::Scan : Phase::After;
}
IndexedSourceJobState IndexedPaneSourceJob::advance(std::stop_token stop) {
    if (state_ != IndexedSourceJobState::Running)
        return state_;
    try {
        if (!boundary(stop))
            return state_;
        if (phase_ == Phase::Scan)
            scan(stop);
        else if (phase_ == Phase::Acquire)
            acquire(stop);
        else {
            auto observed = inspect_directory(stop);
            if (state_ != IndexedSourceJobState::Running)
                return state_;
            if (!observed.value()) {
                fail(*observed.unavailable_detail());
                return state_;
            }
            if (!boundary(stop))
                return state_;
            if (!detail::charge(*observed.value(), diagnostics_, limits_.diagnostic_bytes)) {
                fail(error(AdapterError::InventoryLimit, "indexed_sources.directory_diagnostics"));
                return state_;
            }
            if (phase_ == Phase::Before) {
                before_ = std::move(observed).take_value();
                phase_ = origin_ == PaneSourcesOrigin::Marks ? Phase::Scan : Phase::Acquire;
            } else {
                auto checked = detail::check_index_directory(data_->source_scope_, data_->directory_,
                                                             Observation<BasicMetadata>::available(before_), observed);
                if (!checked.value()) {
                    fail(*checked.unavailable_detail());
                    return state_;
                }
                if (sources_.size() != expected_) {
                    fail(error(AdapterError::InvalidNativeResult, "indexed_sources.incomplete"));
                    return state_;
                }
                result_ = Result(new const CompletedIndexedSources(
                    stamp_, data_->revision_, marks_ ? std::optional<AdapterScope>(marks_->revision()) : std::nullopt,
                    origin_, std::move(before_), std::move(observed).take_value(), std::move(sources_), names_,
                    diagnostics_));
                state_ = IndexedSourceJobState::Complete;
                release();
            }
        }
        return state_;
    } catch (...) {
        if (data_ && (data_->lookup_->state() != DirectoryIndexLookupState::Ready ||
                      data_->store_->state() != DirectoryIndexStoreState::Sealed ||
                      (marks_ && marks_->state() != DirectoryIndexMarksState::Ready)))
            stamp_.token_->current.store(false, std::memory_order_release);
        finish_without_result(IndexedSourceJobState::Failed);
        throw;
    }
}
bool IndexedPaneSourceJob::abandon() noexcept {
    if (state_ == IndexedSourceJobState::Abandoned || state_ == IndexedSourceJobState::Failed ||
        state_ == IndexedSourceJobState::Cancelled || state_ == IndexedSourceJobState::Stale)
        return false;
    result_.reset();
    finish_without_result(IndexedSourceJobState::Abandoned);
    return true;
}
Observation<IndexedPaneSourceJob::Result> IndexedPaneSourceJob::completed() const {
    if (state_ == IndexedSourceJobState::Complete && result_ && stamp_.current())
        return Observation<Result>::available(result_);
    if (failure_)
        return unavailable<Result>(*failure_);
    return unavailable<Result>(
        error(state_ == IndexedSourceJobState::Cancelled ? AdapterError::InterruptedRead : AdapterError::InvalidAccess,
              "indexed_sources.incomplete_or_stale", ObservationState::Unknown));
}
} // namespace ck::files
