// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_listing.hpp"
#include <stdexcept>

namespace ck::files {
void DirectoryListing::validate(DirectoryListingLimits limits) {
    if (limits.rows == 0 || limits.rows > DirectoryCursor::max_batch_size ||
        limits.name_bytes == 0 || limits.name_bytes > 8 * 1024 * 1024 ||
        limits.component_bytes == 0 || limits.component_bytes > 1024 * 1024 ||
        limits.component_bytes > limits.name_bytes ||
        limits.time_slice <= MonotonicClock::Duration::zero() || limits.time_slice > std::chrono::seconds(1)) throw std::invalid_argument("invalid listing limits");
}
DirectoryListing::DirectoryListing(std::unique_ptr<DirectoryCursor> cursor, DirectoryListingLimits limits,
    std::shared_ptr<const MonotonicClock> clock)
    : cursor_(std::move(cursor)), limits_(limits), clock_(std::move(clock)) {
    validate(limits);
    if (!cursor_ || !clock_) throw std::invalid_argument("null listing cursor or clock");
}
Observation<std::unique_ptr<DirectoryListing>> DirectoryListing::open(const BasicFileSystem& filesystem,
    const std::filesystem::path& path, DirectoryListingLimits limits, std::shared_ptr<const MonotonicClock> clock) {
    using Result = Observation<std::unique_ptr<DirectoryListing>>;
    validate(limits);
    if (!clock) throw std::invalid_argument("null listing clock");
    if (!path.is_absolute() || path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
        return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "listing.path"});
    auto opened = filesystem.open_directory(path, DirectoryLinkPolicy::RejectFinalLink);
    if (!opened.value()) { const auto& e = *opened.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    auto cursor = std::move(opened).take_value();
    if (!cursor) return Result::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "listing.null_cursor"});
    return Result::available(std::make_unique<DirectoryListing>(std::move(cursor), limits, std::move(clock)));
}
DirectoryListingBatch DirectoryListing::read(std::stop_token stop) {
    if (failure_) return {{}, DirectoryReadState::Error, failure_};
    if (ended_) return {{}, DirectoryReadState::End, {}};
    DirectoryListingBatch batch;
    const auto fail = [&](AdapterError code, const char* operation) {
        failure_ = UnavailableObservation{ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
        pending_.reset(); prepared_.reset(); batch.state = DirectoryReadState::Error; batch.error = failure_;
    };
    const auto cancelled = [&] {
        if (!stop.stop_requested()) return false;
        prepared_.reset(); batch.state = DirectoryReadState::Cancelled; return true;
    };
    try {
        if (cancelled()) return batch;
        CooperativeTimeSlice slice(*clock_, limits_.time_slice, last_clock_reading_);
        const auto yield = [&] {
            if (cancelled()) return true;
            const auto expired = slice.expired();
            last_clock_reading_ = slice.last_reading();
            if (!expired.value()) {
                failure_ = *expired.unavailable_detail(); pending_.reset(); prepared_.reset();
                batch.state = DirectoryReadState::Error; batch.error = failure_; return true;
            }
            batch.time_slice_exhausted = *expired.value(); return batch.time_slice_exhausted;
        };
        batch.entries.reserve(limits_.rows);
        std::size_t bytes = 0;
        for (std::size_t reads = 0; reads < limits_.rows; ++reads) {
            if (yield()) return batch;
            if (!pending_) {
                auto native = cursor_->read(1, stop);
                if (!native.names.empty()) pending_ = std::move(native.names.front());
                if (native.state == DirectoryReadState::Error) {
                    failure_ = native.error; batch.state = DirectoryReadState::Error; batch.error = failure_; return batch;
                }
                if (native.state == DirectoryReadState::End) { ended_ = true; batch.state = DirectoryReadState::End; return batch; }
                if (native.state == DirectoryReadState::Cancelled) { batch.state = DirectoryReadState::Cancelled; return batch; }
                if (!pending_) return batch;
            }
            if (!pending_->is_valid()) { fail(AdapterError::InvalidNativeResult, "listing.name"); return batch; }
            const auto unit_size = pending_->posix_bytes() ? std::size_t{1} : std::size_t{2};
            const auto units = pending_->posix_bytes() ? pending_->posix_bytes()->size() : pending_->windows_units()->size();
            if (units > limits_.component_bytes / unit_size) { fail(AdapterError::InventoryLimit, "listing.component_bytes"); return batch; }
            const auto count = units * unit_size;
            if (count > limits_.name_bytes - bytes) return batch;
            if (yield()) return batch;
            Observation<BasicMetadata> metadata;
            if (!prepared_) {
                auto opened = cursor_->open_child(*pending_, EntryAccess::MetadataOnly);
                const bool was_available = opened.value() != nullptr;
                if (was_available) prepared_ = std::move(opened).take_value();
                else { const auto& e = *opened.unavailable_detail(); metadata = Observation<BasicMetadata>::unavailable(e.state, e.error); }
                if (cancelled()) return batch;
                if (prepared_) { if (yield()) return batch; }
                else if (was_available) metadata = Observation<BasicMetadata>::unavailable(ObservationState::Failed,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "listing.null_child"});
            }
            if (prepared_) { metadata = prepared_->inspect_basic(); prepared_.reset(); }
            batch.entries.push_back({std::move(*pending_), std::move(metadata)});
            pending_.reset(); bytes += count;
            if (yield()) return batch;
        }
        return batch;
    } catch (...) {
        failure_ = UnavailableObservation{ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}};
        pending_.reset(); prepared_.reset(); throw;
    }
}
} // namespace ck::files
