// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_inventory_job.hpp"
#include <stdexcept>

namespace ck::files {
namespace {
UnavailableObservation error(ObservationState state, AdapterError code, const char* operation) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
template<class T> Observation<T> unavailable(const UnavailableObservation& detail) {
    return Observation<T>::unavailable(detail.state, detail.error);
}
Observation<IdentityComparison> identities(const Observation<ObjectIdentity>& a, const Observation<ObjectIdentity>& b) {
    for (const auto* id : {&a, &b}) if (!id->value()) return unavailable<IdentityComparison>(*id->unavailable_detail());
    const auto compared = compare_identity(*a.value(), *b.value());
    if (compared == IdentityComparison::Unknown) return unavailable<IdentityComparison>(
        error(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "directory_inventory.identity"));
    return Observation<IdentityComparison>::available(compared);
}
Observation<IdentityComparison> continuity(const DirectoryInventoryData& data) {
    for (const auto* value : {&data.before, &data.after}) {
        if (!value->value()) return unavailable<IdentityComparison>(*value->unavailable_detail());
        if (!value->value()->kind.value()) return unavailable<IdentityComparison>(*value->value()->kind.unavailable_detail());
        if (*value->value()->kind.value() != ObjectKind::Directory) return unavailable<IdentityComparison>(
            error(ObservationState::Changed, AdapterError::NotDirectory, "directory_inventory.kind"));
    }
    const auto first = identities(data.anchor, data.before.value()->identity);
    if (!first.value() || *first.value() != IdentityComparison::Same) return first;
    return identities(data.before.value()->identity, data.after.value()->identity);
}
Observation<QualifiedDirectoryNameRule> name_rule(const DirectoryInventoryData& data) {
    auto result = detail::qualify_directory_name_rule(data.anchor, data.location_before, data.location_after);
    if (!result.value()) return result;
    // A bytewise plain-directory rule must not override an independently
    // observed policy on this same object, or an unqualified policy owner.
    const auto check = [&](const ObjectIdentity& owner, bool encrypted) -> std::optional<UnavailableObservation> {
        const auto compared = compare_identity(*data.anchor.value(), owner);
        if (compared != IdentityComparison::Same) return error(
            compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
            AdapterError::UnqualifiedIdentity, "directory_inventory.encryption_owner");
        if (encrypted) return error(ObservationState::Changed, AdapterError::MetadataChanged, "directory_inventory.encryption_disagreement");
        return {};
    };
    if (data.fscrypt_policy.value()) {
        const auto& value = *data.fscrypt_policy.value();
        if (const auto issue = check(value.directory, std::holds_alternative<FscryptPolicyData>(value.policy)))
            return unavailable<QualifiedDirectoryNameRule>(*issue);
    }
    if (data.fscrypt_key.value()) {
        if (const auto issue = check(data.fscrypt_key.value()->directory, true)) return unavailable<QualifiedDirectoryNameRule>(*issue);
    }
    return result;
}
class Budget final {
public:
    Budget(std::size_t limit, std::size_t& used) : limit_(limit), used_(used) {}
    bool bytes(std::size_t count) {
        if (count > limit_ - used_) return false;
        used_ += count; return true;
    }
    template<class T> bool observe(const Observation<T>& item) {
        return item.value() ? value(*item.value()) : bytes(item.unavailable_detail()->error.operation.size());
    }
private:
    template<class T> bool value(const T&) { return true; } // fixed-size payloads only
    bool value(const NativeFeatureIdentifier& id) {
        if (const auto* units = id.windows_units()) {
            if (units->size() > (limit_ - used_) / sizeof(char16_t)) return false;
            return bytes(units->size() * sizeof(char16_t));
        }
        return bytes(id.posix_bytes() ? id.posix_bytes()->size() : id.windows_ea_bytes()->size());
    }
    bool value(const BasicMetadata& m) {
        return observe(m.identity) && observe(m.kind) && observe(m.logical_size) && observe(m.allocated_size) &&
            observe(m.link_count) && observe(m.posix_uid) && observe(m.posix_gid) && observe(m.posix_permissions) &&
            observe(m.bsd_flags) && observe(m.windows_attributes) && observe(m.reparse_tag) && observe(m.linux_attributes) &&
            observe(m.mount_id) && observe(m.accessed) && observe(m.modified) && observe(m.changed) && observe(m.created);
    }
    bool value(const LocationInfo& m) {
        return observe(m.directory) && observe(m.filesystem_name) && observe(m.filesystem_type_code) && observe(m.filesystem_query_id) &&
            observe(m.volume_query_serial) && observe(m.features) && observe(m.native_mount_flags) && observe(m.read_only_mount) &&
            observe(m.case_sensitive_directory) && observe(m.case_preserving_names) && observe(m.windows_directory_case_flags) &&
            observe(m.linux_directory_inode_flags) && observe(m.posix_name_max) && observe(m.posix_name_chars_max) && observe(m.posix_path_max) && observe(m.posix_file_size_bits) && observe(m.posix_symlink_max) &&
            observe(m.windows_component_utf16_max) && observe(m.space_accounting_unit) && observe(m.total_bytes) && observe(m.free_bytes) && observe(m.available_bytes);
    }
    bool value(const DirectoryFscryptPolicy& m) {
        const auto* absent = std::get_if<FscryptPolicyAbsent>(&m.policy);
        return !absent || bytes(absent->witness.operation.size());
    }
    std::size_t limit_;
    std::size_t& used_;
};
bool valid_basic(const Observation<BasicMetadata>& result) {
    return !result.value() || !result.value()->kind.value() ||
        (*result.value()->kind.value() >= ObjectKind::RegularFile && *result.value()->kind.value() <= ObjectKind::Other);
}
bool valid_location(const Observation<LocationInfo>& result) {
    if (!result.value()) return true;
    const auto& value = *result.value();
    return value.family >= LocationQueryFamily::Unknown && value.family <= LocationQueryFamily::WindowsHandle &&
        (!value.features.value() || (value.features.value()->domain >= VolumeFeatureDomain::Unknown && value.features.value()->domain <= VolumeFeatureDomain::DarwinVolumeCapabilities));
}
}
Observation<IdentityComparison> CompletedDirectoryInventory::compare_owner(const Observation<ObjectIdentity>& owner) const {
    if (!data_->directory_continuity.value()) return unavailable<IdentityComparison>(*data_->directory_continuity.unavailable_detail());
    if (*data_->directory_continuity.value() != IdentityComparison::Same) return unavailable<IdentityComparison>(
        error(ObservationState::Changed, AdapterError::MetadataChanged, "directory_inventory.context_changed"));
    return identities(data_->anchor, owner);
}
DirectoryInventoryJob::DirectoryInventoryJob(std::shared_ptr<const DirectoryCursor> directory, DirectoryInventoryLimits limits,
    std::shared_ptr<const MonotonicClock> clock) : directory_(std::move(directory)), limits_(limits), clock_(std::move(clock)) {
    if (!directory_ || !clock_ || !limits.variable_bytes || limits.variable_bytes > DirectoryInventoryLimits::hard_variable_bytes ||
        limits.time_slice <= MonotonicClock::Duration::zero() || limits.time_slice > std::chrono::seconds(1))
        throw std::invalid_argument("invalid directory inventory job");
    data_ = std::make_unique<DirectoryInventoryData>();
}
void DirectoryInventoryJob::release_staging() noexcept { data_.reset(); directory_.reset(); }
void DirectoryInventoryJob::fail(UnavailableObservation detail) noexcept {
    failure_.emplace(std::move(detail)); state_ = DirectoryInventoryState::Failed; release_staging();
}
void DirectoryInventoryJob::invalid_reply() {
    fail(error(ObservationState::Failed, AdapterError::InvalidNativeResult, "directory_inventory.reply"));
}
bool DirectoryInventoryJob::abandon() noexcept {
    if (state_ == DirectoryInventoryState::Failed || state_ == DirectoryInventoryState::Abandoned) return false;
    completed_.reset(); release_staging(); state_ = DirectoryInventoryState::Abandoned; return true;
}
Observation<DirectoryInventoryJob::Result> DirectoryInventoryJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return unavailable<Result>(*failure_);
    return {};
}
template<class T> bool DirectoryInventoryJob::retain(Observation<T> result, Observation<T>& target) {
    Budget budget(limits_.variable_bytes, data_->variable_bytes);
    if (!budget.observe(result)) {
        fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "directory_inventory.variable_limit")); return false;
    }
    target = std::move(result); return true;
}
void DirectoryInventoryJob::step() {
    switch (phase_) {
    case Phase::Anchor: {
        Budget budget(limits_.variable_bytes, data_->variable_bytes);
        if (!budget.observe(directory_->identity())) { fail(error(ObservationState::Failed, AdapterError::InventoryLimit, "directory_inventory.variable_limit")); return; }
        data_->anchor = directory_->identity(); phase_ = Phase::Before; break;
    }
    case Phase::Before: case Phase::After: {
        auto reply = directory_->inspect_basic();
        if (!valid_basic(reply)) { invalid_reply(); return; }
        const bool before = phase_ == Phase::Before;
        if (!retain(std::move(reply), before ? data_->before : data_->after)) return;
        phase_ = before ? Phase::LocationBefore : Phase::Derive; break;
    }
    case Phase::LocationBefore: case Phase::LocationAfter: {
        auto reply = directory_->inspect_location();
        if (!valid_location(reply)) { invalid_reply(); return; }
        const bool before = phase_ == Phase::LocationBefore;
        if (!retain(std::move(reply), before ? data_->location_before : data_->location_after)) return;
        phase_ = before ? Phase::Access : Phase::After; break;
    }
    case Phase::Access: {
        const auto requested = inventory_directory_access[next_access_];
        auto reply = directory_->check_access(requested);
        if (reply.value() && (reply.value()->requested != requested || reply.value()->family < DirectoryAccessFamily::LinuxFaccessat2 ||
            reply.value()->family > DirectoryAccessFamily::WindowsOpenById)) { invalid_reply(); return; }
        if (!retain(std::move(reply), data_->access[next_access_])) return;
        if (++next_access_ == inventory_directory_access.size()) phase_ = Phase::Policy;
        break;
    }
    case Phase::Policy:
        if (retain(directory_->inspect_fscrypt_policy(), data_->fscrypt_policy)) phase_ = Phase::Key;
        break;
    case Phase::Key:
        if (retain(directory_->inspect_fscrypt_key_status(), data_->fscrypt_key)) phase_ = Phase::LocationAfter;
        break;
    case Phase::Derive:
        if (!retain(continuity(*data_), data_->directory_continuity)) return;
        if (!data_->directory_continuity.value()) {
            if (!retain(unavailable<QualifiedDirectoryNameRule>(*data_->directory_continuity.unavailable_detail()), data_->name_rule)) return;
        } else if (*data_->directory_continuity.value() != IdentityComparison::Same) {
            if (!retain(unavailable<QualifiedDirectoryNameRule>(error(ObservationState::Changed, AdapterError::MetadataChanged, "directory_inventory.context_changed")), data_->name_rule)) return;
        } else if (!retain(name_rule(*data_), data_->name_rule)) return;
        phase_ = Phase::Seal; break;
    case Phase::Seal: break;
    }
}
DirectoryInventoryState DirectoryInventoryJob::advance(std::stop_token stop) {
    if (state_ == DirectoryInventoryState::Complete || state_ == DirectoryInventoryState::Failed || state_ == DirectoryInventoryState::Abandoned) return state_;
    if (stop.stop_requested()) { state_ = DirectoryInventoryState::Paused; return state_; }
    try {
        CooperativeTimeSlice slice(*clock_, limits_.time_slice, last_reading_);
        for (std::size_t steps = 0; steps < steps_per_advance; ++steps) {
            if (stop.stop_requested()) { state_ = DirectoryInventoryState::Paused; return state_; }
            const auto expired = slice.expired(); last_reading_ = slice.last_reading();
            if (!expired.value()) { fail(*expired.unavailable_detail()); return state_; }
            state_ = DirectoryInventoryState::Collecting;
            if (*expired.value()) return state_;
            if (phase_ == Phase::Seal) {
                completed_ = Result(new const CompletedDirectoryInventory(std::move(directory_), std::move(data_)));
                release_staging(); state_ = DirectoryInventoryState::Complete; return state_;
            }
            step(); if (state_ != DirectoryInventoryState::Collecting) return state_;
        }
        return state_;
    } catch (...) {
        fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw;
    }
}
} // namespace ck::files
