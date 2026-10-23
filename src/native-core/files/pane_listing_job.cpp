// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_listing_job.hpp"
#include <stdexcept>

namespace ck::files {
Observation<bool> pane_hidden_state(const NativeName& name, const Observation<BasicMetadata>& metadata, StorageModel model) {
    const bool windows = name.windows_units() != nullptr;
    if (!name.is_valid() || (model != StorageModel::Unknown && model != StorageModel::LinuxStatx &&
        model != StorageModel::DarwinStatFlags && model != StorageModel::WindowsHandleAttributes) ||
        (model == StorageModel::WindowsHandleAttributes && !windows) ||
        ((model == StorageModel::LinuxStatx || model == StorageModel::DarwinStatFlags) && windows))
        return Observation<bool>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "pane.hidden_input"});
    if (model == StorageModel::Unknown) return {};
    if (model != StorageModel::WindowsHandleAttributes && name.posix_bytes()->front() == '.')
        return Observation<bool>::available(true);
    if (model == StorageModel::LinuxStatx) return Observation<bool>::available(false);
    if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Observation<bool>::unavailable(e.state, e.error); }
    const auto storage = storage_from_basic(*metadata.value(), model);
    if (!storage.value()) { const auto& e = *storage.unavailable_detail(); return Observation<bool>::unavailable(e.state, e.error); }
    const auto flag = reported_storage_flag(*storage.value(), StorageFeature::HiddenFlag);
    if (!flag.value()) { const auto& e = *flag.unavailable_detail(); return Observation<bool>::unavailable(e.state, e.error); }
    return Observation<bool>::available(*flag.value() == ReportedFlag::Set);
}
PaneListingJob::PaneListingJob(PaneListingRequest request, std::unique_ptr<DirectoryListing> listing,
    StorageModel model, PaneLimits limits, ListingDirectoryRetention retention)
    : request_(std::move(request)), listing_(std::move(listing)), model_(model), limits_(limits) {
    validate_pane_limits(limits);
    if (!listing_ || !request_.scope.is_valid() || request_.number == 0 ||
        (model != StorageModel::Unknown && model != StorageModel::LinuxStatx && model != StorageModel::DarwinStatFlags && model != StorageModel::WindowsHandleAttributes))
        throw std::invalid_argument("invalid pane listing job");
    if (retention != ListingDirectoryRetention::ReleaseAtEnd && retention != ListingDirectoryRetention::RetainForRefresh)
        throw std::invalid_argument("invalid listing directory retention");
    directory_ = listing_->identity();
    if (retention == ListingDirectoryRetention::RetainForRefresh) directory_pin_ = listing_->pin_directory();
}
void PaneListingJob::fail(UnavailableObservation failure) {
    failure_ = std::move(failure); state_ = PaneListingJobState::Failed;
    release_staging();
}
void PaneListingJob::release_staging() noexcept {
    listing_.reset(); directory_pin_.reset(); std::vector<PaneEntry>{}.swap(entries_); names_.clear();
    encoding_.reset(); directory_ = {}; native_bytes_ = display_bytes_ = 0;
}
bool PaneListingJob::abandon() noexcept {
    if (state_ == PaneListingJobState::Abandoned || state_ == PaneListingJobState::Failed) return false;
    completed_.reset(); release_staging(); state_ = PaneListingJobState::Abandoned;
    return true;
}
Observation<PaneListingJob::Result> PaneListingJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
PaneListingJobState PaneListingJob::advance(std::stop_token stop) {
    if (state_ == PaneListingJobState::Complete || state_ == PaneListingJobState::Failed || state_ == PaneListingJobState::Abandoned) return state_;
    try {
        auto batch = listing_->read(stop);
        if (batch.state == DirectoryReadState::Error) { fail(*batch.error); return state_; }
        for (auto& entry : batch.entries) {
            const bool windows = entry.name.windows_units() != nullptr;
            if (!entry.name.is_valid() || (encoding_ && *encoding_ != entry.name.encoding()) ||
                (model_ == StorageModel::WindowsHandleAttributes && !windows) ||
                ((model_ == StorageModel::LinuxStatx || model_ == StorageModel::DarwinStatFlags) && windows)) {
                fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "pane_listing.name"}}); return state_;
            }
            const auto unit_size = windows ? std::size_t{2} : std::size_t{1};
            const auto units = windows ? entry.name.windows_units()->size() : entry.name.posix_bytes()->size();
            if (entries_.size() >= limits_.rows || units > (limits_.native_name_bytes - native_bytes_) / unit_size) {
                fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "pane_listing.capacity"}}); return state_;
            }
            const auto display = entry.name.display_name();
            if (display.size() > limits_.display_name_bytes - display_bytes_) {
                fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "pane_listing.display_bytes"}}); return state_;
            }
            if (!names_.insert(entry.name).second) {
                fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "pane_listing.duplicate"}}); return state_;
            }
            encoding_ = entry.name.encoding(); native_bytes_ += units * unit_size; display_bytes_ += display.size();
            auto hidden_state = pane_hidden_state(entry.name, entry.metadata, model_);
            entries_.push_back(PaneEntry::from_listed(std::move(entry), std::move(hidden_state)));
        }
        if (batch.state == DirectoryReadState::End) {
            completed_ = Result(new const CompletedPaneListing(request_, directory_, std::move(entries_), directory_pin_));
            release_staging(); state_ = PaneListingJobState::Complete;
        } else state_ = batch.state == DirectoryReadState::Cancelled ? PaneListingJobState::Paused : PaneListingJobState::Collecting;
        return state_;
    } catch (...) {
        fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw;
    }
}
} // namespace ck::files
