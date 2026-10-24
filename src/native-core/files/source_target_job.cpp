// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_target_job.hpp"
#include <stdexcept>
#include <type_traits>

namespace ck::files {
namespace {
static_assert(std::is_nothrow_move_constructible_v<UnavailableObservation>);
UnavailableObservation error(ObservationState state, AdapterError code, const char* operation) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
template<class T> Observation<T> unavailable(const UnavailableObservation& failure) {
    return Observation<T>::unavailable(failure.state, failure.error);
}
std::optional<UnavailableObservation> identity_change(const BasicMetadata& expected, const Observation<BasicMetadata>& now) {
    if (!now.value()) return *now.unavailable_detail();
    for (const auto* metadata : {&expected, now.value()}) {
        if (!metadata->identity.value()) return *metadata->identity.unavailable_detail();
        if (!metadata->kind.value()) return *metadata->kind.unavailable_detail();
        if (*metadata->kind.value() < ObjectKind::RegularFile || *metadata->kind.value() > ObjectKind::Other)
            return error(ObservationState::Failed, AdapterError::InvalidNativeResult, "source_target.kind");
    }
    const auto compared = compare_identity(*expected.identity.value(), *now.value()->identity.value());
    if (compared == IdentityComparison::Unknown)
        return error(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "source_target.identity");
    if (compared != IdentityComparison::Same || *expected.kind.value() != *now.value()->kind.value())
        return error(ObservationState::Changed, AdapterError::MetadataChanged, "source_target.changed");
    return {};
}
bool bounded(const NativeName& name, std::size_t bytes) {
    const auto unit = name.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    return (name.posix_bytes() ? name.posix_bytes()->size() : name.windows_units()->size()) <= bytes / unit;
}
}
SourceTargetJob::SourceTargetJob(std::vector<HeldChild::Pointer>&& sources, DestinationBatchJob::Result destination,
    SourceTargetLimits limits, std::shared_ptr<const MonotonicClock> clock)
    : sources_(std::move(sources)), destination_(std::move(destination)), limits_(limits), clock_(std::move(clock)) {
    if (!destination_ || !clock_ || limits.rows == 0 || limits.rows > DestinationBatchLimits::hard_rows ||
        limits.component_bytes == 0 || limits.component_bytes > hard_component_bytes ||
        limits.time_slice <= MonotonicClock::Duration::zero() || limits.time_slice > std::chrono::seconds(1))
        throw std::invalid_argument("invalid source target inspection");
    if (sources_.size() != destination_->entries().size()) throw std::invalid_argument("source target count mismatch");
    if (sources_.size() > limits.rows) throw std::length_error("source target row limit");
    entries_.reserve(sources_.size());
}
void SourceTargetJob::release_staging() noexcept {
    std::vector<HeldChild::Pointer>{}.swap(sources_); destination_.reset(); std::vector<SourceTargetInspection>{}.swap(entries_);
    next_ = next_validation_ = 0;
}
void SourceTargetJob::fail(UnavailableObservation failure) noexcept {
    failure_.emplace(std::move(failure)); state_ = SourceTargetState::Failed; release_staging();
}
bool SourceTargetJob::abandon() noexcept {
    if (state_ == SourceTargetState::Failed || state_ == SourceTargetState::Abandoned) return false;
    completed_.reset(); release_staging(); state_ = SourceTargetState::Abandoned; return true;
}
Observation<SourceTargetJob::Result> SourceTargetJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return unavailable<Result>(*failure_);
    return {};
}
void SourceTargetJob::step() {
    if (phase_ == Phase::Validate) {
        if (next_validation_ == sources_.size()) { phase_ = Phase::Source; return; }
        const auto& source = sources_[next_validation_]; const auto& expected = destination_->entries()[next_validation_].source;
        if (!source || !source->name().is_valid() || !expected.is_valid()) { fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "source_target.source")); return; }
        if (!bounded(source->name(), limits_.component_bytes) || !bounded(expected, limits_.component_bytes)) { fail(error(ObservationState::Unknown, AdapterError::InventoryLimit, "source_target.name_limit")); return; }
        if (source->name() != expected) { fail(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "source_target.source_name")); return; }
        entries_.emplace_back(); ++next_validation_; return;
    }
    auto& row = entries_[next_];
    const auto* probe = destination_->probe(next_);
    switch (phase_) {
    case Phase::Validate: break;
    case Phase::Source:
        row.source_now = sources_[next_]->inspect_basic(); phase_ = Phase::Target; break;
    case Phase::Target: {
        std::optional<UnavailableObservation> failure;
        const auto& mapped = destination_->entries()[next_].mapped;
        if (!mapped.value()) failure = *mapped.unavailable_detail();
        else if (const auto* failed = destination_->probe_failure(next_)) failure = *failed;
        else if (!probe) failure = error(ObservationState::Failed, AdapterError::InvalidNativeResult, "source_target.probe");
        else if (!probe->lookup().value()) failure = *probe->lookup().unavailable_detail();
        else if (probe->lookup().value()->state() == ChildLookupState::Missing)
            failure = error(ObservationState::Unsupported, AdapterError::NotApplicable, "source_target.no_existing_target");
        else if (!probe->lookup().value()->entry()) failure = error(ObservationState::Failed, AdapterError::InvalidNativeResult, "source_target.target");
        if (failure) { row.target_now = unavailable<BasicMetadata>(*failure); row.comparison = unavailable<IdentityComparison>(*failure); ++next_; phase_ = Phase::Source; }
        else { row.target_now = probe->lookup().value()->entry()->inspect_basic(); phase_ = Phase::Compare; }
        break;
    }
    case Phase::Compare: {
        auto failure = identity_change(sources_[next_]->metadata(), row.source_now);
        if (!failure) failure = probe->existing_metadata().value() ? identity_change(*probe->existing_metadata().value(), row.target_now) :
            std::optional<UnavailableObservation>{*probe->existing_metadata().unavailable_detail()};
        if (failure) row.comparison = unavailable<IdentityComparison>(*failure);
        else {
            const auto compared = compare_identity(*row.source_now.value()->identity.value(), *row.target_now.value()->identity.value());
            if (compared == IdentityComparison::Same && *row.source_now.value()->kind.value() != *row.target_now.value()->kind.value())
                row.comparison = unavailable<IdentityComparison>(error(ObservationState::Changed, AdapterError::MetadataChanged, "source_target.kind_conflict"));
            else row.comparison = compared == IdentityComparison::Unknown ? unavailable<IdentityComparison>(error(ObservationState::Unknown,
                AdapterError::UnqualifiedIdentity, "source_target.comparison")) : Observation<IdentityComparison>::available(compared);
        }
        ++next_; phase_ = Phase::Source; break;
    }
    }
}
SourceTargetState SourceTargetJob::advance(std::stop_token stop) {
    if (state_ == SourceTargetState::Complete || state_ == SourceTargetState::Failed || state_ == SourceTargetState::Abandoned) return state_;
    if (stop.stop_requested()) { state_ = SourceTargetState::Paused; return state_; }
    try {
        CooperativeTimeSlice slice(*clock_, limits_.time_slice, last_reading_);
        state_ = SourceTargetState::Collecting;
        for (std::size_t steps = 0; steps < steps_per_advance; ++steps) {
            if (stop.stop_requested()) { state_ = SourceTargetState::Paused; return state_; }
            const auto expired = slice.expired(); last_reading_ = slice.last_reading();
            if (!expired.value()) { fail(*expired.unavailable_detail()); return state_; }
            if (*expired.value()) return state_;
            if (phase_ != Phase::Validate && next_ == sources_.size()) {
                completed_ = Result(new const CompletedSourceTargets(std::move(sources_), std::move(destination_), std::move(entries_)));
                release_staging(); state_ = SourceTargetState::Complete; return state_;
            }
            step(); if (state_ == SourceTargetState::Failed) return state_;
        }
        return state_;
    } catch (...) { fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw; }
}
} // namespace ck::files
