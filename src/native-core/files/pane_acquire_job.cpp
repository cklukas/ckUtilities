// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_acquire_job.hpp"
#include <stdexcept>
#include <type_traits>

namespace ck::files {
namespace {
static_assert(std::is_nothrow_move_constructible_v<UnavailableObservation>);
UnavailableObservation error(ObservationState state, AdapterError code, const char* operation) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
bool valid_name(const NativeName& name, StorageModel model) {
    return name.is_valid() && (model == StorageModel::Unknown ||
        (model == StorageModel::WindowsHandleAttributes ? name.windows_units() != nullptr : name.posix_bytes() != nullptr));
}
std::optional<std::size_t> bounded_bytes(const NativeName& name, std::size_t remaining) {
    const auto unit = name.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    const auto units = name.posix_bytes() ? name.posix_bytes()->size() : name.windows_units()->size();
    if (units > remaining / unit || units > PaneAcquireJob::hard_component_bytes / unit) return {};
    return units * unit;
}
}
PaneAcquireJob::PaneAcquireJob(Result listing, std::vector<NativeName>&& requested, StorageModel model,
    PaneLimits limits, std::shared_ptr<const MonotonicClock> clock, MonotonicClock::Duration time_slice)
    : input_(std::move(listing)), requested_(std::move(requested)), model_(model), limits_(limits),
      clock_(std::move(clock)), time_slice_(time_slice) {
    validate_pane_limits(limits);
    if (!input_ || !input_->directory_pin_ || !clock_ || time_slice_ <= MonotonicClock::Duration::zero() || time_slice_ > std::chrono::seconds(1) ||
        (model != StorageModel::Unknown && model != StorageModel::LinuxStatx && model != StorageModel::DarwinStatFlags && model != StorageModel::WindowsHandleAttributes))
        throw std::invalid_argument("invalid pane acquisition preparation");
    if (requested_.size() > limits_.held_entries || input_->entries().size() > limits_.rows || input_->binding_issues().size() > limits_.held_entries)
        throw std::length_error("pane acquisition count limit");
    request_ = input_->request(); matches_.resize(requested_.size()); issues_.reserve(requested_.size() + input_->binding_issues().size());
}
void PaneAcquireJob::release_staging() noexcept {
    input_.reset(); std::vector<NativeName>{}.swap(requested_); std::vector<std::optional<std::size_t>>{}.swap(matches_);
    indexes_.clear(); std::vector<PaneEntry>{}.swap(entries_); std::vector<PaneBindingIssue>{}.swap(issues_);
    next_request_ = next_row_ = next_binding_ = next_issue_ = requested_bytes_ = native_bytes_ = display_bytes_ = issue_bytes_ = held_count_ = 0;
}
void PaneAcquireJob::fail(UnavailableObservation failure) noexcept {
    failure_.emplace(std::move(failure)); state_ = PaneListingJobState::Failed; release_staging();
}
bool PaneAcquireJob::abandon() noexcept {
    if (state_ == PaneListingJobState::Abandoned || state_ == PaneListingJobState::Failed) return false;
    completed_.reset(); release_staging(); state_ = PaneListingJobState::Abandoned; return true;
}
Observation<PaneAcquireJob::Result> PaneAcquireJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
PaneListingJobState PaneAcquireJob::advance(std::stop_token stop) {
    if (state_ == PaneListingJobState::Complete || state_ == PaneListingJobState::Failed || state_ == PaneListingJobState::Abandoned) return state_;
    if (stop.stop_requested()) { state_ = PaneListingJobState::Paused; return state_; }
    try {
        CooperativeTimeSlice slice(*clock_, time_slice_, last_reading_);
        std::size_t steps = 0;
        const auto yield = [&] {
            if (stop.stop_requested()) { state_ = PaneListingJobState::Paused; return true; }
            const auto expired = slice.expired(); last_reading_ = slice.last_reading();
            if (!expired.value()) { fail(*expired.unavailable_detail()); return true; }
            state_ = PaneListingJobState::Collecting;
            return *expired.value() || steps >= steps_per_advance;
        };
        // Validate all requests and rows before opening any child. Each copied
        // name is bounded first; request/index, row and issue storage have
        // separate bounded counts, not a claim of a total process-memory quota.
        while (next_request_ < requested_.size()) {
            if (yield()) return state_;
            const auto& name = requested_[next_request_];
            if (!valid_name(name, model_)) { fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "pane_acquire.request")); return state_; }
            const auto bytes = bounded_bytes(name, limits_.native_name_bytes - requested_bytes_);
            if (!bytes) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "pane_acquire.request_bytes")); return state_; }
            if (!indexes_.emplace(name, next_request_).second) { fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "pane_acquire.duplicate")); return state_; }
            requested_bytes_ += *bytes; ++next_request_; ++steps;
        }
        while (next_row_ < input_->entries().size()) {
            if (yield()) return state_;
            const auto& entry = input_->entries()[next_row_];
            if (!valid_name(entry.name, model_)) { fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "pane_acquire.discovery_row")); return state_; }
            const auto bytes = bounded_bytes(entry.name, limits_.native_name_bytes - native_bytes_);
            if (!bytes) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "pane_acquire.row_bytes")); return state_; }
            const auto display = entry.name.display_name();
            if (display.size() > limits_.display_name_bytes - display_bytes_) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "pane_acquire.display_bytes")); return state_; }
            const auto found = indexes_.find(entry.name);
            if (found != indexes_.end()) matches_[found->second] = next_row_;
            // Reserve the union, not the sum: requests for existing sealed
            // bindings consume no extra handle and must not reopen them.
            if (entry.held_child() || found != indexes_.end()) {
                if (held_count_ == limits_.held_entries) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "pane_acquire.held_limit")); return state_; }
                ++held_count_;
            }
            if (entry.held_child()) entries_.push_back(PaneEntry::from_held(entry.held_child(),
                pane_hidden_state(entry.name, Observation<BasicMetadata>::available(entry.held_child()->metadata()), model_)));
            else entries_.push_back(entry);
            native_bytes_ += *bytes; display_bytes_ += display.size(); ++next_row_; ++steps;
        }
        indexes_.clear();
        while (next_issue_ < input_->binding_issues().size()) {
            if (yield()) return state_;
            const auto& issue = input_->binding_issues()[next_issue_];
            if (!valid_name(issue.name, model_)) { fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "pane_acquire.issue_name")); return state_; }
            const auto bytes = bounded_bytes(issue.name, limits_.native_name_bytes - issue_bytes_);
            if (!bytes) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "pane_acquire.issue_bytes")); return state_; }
            issues_.push_back(issue); issue_bytes_ += *bytes; ++next_issue_; ++steps;
        }
        while (next_binding_ < requested_.size()) {
            if (yield()) return state_;
            const auto& name = requested_[next_binding_];
            if (!matches_[next_binding_]) issues_.push_back({name, error(ObservationState::Unknown, AdapterError::MetadataChanged, "pane_acquire.not_listed")});
            else if (!entries_[*matches_[next_binding_]].held_child()) {
                auto reply = HeldChild::observe(input_->directory_pin_, name, stop);
                if (!reply.value() && stop.stop_requested()) { state_ = PaneListingJobState::Paused; return state_; }
                if (!reply.value()) issues_.push_back({name, *reply.unavailable_detail()});
                else {
                    auto held = std::move(reply).take_value();
                    auto hidden = pane_hidden_state(name, Observation<BasicMetadata>::available(held->metadata()), model_);
                    entries_[*matches_[next_binding_]] = PaneEntry::from_held(std::move(held), std::move(hidden));
                }
            }
            ++next_binding_; ++steps;
        }
        if (yield()) return state_;
        completed_ = Result(new const CompletedPaneListing(request_, input_->directory(), std::move(entries_), {}, std::move(issues_)));
        release_staging(); state_ = PaneListingJobState::Complete; return state_;
    } catch (...) {
        fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw;
    }
}
} // namespace ck::files
