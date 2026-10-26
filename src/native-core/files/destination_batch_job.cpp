// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_batch_job.hpp"
#include <stdexcept>

namespace ck::files {
namespace {
UnavailableObservation limit_error(const char* operation) {
    return {ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), operation}};
}
std::shared_ptr<const DirectoryCursor> first_parent(
    const std::vector<std::shared_ptr<const DirectoryCursor>>& parents) {
    for (const auto& parent : parents) if (parent) return parent;
    return {};
}
bool same_parent(const std::vector<std::shared_ptr<const DirectoryCursor>>& parents,
    std::size_t left, std::size_t right) noexcept {
    if (!parents[left] || !parents[right]) return false;
    const auto& left_identity = parents[left]->identity();
    const auto& right_identity = parents[right]->identity();
    return left_identity.value() && right_identity.value() &&
        compare_identity(*left_identity.value(), *right_identity.value()) == IdentityComparison::Same;
}
}
const DestinationEntryInspection* CompletedDestinationBatch::leader(std::size_t row) const noexcept {
    if (row >= entries_.size() || !entries_[row].exact_name_leader) return nullptr;
    return &entries_[*entries_[row].exact_name_leader];
}
std::size_t CompletedDestinationBatch::exact_name_count(std::size_t row) const noexcept {
    const auto* entry = leader(row); return entry ? entry->exact_name_count : 0;
}
const DirectoryEntryProbe* CompletedDestinationBatch::probe(std::size_t row) const noexcept {
    const auto* entry = leader(row); return entry ? entry->probe.get() : nullptr;
}
const UnavailableObservation* CompletedDestinationBatch::probe_failure(std::size_t row) const noexcept {
    const auto* entry = leader(row); return entry && entry->probe_failure ? &*entry->probe_failure : nullptr;
}
std::optional<std::size_t> CompletedDestinationBatch::reported_object_leader(std::size_t row) const noexcept {
    const auto* entry = leader(row); return entry ? entry->reported_object_leader : std::nullopt;
}
DestinationBatchJob::DestinationBatchJob(std::shared_ptr<const DirectoryCursor> directory, std::vector<NativeName> names,
    NameEncoding destination_encoding, DestinationBatchLimits limits, std::shared_ptr<const MonotonicClock> clock)
    : parent_directories_(names.size(), directory), root_directory_(directory), names_(std::move(names)), encoding_(destination_encoding),
      limits_(limits), clock_(std::move(clock)) {
    if (!directory || !clock_ || (encoding_ != NameEncoding::PosixBytes && encoding_ != NameEncoding::WindowsUtf16) ||
        limits.rows == 0 || limits.rows > DestinationBatchLimits::hard_rows || limits.name_bytes == 0 ||
        limits.name_bytes > DestinationBatchLimits::hard_name_bytes || limits.component_bytes == 0 ||
        limits.component_bytes > NameMappingLimits::hard_native_bytes || limits.time_slice <= MonotonicClock::Duration::zero() ||
        limits.time_slice > std::chrono::seconds(1)) throw std::invalid_argument("invalid destination batch job");
    if (names_.size() > limits.rows) fail(limit_error("destination_batch.rows"));
}
DestinationBatchJob::DestinationBatchJob(std::vector<std::shared_ptr<const DirectoryCursor>> parent_directories,
    std::vector<NativeName> names, NameEncoding destination_encoding, DestinationBatchLimits limits,
    std::shared_ptr<const MonotonicClock> clock)
    : parent_directories_(std::move(parent_directories)), root_directory_(first_parent(parent_directories_)),
      names_(std::move(names)), encoding_(destination_encoding),
      limits_(limits), clock_(std::move(clock)) {
    if (parent_directories_.size() != names_.size() || !clock_ || (encoding_ != NameEncoding::PosixBytes && encoding_ != NameEncoding::WindowsUtf16) ||
        limits.rows == 0 || limits.rows > DestinationBatchLimits::hard_rows || limits.name_bytes == 0 ||
        limits.name_bytes > DestinationBatchLimits::hard_name_bytes || limits.component_bytes == 0 ||
        limits.component_bytes > NameMappingLimits::hard_native_bytes || limits.time_slice <= MonotonicClock::Duration::zero() ||
        limits.time_slice > std::chrono::seconds(1)) throw std::invalid_argument("invalid destination batch job");
    if (names_.size() > limits.rows) fail(limit_error("destination_batch.rows"));
}
void DestinationBatchJob::release_staging() noexcept {
    std::vector<std::shared_ptr<const DirectoryCursor>>{}.swap(parent_directories_);
    root_directory_.reset();
    std::vector<NativeName>{}.swap(names_); std::vector<DestinationEntryInspection>{}.swap(entries_);
    names_index_.clear(); objects_.clear(); bucket_ = nullptr;
}
void DestinationBatchJob::fail(UnavailableObservation detail) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<UnavailableObservation>);
    static_assert(noexcept(UnavailableObservation{ObservationState::Failed, {ErrorDomain::Adapter, 0, {}}}));
    failure_.emplace(std::move(detail)); state_ = DestinationBatchState::Failed; release_staging();
}
bool DestinationBatchJob::abandon() noexcept {
    if (state_ == DestinationBatchState::Failed || state_ == DestinationBatchState::Abandoned) return false;
    completed_.reset(); release_staging(); state_ = DestinationBatchState::Abandoned; return true;
}
Observation<DestinationBatchJob::Result> DestinationBatchJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
bool DestinationBatchJob::retain_name(const NativeName& name) {
    const auto unit_bytes = name.windows_units() ? std::size_t{2} : std::size_t{1};
    const auto count = name.windows_units() ? name.windows_units()->size() : name.posix_bytes()->size();
    if (count > limits_.component_bytes / unit_bytes || count > (limits_.name_bytes - bytes_) / unit_bytes) {
        fail(limit_error("destination_batch.name_bytes")); return false;
    }
    bytes_ += count * unit_bytes; return true;
}
const ObjectIdentity* DestinationBatchJob::identity(std::size_t row) const noexcept {
    const auto& entry = entries_[row];
    if (!entry.probe || !entry.probe->existing_metadata().value()) return nullptr;
    return entry.probe->existing_metadata().value()->identity.value();
}
void DestinationBatchJob::step(std::stop_token stop) {
    switch (phase_) {
    case Phase::Map: {
        if (next_ == names_.size()) { std::vector<NativeName>{}.swap(names_); names_index_.clear(); next_ = 0; phase_ = Phase::Probe; break; }
        if (!retain_name(names_[next_])) return;
        auto mapped = map_native_name(names_[next_], encoding_, {limits_.component_bytes});
        DestinationEntryInspection entry(std::move(names_[next_]));
        if (mapped.value()) {
            if (!retain_name(*mapped.value())) return;
            std::optional<std::size_t> leader;
            if (parent_directories_[next_]) {
                const auto found = names_index_.find(*mapped.value());
                if (found != names_index_.end()) {
                    for (const auto candidate : found->second) {
                        if (same_parent(parent_directories_, candidate, next_)) {
                            leader = candidate;
                            break;
                        }
                    }
                }
            }
            if (!leader) {
                // Reserve once during mapping, never again on a paused retry.
                // Index key + probe requested name + lookup context name.
                for (int copy = 0; copy < 3; ++copy) if (!retain_name(*mapped.value())) return;
                if (parent_directories_[next_]) {
                    names_index_[*mapped.value()].push_back(next_);
                }
                entry.exact_name_leader = next_; entry.exact_name_count = 1;
            } else {
                entry.exact_name_leader = *leader;
                ++entries_[*leader].exact_name_count;
            }
        }
        entry.mapped = std::move(mapped); entries_.push_back(std::move(entry)); ++next_; break;
    }
    case Phase::Probe: {
        if (next_ == entries_.size()) { next_ = 0; phase_ = Phase::Group; break; }
        auto& entry = entries_[next_];
        if (entry.exact_name_leader && *entry.exact_name_leader == next_) {
            if (!parent_directories_[next_]) {
                entry.probe_failure = {ObservationState::Unknown,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable),
                     "destination_batch.parent_unavailable"}};
                ++next_;
                break;
            }
            auto probed = DirectoryEntryProbe::inspect(parent_directories_[next_], *entry.mapped.value(),
                ChildLookupMode::NativeDefault, {limits_.component_bytes}, stop);
            if (!probed.value() && stop.stop_requested()) { state_ = DestinationBatchState::Paused; return; }
            if (probed.value()) entry.probe = std::move(probed).take_value();
            else entry.probe_failure = *probed.unavailable_detail();
        }
        ++next_; break;
    }
    case Phase::Group: {
        if (next_ == entries_.size()) { phase_ = Phase::Seal; break; }
        const auto* id = identity(next_);
        if (!id || compare_identity(*id, *id) != IdentityComparison::Same) { ++next_; break; }
        const IdentityKey key{id->scheme, id->volume.bytes, id->object.bytes, id->generation};
        bucket_ = &objects_[key]; candidate_ = 0; phase_ = Phase::CompareGroup; break;
    }
    case Phase::CompareGroup:
        if (candidate_ == bucket_->size()) { bucket_->push_back(next_); ++next_; phase_ = Phase::Group; break; }
        if (compare_identity(*identity(next_), *identity((*bucket_)[candidate_])) == IdentityComparison::Same) {
            const auto first = (*bucket_)[candidate_];
            entries_[first].reported_object_leader = first; entries_[next_].reported_object_leader = first;
            ++next_; phase_ = Phase::Group;
        } else ++candidate_;
        break;
    case Phase::Seal: break;
    }
}
DestinationBatchState DestinationBatchJob::advance(std::stop_token stop) {
    if (state_ == DestinationBatchState::Complete || state_ == DestinationBatchState::Failed || state_ == DestinationBatchState::Abandoned) return state_;
    if (stop.stop_requested()) { state_ = DestinationBatchState::Paused; return state_; }
    try {
        CooperativeTimeSlice slice(*clock_, limits_.time_slice, last_reading_);
        for (std::size_t steps = 0; steps < steps_per_advance; ++steps) {
            if (stop.stop_requested()) { state_ = DestinationBatchState::Paused; return state_; }
            const auto expired = slice.expired(); last_reading_ = slice.last_reading();
            if (!expired.value()) { fail(*expired.unavailable_detail()); return state_; }
            state_ = DestinationBatchState::Collecting;
            if (*expired.value()) return state_;
            if (phase_ == Phase::Seal) {
                completed_ = Result(new const CompletedDestinationBatch(std::move(root_directory_),
                    std::move(parent_directories_), std::move(entries_)));
                release_staging(); state_ = DestinationBatchState::Complete; return state_;
            }
            step(stop); if (state_ != DestinationBatchState::Collecting) return state_;
        }
        return state_;
    } catch (...) {
        fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw;
    }
}
} // namespace ck::files
