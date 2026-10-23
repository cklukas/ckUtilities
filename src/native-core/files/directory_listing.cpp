// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_listing.hpp"
#include <stdexcept>

namespace ck::files {
void DirectoryListing::validate(DirectoryListingLimits limits) {
    if (limits.rows == 0 || limits.rows > DirectoryCursor::max_batch_size ||
        limits.name_bytes == 0 || limits.name_bytes > 8 * 1024 * 1024 ||
        limits.component_bytes == 0 || limits.component_bytes > 1024 * 1024 ||
        limits.component_bytes > limits.name_bytes) throw std::invalid_argument("invalid listing limits");
}
DirectoryListing::DirectoryListing(std::unique_ptr<DirectoryCursor> cursor, DirectoryListingLimits limits)
    : cursor_(std::move(cursor)), limits_(limits) {
    validate(limits);
    if (!cursor_) throw std::invalid_argument("null listing cursor");
}
Observation<std::unique_ptr<DirectoryListing>> DirectoryListing::open(const BasicFileSystem& filesystem,
    const std::filesystem::path& path, DirectoryListingLimits limits) {
    using Result = Observation<std::unique_ptr<DirectoryListing>>;
    validate(limits);
    if (!path.is_absolute() || path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
        return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "listing.path"});
    auto opened = filesystem.open_directory(path, DirectoryLinkPolicy::RejectFinalLink);
    if (!opened.value()) { const auto& e = *opened.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    auto cursor = std::move(opened).take_value();
    if (!cursor) return Result::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "listing.null_cursor"});
    return Result::available(std::make_unique<DirectoryListing>(std::move(cursor), limits));
}
DirectoryListingBatch DirectoryListing::read(std::stop_token stop) {
    if (failure_) return {{}, DirectoryReadState::Error, failure_};
    if (ended_) return {{}, DirectoryReadState::End, {}};
    DirectoryListingBatch batch;
    const auto fail = [&](AdapterError code, const char* operation) {
        failure_ = UnavailableObservation{ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
        pending_.reset(); batch.state = DirectoryReadState::Error; batch.error = failure_;
    };
    try {
        batch.entries.reserve(limits_.rows);
        std::size_t bytes = 0;
        for (std::size_t reads = 0; reads < limits_.rows; ++reads) {
            if (stop.stop_requested()) { batch.state = DirectoryReadState::Cancelled; return batch; }
            if (!pending_) {
                auto native = cursor_->read(1, stop);
                if (!native.names.empty()) pending_ = std::move(native.names.front());
                if (native.state == DirectoryReadState::Error) {
                    failure_ = native.error; batch.state = DirectoryReadState::Error; batch.error = failure_; return batch;
                }
                if (native.state == DirectoryReadState::End) { ended_ = true; batch.state = DirectoryReadState::End; return batch; }
                if (native.state == DirectoryReadState::Cancelled) { batch.state = DirectoryReadState::Cancelled; return batch; }
                // A bounded native read may consume only skipped records.
                if (!pending_) return batch;
            }
            if (!pending_->is_valid()) { fail(AdapterError::InvalidNativeResult, "listing.name"); return batch; }
            const auto unit_size = pending_->posix_bytes() ? std::size_t{1} : std::size_t{2};
            const auto units = pending_->posix_bytes() ? pending_->posix_bytes()->size() : pending_->windows_units()->size();
            if (units > limits_.component_bytes / unit_size) { fail(AdapterError::InventoryLimit, "listing.component_bytes"); return batch; }
            const auto count = units * unit_size;
            if (count > limits_.name_bytes - bytes) return batch; // retained for the next batch
            if (stop.stop_requested()) { batch.state = DirectoryReadState::Cancelled; return batch; }
            Observation<BasicMetadata> metadata;
            {
                auto opened = cursor_->open_child(*pending_, EntryAccess::MetadataOnly);
                if (stop.stop_requested()) { batch.state = DirectoryReadState::Cancelled; return batch; }
                if (opened.value()) {
                    const auto& entry = *opened.value();
                    metadata = entry ? entry->inspect_basic() : Observation<BasicMetadata>::unavailable(ObservationState::Failed,
                        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "listing.null_child"});
                } else { const auto& e = *opened.unavailable_detail(); metadata = Observation<BasicMetadata>::unavailable(e.state, e.error); }
            }
            batch.entries.push_back({std::move(*pending_), std::move(metadata)});
            pending_.reset(); bytes += count;
            if (stop.stop_requested()) { batch.state = DirectoryReadState::Cancelled; return batch; }
        }
        return batch;
    } catch (...) {
        // Consumption may already have occurred. Never resume after an exception
        // and later report a falsely complete enumeration. Avoid allocating here.
        failure_ = UnavailableObservation{ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}};
        pending_.reset(); throw;
    }
}
} // namespace ck::files
