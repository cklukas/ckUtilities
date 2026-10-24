// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_names_job.hpp"
#include <stdexcept>
namespace ck::files {
namespace {
UnavailableObservation error(ObservationState state, AdapterError code, const char* operation) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
template<class T> Observation<T> unavailable(const UnavailableObservation& detail) {
    return Observation<T>::unavailable(detail.state, detail.error);
}
}
Observation<DestinationNameRow> CompletedDestinationNames::row(std::size_t index) const {
    if (index >= batch_->entries().size()) return unavailable<DestinationNameRow>(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "destination_names.row"));
    const auto& entry = batch_->entries()[index];
    if (!entry.mapped.value()) return Observation<DestinationNameRow>::available({{}, 0, 0, unavailable<OtherNameGroups>(*entry.mapped.unavailable_detail())});
    return Observation<DestinationNameRow>::available({entry.exact_name_leader, batch_->exact_name_count(index), summary_.groups - 1, other_groups_});
}
Observation<ComponentNameComparison> CompletedDestinationNames::compare(std::size_t left, std::size_t right) const {
    using Reply = Observation<ComponentNameComparison>;
    if (left >= batch_->entries().size() || right >= batch_->entries().size())
        return unavailable<ComponentNameComparison>(error(ObservationState::Failed, AdapterError::InvalidIdentifier, "destination_names.pair"));
    const auto& a = batch_->entries()[left]; const auto& b = batch_->entries()[right];
    if (!a.mapped.value()) return unavailable<ComponentNameComparison>(*a.mapped.unavailable_detail());
    if (!b.mapped.value()) return unavailable<ComponentNameComparison>(*b.mapped.unavailable_detail());
    if (a.exact_name_leader == b.exact_name_leader) {
        const auto context = before_.value() ? before_.value()->directory : unavailable<ObjectIdentity>(*before_.unavailable_detail());
        return Reply::available({ComponentNameRelation::IdenticalNativeUnits, context});
    }
    if (!other_groups_.value()) return unavailable<ComponentNameComparison>(*other_groups_.unavailable_detail());
    if (*other_groups_.value() != OtherNameGroups::DistinctUnderReportedRules || !rule_.value())
        return unavailable<ComponentNameComparison>(error(ObservationState::Failed, AdapterError::InvalidNativeResult, "destination_names.coverage"));
    return Reply::available({ComponentNameRelation::DistinctUnderReportedRules, Observation<ObjectIdentity>::available(rule_.value()->directory)});
}
DestinationNamesJob::DestinationNamesJob(DestinationBatchJob::Result batch, DestinationNamesLimits limits,
    std::shared_ptr<const MonotonicClock> clock) : batch_(std::move(batch)), limits_(limits), clock_(std::move(clock)) {
    if (!batch_ || !clock_ || limits.rows == 0 || limits.rows > DestinationBatchLimits::hard_rows ||
        limits.component_bytes == 0 || limits.component_bytes > NameMappingLimits::hard_native_bytes ||
        limits.time_slice <= MonotonicClock::Duration::zero() || limits.time_slice > std::chrono::seconds(1))
        throw std::invalid_argument("invalid destination name assessment");
    if (batch_->entries().size() > limits.rows) fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "destination_names.rows"));
}
void DestinationNamesJob::release_staging() noexcept {
    batch_.reset(); summary_ = {}; encoding_.reset(); before_ = {}; after_ = {}; rule_ = {}; other_groups_ = {};
}
void DestinationNamesJob::fail(UnavailableObservation detail) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<UnavailableObservation>);
    failure_.emplace(std::move(detail)); state_ = DestinationNamesState::Failed; release_staging();
}
bool DestinationNamesJob::abandon() noexcept {
    if (state_ == DestinationNamesState::Failed || state_ == DestinationNamesState::Abandoned) return false;
    completed_.reset(); release_staging(); state_ = DestinationNamesState::Abandoned; return true;
}
Observation<DestinationNamesJob::Result> DestinationNamesJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return unavailable<Result>(*failure_);
    return {};
}
void DestinationNamesJob::set_coverage() {
    if (!rule_.value()) other_groups_ = unavailable<OtherNameGroups>(*rule_.unavailable_detail());
    else if (rule_.value()->rule != DirectoryNameRule::LinuxExtendedFsBytewise || encoding_ != NameEncoding::PosixBytes)
        other_groups_ = unavailable<OtherNameGroups>(error(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "destination_names.rule_encoding"));
    else other_groups_ = Observation<OtherNameGroups>::available(OtherNameGroups::DistinctUnderReportedRules);
    phase_ = Phase::Seal;
}
void DestinationNamesJob::step() {
    switch (phase_) {
    case Phase::Validate: {
        if (next_ == batch_->entries().size()) {
            const auto not_attempted = error(ObservationState::Unknown, AdapterError::NotApplicable, "destination_names.not_attempted");
            before_ = unavailable<LocationInfo>(not_attempted); after_ = unavailable<LocationInfo>(not_attempted);
            if (summary_.groups <= 1) {
                rule_ = unavailable<QualifiedDirectoryNameRule>(error(ObservationState::Unsupported, AdapterError::NotApplicable, "destination_names.no_other_groups"));
                other_groups_ = Observation<OtherNameGroups>::available(OtherNameGroups::None); phase_ = Phase::Seal;
            } else phase_ = Phase::Before;
            break;
        }
        const auto& entry = batch_->entries()[next_];
        if (!entry.mapped.value()) { ++summary_.unmapped_rows; ++next_; break; }
        const auto& name = *entry.mapped.value();
        const auto units = name.posix_bytes() ? name.posix_bytes()->size() : name.windows_units()->size();
        const auto unit_bytes = name.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
        if (units > limits_.component_bytes / unit_bytes) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "destination_names.component_bytes")); break; }
        const auto leader = entry.exact_name_leader; const auto count = batch_->exact_name_count(next_);
        if (!leader || *leader > next_ || count == 0 || count > batch_->entries().size() || (encoding_ && *encoding_ != name.encoding())) {
            fail(error(ObservationState::Failed, AdapterError::InvalidNativeResult, "destination_names.sealed_group")); break;
        }
        encoding_ = name.encoding(); ++summary_.mapped_rows;
        if (*leader == next_) {
            ++summary_.groups;
            if (count > 1) { ++summary_.duplicate_groups; summary_.rows_in_duplicate_groups += count; }
        }
        ++next_; break;
    }
    case Phase::Before: before_ = batch_->pin_directory()->inspect_location(); phase_ = Phase::CheckBefore; break;
    case Phase::CheckBefore:
        rule_ = detail::qualify_directory_name_rule(batch_->pin_directory()->identity(), before_, before_);
        if (!rule_.value()) set_coverage(); else phase_ = Phase::After;
        break;
    case Phase::After: after_ = batch_->pin_directory()->inspect_location(); phase_ = Phase::CheckAfter; break;
    case Phase::CheckAfter:
        rule_ = detail::qualify_directory_name_rule(batch_->pin_directory()->identity(), before_, after_); set_coverage(); break;
    case Phase::Seal:
        completed_ = Result(new const CompletedDestinationNames(std::move(batch_), summary_, std::move(before_), std::move(after_), std::move(rule_), std::move(other_groups_)));
        release_staging(); state_ = DestinationNamesState::Complete; break;
    }
}
DestinationNamesState DestinationNamesJob::advance(std::stop_token stop) {
    if (state_ == DestinationNamesState::Complete || state_ == DestinationNamesState::Failed || state_ == DestinationNamesState::Abandoned) return state_;
    if (stop.stop_requested()) { state_ = DestinationNamesState::Paused; return state_; }
    try {
        CooperativeTimeSlice slice(*clock_, limits_.time_slice, last_reading_); state_ = DestinationNamesState::Collecting;
        for (std::size_t count = 0; count < steps_per_advance; ++count) {
            if (stop.stop_requested()) { state_ = DestinationNamesState::Paused; return state_; }
            const auto expired = slice.expired(); last_reading_ = slice.last_reading();
            if (!expired.value()) { fail(*expired.unavailable_detail()); return state_; }
            if (*expired.value()) return state_;
            step(); if (state_ != DestinationNamesState::Collecting) return state_;
        }
        return state_;
    } catch (...) { fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw; }
}
} // namespace ck::files
