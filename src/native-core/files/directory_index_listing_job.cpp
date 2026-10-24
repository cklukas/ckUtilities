// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_listing_job.hpp"
#include <stdexcept>
namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char *operation,
                             ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
bool differs(const Observation<FileTimestamp> &a, const Observation<FileTimestamp> &b) {
    return a.value() && b.value() && *a.value() != *b.value();
}
} // namespace
DirectoryIndexListingJob::DirectoryIndexListingJob(
    PaneListingRequest request, std::unique_ptr<DirectoryListing> listing, StorageModel model,
    std::shared_ptr<IndexFileFactory> factory, std::shared_ptr<IndexStorageBudget> budget,
    std::array<std::byte, 16> generation, DirectoryIndexStoreLimits store_limits, DirectoryIndexSortLimits sort_limits)
    : request_(std::move(request)), listing_(std::move(listing)), model_(model), sort_limits_(sort_limits) {
    if (!listing_ || !request_.scope.is_valid() || !request_.number ||
        (model != StorageModel::Unknown && model != StorageModel::LinuxStatx &&
         model != StorageModel::DarwinStatFlags && model != StorageModel::WindowsHandleAttributes))
        throw std::invalid_argument("invalid indexed listing input");
    // Validate sorter bounds before any native query or scratch creation.
    if (!sort_limits.block.rows || sort_limits.block.rows > DirectoryIndexScanLimits::hard_rows ||
        sort_limits.block.wire_bytes < 16 || sort_limits.block.wire_bytes > DirectoryIndexScanLimits::hard_wire_bytes ||
        !sort_limits.merge_rows || sort_limits.merge_rows > DirectoryIndexScanLimits::hard_rows)
        throw std::invalid_argument("invalid indexed listing sort limits");
    auto scope = listing_->source_scope();
    if (!scope.value()) {
        fail(*scope.unavailable_detail());
        return;
    }
    if (!scope.value()->is_valid()) {
        fail(error(AdapterError::InvalidIdentifier, "index_listing.scope"));
        return;
    }
    source_scope_ = *scope.value();
    directory_ = listing_->identity();
    pin_ = listing_->pin_directory();
    before_ = pin_->inspect_basic();
    if (!check_directory(before_))
        return;
    auto created = DirectoryIndexStore::create_new(std::move(factory), std::move(budget), {source_scope_, generation},
                                                   store_limits);
    if (!created.value()) {
        fail(*created.unavailable_detail());
        return;
    }
    store_ = std::move(created).take_value();
}
void DirectoryIndexListingJob::release() noexcept {
    sorter_.reset();
    store_.reset();
    cursor_.reset();
    previous_.reset();
    listing_.reset();
    pin_.reset();
}
void DirectoryIndexListingJob::fail(UnavailableObservation detail) {
    state_ = DirectoryIndexListingState::Failed;
    failure_ = std::move(detail);
    release();
}
void DirectoryIndexListingJob::cancel() noexcept {
    state_ = DirectoryIndexListingState::Cancelled;
    release();
    try {
        failure_ = error(AdapterError::InterruptedRead, "index_listing.cancelled", ObservationState::Unknown);
    } catch (...) {
        failure_.reset();
    }
}
bool DirectoryIndexListingJob::abandon() noexcept {
    if (state_ == DirectoryIndexListingState::Failed || state_ == DirectoryIndexListingState::Cancelled ||
        state_ == DirectoryIndexListingState::Abandoned)
        return false;
    release();
    result_.reset();
    state_ = DirectoryIndexListingState::Abandoned;
    return true;
}
bool DirectoryIndexListingJob::check_directory(const Observation<BasicMetadata> &value) {
    if (value.state() == ObservationState::Gone || value.state() == ObservationState::Changed) {
        fail(*value.unavailable_detail());
        return false;
    }
    if (directory_.value() && directory_.value()->scope != source_scope_) {
        fail(error(AdapterError::InvalidIdentifier, "index_listing.directory_scope"));
        return false;
    }
    if (!value.value())
        return true; // uncertainty retained, never converted into snapshot stability
    const auto &m = *value.value();
    if (m.kind.value() && *m.kind.value() != ObjectKind::Directory) {
        fail(error(AdapterError::NotDirectory, "index_listing.directory_kind"));
        return false;
    }
    if (m.identity.value() && m.identity.value()->scope != source_scope_) {
        fail(error(AdapterError::InvalidIdentifier, "index_listing.metadata_scope"));
        return false;
    }
    if ((directory_.value() && m.identity.value() &&
         compare_identity(*directory_.value(), *m.identity.value()) == IdentityComparison::Different) ||
        (before_.value() && before_.value()->identity.value() && m.identity.value() &&
         compare_identity(*before_.value()->identity.value(), *m.identity.value()) == IdentityComparison::Different) ||
        (before_.value() &&
         (differs(before_.value()->modified, m.modified) || differs(before_.value()->changed, m.changed)))) {
        fail(error(AdapterError::MetadataChanged, "index_listing.directory_changed", ObservationState::Changed));
        return false;
    }
    return true;
}
void DirectoryIndexListingJob::collect(std::stop_token stop) {
    auto batch = listing_->read(stop);
    if (batch.state == DirectoryReadState::Error) {
        fail(*batch.error);
        return;
    }
    if (batch.state == DirectoryReadState::Cancelled || stop.stop_requested()) {
        cancel();
        return;
    }
    for (auto &entry : batch.entries) {
        if (stop.stop_requested()) {
            cancel();
            return;
        }
        const bool windows = entry.name.windows_units() != nullptr;
        if (!entry.name.is_valid() || (encoding_ && *encoding_ != entry.name.encoding()) ||
            (model_ == StorageModel::WindowsHandleAttributes && !windows) ||
            ((model_ == StorageModel::LinuxStatx || model_ == StorageModel::DarwinStatFlags) && windows)) {
            fail(error(AdapterError::InvalidIdentifier, "index_listing.name"));
            return;
        }
        encoding_ = entry.name.encoding();
        // Store checks overflow/limits before encoding or writing this ordinal.
        if (store_->rows() == UINT64_MAX) {
            fail(error(AdapterError::InventoryLimit, "index_listing.rows"));
            return;
        }
        auto hidden = pane_hidden_state(entry.name, entry.metadata, model_);
        auto appended = store_->append(
            {store_->rows() + 1, std::move(entry.name), std::move(entry.metadata), std::move(hidden)}, stop);
        if (!appended.value()) {
            if (stop.stop_requested() && appended.unavailable_detail()->error.domain == ErrorDomain::Adapter &&
                appended.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead))
                cancel();
            else
                fail(*appended.unavailable_detail());
            return;
        }
    }
    if (batch.state == DirectoryReadState::End) {
        auto sealed = store_->seal(stop);
        if (!sealed.value()) {
            if (stop.stop_requested() && sealed.unavailable_detail()->error.domain == ErrorDomain::Adapter &&
                sealed.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead))
                cancel();
            else
                fail(*sealed.unavailable_detail());
            return;
        }
        listing_.reset();
        sorter_ =
            std::make_unique<DirectoryIndexSortJob>(store_, PaneOrder{PaneSortKey::Name, false, false}, sort_limits_);
        state_ = DirectoryIndexListingState::SortingNames;
    }
}
void DirectoryIndexListingJob::check_names(std::stop_token stop) {
    auto read = store_->read_page(*cursor_, sort_limits_.block, stop);
    if (!read.value()) {
        if (stop.stop_requested() && read.unavailable_detail()->error.domain == ErrorDomain::Adapter &&
            read.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead))
            cancel();
        else
            fail(*read.unavailable_detail());
        return;
    }
    auto page = std::move(read).take_value();
    for (const auto &row : page.rows) {
        if (stop.stop_requested()) {
            cancel();
            return;
        }
        if (previous_ && compare_native_names(*previous_, row.record.name) >= 0) {
            fail(error(AdapterError::InvalidNativeResult, "index_listing.duplicate_or_order"));
            return;
        }
        previous_ = row.record.name;
    }
    cursor_ = page.next;
    if (!page.end)
        return;
    auto scope = pin_->source_scope();
    if (!scope.value()) {
        fail(*scope.unavailable_detail());
        return;
    }
    if (*scope.value() != source_scope_) {
        fail(error(AdapterError::InvalidIdentifier, "index_listing.scope_changed"));
        return;
    }
    auto after = pin_->inspect_basic();
    if (!check_directory(after))
        return;
    if (stop.stop_requested()) {
        cancel();
        return;
    }
    result_ = Result(new const CompletedDirectoryIndex(request_, source_scope_, directory_, before_, std::move(after),
                                                       store_, pin_));
    state_ = DirectoryIndexListingState::Complete;
    release();
}
DirectoryIndexListingState DirectoryIndexListingJob::advance(std::stop_token stop) {
    if (state_ != DirectoryIndexListingState::Collecting && state_ != DirectoryIndexListingState::SortingNames &&
        state_ != DirectoryIndexListingState::CheckingNames)
        return state_;
    try {
        if (stop.stop_requested()) {
            cancel();
            return state_;
        }
        if (state_ == DirectoryIndexListingState::Collecting)
            collect(stop);
        else if (state_ == DirectoryIndexListingState::CheckingNames)
            check_names(stop);
        else {
            const auto sorted = sorter_->advance(stop);
            if (sorted == DirectoryIndexSortState::Complete) {
                store_ = *sorter_->completed().value();
                sorter_.reset();
                auto cursor = store_->begin_scan();
                if (!cursor.value()) {
                    fail(*cursor.unavailable_detail());
                    return state_;
                }
                cursor_ = std::move(cursor).take_value();
                state_ = DirectoryIndexListingState::CheckingNames;
            } else if (sorted == DirectoryIndexSortState::Cancelled)
                cancel();
            else if (sorted != DirectoryIndexSortState::Running) {
                auto result = sorter_->completed();
                fail(*result.unavailable_detail());
            }
        }
        return state_;
    } catch (...) {
        state_ = DirectoryIndexListingState::Failed;
        failure_.reset();
        release();
        throw;
    }
}
Observation<DirectoryIndexListingJob::Result> DirectoryIndexListingJob::completed() const {
    if (state_ == DirectoryIndexListingState::Complete && result_)
        return Observation<Result>::available(result_);
    if (failure_)
        return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
} // namespace ck::files
