// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_refresh_job.hpp"
#include <map>
#include <stdexcept>
#include <type_traits>

namespace ck::files {
namespace {
static_assert(noexcept(UnavailableObservation{ObservationState::Failed,
    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}));
static_assert(std::is_nothrow_move_constructible_v<UnavailableObservation>);
static_assert(std::is_nothrow_destructible_v<UnavailableObservation>);
UnavailableObservation error(ObservationState state, AdapterError code, const char* operation) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
std::optional<std::size_t> bounded_name_bytes(const NativeName& name, std::size_t remaining) {
    const auto unit = name.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    const auto units = name.posix_bytes() ? name.posix_bytes()->size() : name.windows_units()->size();
    if (units > remaining / unit || units > PaneRefreshJob::hard_component_bytes / unit) return {};
    return units * unit;
}
std::optional<UnavailableObservation> directory_difference(const Observation<ObjectIdentity>& a, const Observation<ObjectIdentity>& b) {
    if (!a.value()) return *a.unavailable_detail();
    if (!b.value()) return *b.unavailable_detail();
    const auto compared = compare_identity(*a.value(), *b.value());
    if (compared == IdentityComparison::Same) return {};
    return error(compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
        compared == IdentityComparison::Different ? AdapterError::MetadataChanged : AdapterError::UnqualifiedIdentity,
        "pane_refresh.directory_identity");
}
}
PaneRefreshJob::PaneRefreshJob(Result listing, std::span<const HeldChild::Pointer> previous, StorageModel model,
    PaneLimits limits, std::shared_ptr<const MonotonicClock> clock, MonotonicClock::Duration time_slice)
    : input_(std::move(listing)), model_(model), clock_(std::move(clock)), time_slice_(time_slice), limits_(limits) {
    validate_pane_limits(limits);
    if (!input_ || !input_->directory_pin_ || !clock_ || time_slice_ <= MonotonicClock::Duration::zero() || time_slice_ > std::chrono::seconds(1) ||
        (model != StorageModel::Unknown && model != StorageModel::LinuxStatx && model != StorageModel::DarwinStatFlags && model != StorageModel::WindowsHandleAttributes))
        throw std::invalid_argument("invalid pane refresh preparation");
    if (previous.size() > limits.held_entries || input_->entries().size() > limits.rows)
        throw std::length_error("pane refresh count limit");
    request_ = input_->request();
    previous_.assign(previous.begin(), previous.end());
    bindings_.reserve(previous.size()); issues_.reserve(previous.size());

}
void PaneRefreshJob::release_staging() noexcept {
    input_.reset(); std::vector<PaneEntry>{}.swap(entries_); std::vector<Binding>{}.swap(bindings_);
    std::vector<PaneBindingIssue>{}.swap(issues_); std::vector<HeldChild::Pointer>{}.swap(previous_); indexes_.clear();
    next_ = next_previous_ = next_row_ = previous_bytes_ = native_bytes_ = display_bytes_ = 0;
}
void PaneRefreshJob::fail(UnavailableObservation failure) noexcept {
    // optional::emplace can throw only from constructing its contained value;
    // this payload's move constructor/destructor are statically nonthrowing.
    failure_.emplace(std::move(failure)); state_ = PaneListingJobState::Failed; release_staging();
}
void PaneRefreshJob::issue(const NativeName& name, UnavailableObservation failure) { issues_.push_back({name, std::move(failure)}); }
bool PaneRefreshJob::abandon() noexcept {
    if (state_ == PaneListingJobState::Abandoned || state_ == PaneListingJobState::Failed) return false;
    completed_.reset(); release_staging(); state_ = PaneListingJobState::Abandoned; return true;
}
Observation<PaneRefreshJob::Result> PaneRefreshJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
PaneListingJobState PaneRefreshJob::advance(std::stop_token stop) {
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
        while (next_previous_ < previous_.size()) {
            if (yield()) return state_;
            const auto& held = previous_[next_previous_];
            if (!held) { fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "pane_refresh.previous")); return state_; }
            const auto count = bounded_name_bytes(held->name(), limits_.native_name_bytes - previous_bytes_);
            if (!count) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "pane_refresh.previous_bytes")); return state_; }
            if (!held->name().is_valid() || !indexes_.emplace(held->name(), bindings_.size()).second) {
                fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "pane_refresh.previous_name")); return state_;
            }
            previous_bytes_ += *count; bindings_.push_back({{}, held}); ++next_previous_; ++steps;
        }
        while (next_row_ < input_->entries().size()) {
            if (yield()) return state_;
            const auto& entry = input_->entries()[next_row_];
            const auto count = bounded_name_bytes(entry.name, limits_.native_name_bytes - native_bytes_);
            if (!count) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "pane_refresh.name_bytes")); return state_; }
            const bool windows = entry.name.windows_units() != nullptr;
            if (entry.held_child() || !entry.name.is_valid() || (model_ == StorageModel::WindowsHandleAttributes && !windows) ||
                ((model_ == StorageModel::LinuxStatx || model_ == StorageModel::DarwinStatFlags) && windows)) {
                fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "pane_refresh.discovery_row")); return state_;
            }
            const auto display = entry.name.display_name();
            if (display.size() > limits_.display_name_bytes - display_bytes_) {
                fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "pane_refresh.display_bytes")); return state_;
            }
            const auto found = indexes_.find(entry.name);
            if (found != indexes_.end()) bindings_[found->second].row = next_row_;
            entries_.push_back(entry); native_bytes_ += *count; display_bytes_ += display.size(); ++next_row_; ++steps;
        }
        indexes_.clear();
        while (next_ < bindings_.size()) {
            if (yield()) return state_;
            const auto& binding = bindings_[next_];
            if (!binding.row) issue(binding.previous->name(), error(ObservationState::Unknown, AdapterError::MetadataChanged, "pane_refresh.not_listed"));
            else if (const auto mismatch = directory_difference(input_->directory_pin_->identity(), binding.previous->parent_identity()))
                issue(binding.previous->name(), *mismatch);
            else {
                auto observed = binding.previous->refresh(stop);
                if (stop.stop_requested()) { state_ = PaneListingJobState::Paused; return state_; }
                if (!observed.value()) issue(binding.previous->name(), *observed.unavailable_detail());
                else {
                    auto fresh = std::move(observed).take_value();
                    if (!fresh->continues(*binding.previous)) {
                        const auto& continuity = fresh->previous_continuity();
                        issue(fresh->name(), continuity.value() ? error(ObservationState::Changed, AdapterError::MetadataChanged, "pane_refresh.replaced") : *continuity.unavailable_detail());
                    }
                    auto metadata = Observation<BasicMetadata>::available(fresh->metadata());
                    auto hidden = pane_hidden_state(fresh->name(), metadata, model_);
                    entries_[*binding.row] = PaneEntry::from_held(std::move(fresh), std::move(hidden));
                }
            }
            ++next_; ++steps;
            if (yield()) return state_;
        }
        if (yield()) return state_;
        completed_ = Result(new const CompletedPaneListing(request_, input_->directory(), std::move(entries_), {}, std::move(issues_)));
        release_staging(); state_ = PaneListingJobState::Complete; return state_;
    } catch (...) {
        // Do not allocate a diagnostic string while handling allocation failure.
        fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw;
    }
}
} // namespace ck::files
