// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "object_inventory_job.hpp"
#include <stdexcept>

namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char* operation) {
    return {ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
Observation<IdentityComparison> identities(const Observation<ObjectIdentity>& a, const Observation<ObjectIdentity>& b) {
    using Result = Observation<IdentityComparison>;
    for (const auto* id : {&a, &b}) if (!id->value()) {
        const auto& detail = *id->unavailable_detail(); return Result::unavailable(detail.state, detail.error);
    }
    const auto comparison = compare_identity(*a.value(), *b.value());
    if (comparison == IdentityComparison::Unknown) return Result::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "object_inventory.identity"});
    return Result::available(comparison);
}
std::optional<std::size_t> identifier_bytes(const NativeFeatureIdentifier& id, std::size_t budget) {
    if (const auto* units = id.windows_units()) {
        if (units->empty() || units->size() > budget / 2) return {};
        return units->size() * 2;
    }
    const auto* bytes = id.posix_bytes() ? id.posix_bytes() : id.windows_ea_bytes();
    if (!bytes || bytes->empty() || bytes->size() > budget) return {};
    return bytes->size();
}
}
Observation<IdentityComparison> CompletedObjectInventory::compare_owner(const Observation<ObjectIdentity>& owner) const {
    if (!data_->before.value()) {
        const auto& detail = *data_->before.unavailable_detail();
        return Observation<IdentityComparison>::unavailable(detail.state, detail.error);
    }
    return identities(data_->before.value()->identity, owner);
}
bool ObjectInventoryJob::FeatureLess::operator()(const NativeFeatureIdentifier& a, const NativeFeatureIdentifier& b) const noexcept {
    if (a.encoding() != b.encoding()) return a.encoding() < b.encoding();
    if (a.posix_bytes()) return *a.posix_bytes() < *b.posix_bytes();
    if (a.windows_units()) return *a.windows_units() < *b.windows_units();
    return *a.windows_ea_bytes() < *b.windows_ea_bytes();
}
ObjectInventoryJob::ObjectInventoryJob(std::unique_ptr<OpenedEntry> object, ObjectInventoryLimits limits,
    std::shared_ptr<const MonotonicClock> clock)
    : object_(std::move(object)), limits_(limits), clock_(std::move(clock)) {
    if (!object_ || !clock_ || !limits.query.valid() || !limits.retained.valid() ||
        limits.time_slice <= MonotonicClock::Duration::zero() || limits.time_slice > std::chrono::seconds(1))
        throw std::invalid_argument("invalid object inventory job");
    data_ = std::make_unique<ObjectInventoryData>();
}
void ObjectInventoryJob::release_staging() noexcept { data_.reset(); object_.reset(); identifiers_.clear(); }
void ObjectInventoryJob::fail(UnavailableObservation detail) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<UnavailableObservation>);
    static_assert(noexcept(UnavailableObservation{ObservationState::Failed, {ErrorDomain::Adapter, 0, {}}}));
    failure_.emplace(std::move(detail)); state_ = ObjectInventoryJobState::Failed; release_staging();
}
bool ObjectInventoryJob::abandon() noexcept {
    if (state_ == ObjectInventoryJobState::Failed || state_ == ObjectInventoryJobState::Abandoned) return false;
    completed_.reset(); release_staging(); state_ = ObjectInventoryJobState::Abandoned; return true;
}
Observation<ObjectInventoryJob::Result> ObjectInventoryJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return Observation<Result>::unavailable(failure_->state, failure_->error);
    return {};
}
bool ObjectInventoryJob::retain(std::size_t bytes, std::size_t records) {
    if (bytes > limits_.retained.max_native_bytes - bytes_ || records > limits_.retained.max_records - records_) {
        fail(error(AdapterError::InventoryLimit, "object_inventory.retained_limit")); return false;
    }
    bytes_ += bytes; records_ += records; return true;
}
bool ObjectInventoryJob::retain_identifier(const NativeFeatureIdentifier& identifier, std::size_t records) {
    const auto size = identifier_bytes(identifier, limits_.retained.max_native_bytes - bytes_);
    if (!size) { fail(error(AdapterError::InventoryLimit, "object_inventory.identifier_limit")); return false; }
    if (!retain(*size, records)) return false;
    return true;
}
bool ObjectInventoryJob::invalid_reply() { fail(error(AdapterError::InvalidNativeResult, "object_inventory.reply")); return false; }
void ObjectInventoryJob::step(std::stop_token stop) {
    switch (phase_) {
    case Phase::Before:
        if (!retain(0, 1)) return;
        data_->before = object_->inspect_basic(); phase_ = Phase::Storage; break;
    case Phase::Storage:
        if (!retain(0, 1)) return;
        data_->storage = object_->inspect_storage(); phase_ = Phase::Layout; break;
    case Phase::Layout: {
        if (records_ == limits_.retained.max_records) { fail(error(AdapterError::InventoryLimit, "object_inventory.retained_limit")); return; }
        auto reply = object_->inspect_data_layout(limits_.query, stop);
        if (!reply.value() && stop.stop_requested()) { state_ = ObjectInventoryJobState::Paused; return; }
        if (!retain(0, 1)) return;
        if (const auto* layout = reply.value(); layout && (layout->family == DataLayoutFamily::Unknown ||
            layout->family > DataLayoutFamily::WindowsAllocatedRanges || layout->family < DataLayoutFamily::Unknown ||
            layout->ranges.size() > limits_.query.max_records || layout->ranges.size() > limits_.query.max_native_bytes / sizeof(FileDataRange))) { invalid_reply(); return; }
        data_->layout = std::move(reply); phase_ = Phase::CheckLayout; next_ = 0; break;
    }
    case Phase::CheckLayout:
        if (const auto* layout = data_->layout.value(); layout && next_ < layout->ranges.size()) {
            const auto& range = layout->ranges[next_];
            if (range.length == 0 || range.offset >= layout->logical_size || range.length > layout->logical_size - range.offset ||
                (next_ != 0 && range.offset < layout->ranges[next_ - 1].offset + layout->ranges[next_ - 1].length)) { invalid_reply(); return; }
            if (!retain(sizeof(FileDataRange), 1)) return;
            ++next_; break;
        }
        phase_ = Phase::Link; break;
    case Phase::Link: {
        if (!retain(0, 1)) return;
        auto reply = object_->read_link_metadata(limits_.query);
        if (const auto* value = reply.value()) {
            if (const auto* posix = std::get_if<PosixSymlinkData>(&value->payload)) {
                if (posix->target.size() > limits_.query.max_native_bytes) { invalid_reply(); return; }
                if (!retain(posix->target.size(), 0)) return;
            } else if (const auto* windows = std::get_if<WindowsReparseData>(&value->payload)) {
                if (windows->raw.size() > limits_.query.max_native_bytes || windows->kind < WindowsReparseKind::OpaqueMicrosoft ||
                    windows->kind > WindowsReparseKind::MountPoint) { invalid_reply(); return; }
                if (!retain(windows->raw.size(), 0)) return;
                if (windows->names) for (const auto* name : {&windows->names->substitute, &windows->names->print}) {
                    if (name->size() > (limits_.retained.max_native_bytes - bytes_) / 2) {
                        fail(error(AdapterError::InventoryLimit, "object_inventory.link_names")); return;
                    }
                    if (!retain(name->size() * 2, 0)) return;
                }
            } else { invalid_reply(); return; }
        }
        data_->link = std::move(reply); phase_ = Phase::Channels; break;
    }
    case Phase::Channels:
        if (!retain(0, 1)) return;
        data_->channels = object_->inspect_data_channels(limits_.query);
        if (const auto* catalog = data_->channels.value(); catalog && (catalog->channels.size() > limits_.query.max_records ||
            catalog->coverage < DataChannelCoverage::Unknown || catalog->coverage > DataChannelCoverage::MacResourceFork)) { invalid_reply(); return; }
        phase_ = Phase::CheckChannels; next_ = 0; break;
    case Phase::CheckChannels:
        if (const auto* catalog = data_->channels.value(); catalog && next_ < catalog->channels.size()) {
            const auto& channel = catalog->channels[next_];
            if (channel.kind < DataChannelKind::MainData || channel.kind > DataChannelKind::Opaque) { invalid_reply(); return; }
            if (!retain_identifier(channel.identifier, 1)) return;
            if (!identifiers_.insert(channel.identifier).second) { invalid_reply(); return; }
            ++next_; break;
        }
        identifiers_.clear(); phase_ = Phase::Attributes; break;
    case Phase::Attributes:
        if (!retain(0, 1)) return;
        data_->attributes = object_->inspect_attributes(limits_.query);
        if (const auto* catalog = data_->attributes.value(); catalog && (catalog->attributes.size() > limits_.query.max_records ||
            catalog->coverage < AttributeCoverage::Unknown || catalog->coverage > AttributeCoverage::WindowsExtendedAttributes)) { invalid_reply(); return; }
        phase_ = Phase::CheckAttributes; next_ = 0; break;
    case Phase::CheckAttributes:
        if (const auto* catalog = data_->attributes.value(); catalog && next_ < catalog->attributes.size()) {
            const auto& attribute = catalog->attributes[next_];
            if (attribute.role < AttributeRole::Opaque || attribute.role > AttributeRole::InterpretationRequired) { invalid_reply(); return; }
            if (!retain_identifier(attribute.identifier, 1)) return;
            if (!identifiers_.insert(attribute.identifier).second) { invalid_reply(); return; }
            ++next_; break;
        }
        identifiers_.clear(); phase_ = Phase::Values; next_ = 0; break;
    case Phase::Values:
        if (const auto* catalog = data_->attributes.value(); catalog && next_ < catalog->attributes.size()) {
            if (!retain(0, 1)) return;
            const auto& identifier = catalog->attributes[next_].identifier;
            auto reply = object_->read_attribute(identifier, limits_.query);
            if (const auto* value = reply.value()) {
                if (value->identifier != identifier) {
                    fail({ObservationState::Changed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged),
                        "object_inventory.attribute_changed"}}); return;
                }
                if (value->bytes.size() > limits_.query.max_native_bytes) { invalid_reply(); return; }
                if (!retain_identifier(value->identifier, 0) || !retain(value->bytes.size(), 0)) return;
            }
            data_->attribute_values.push_back({next_, std::move(reply)}); ++next_; break;
        }
        phase_ = Phase::Security; next_ = 0; break;
    case Phase::Security:
        if (next_ < inventory_security_parts.size()) {
            if (!retain(0, 1)) return;
            auto reply = object_->read_security(inventory_security_parts[next_], limits_.query);
            if (const auto* value = reply.value()) {
                if (value->part != inventory_security_parts[next_] || value->bytes.size() > limits_.query.max_native_bytes ||
                    value->encoding < SecurityEncoding::Unknown || value->encoding > SecurityEncoding::WindowsRelativeDescriptor ||
                    value->presence < SecurityPresence::Unknown || value->presence > SecurityPresence::Present) { invalid_reply(); return; }
                if (!retain(value->bytes.size(), 0)) return;
            }
            data_->security[next_++] = std::move(reply); break;
        }
        phase_ = Phase::After; break;
    case Phase::After:
        if (!retain(0, 1)) return;
        data_->after = object_->inspect_basic();
        if (data_->before.value() && data_->after.value()) data_->identity_continuity = identities(data_->before.value()->identity, data_->after.value()->identity);
        else {
            const auto& detail = *(data_->before.value() ? data_->after.unavailable_detail() : data_->before.unavailable_detail());
            data_->identity_continuity = Observation<IdentityComparison>::unavailable(detail.state, detail.error);
        }
        phase_ = Phase::Seal; break;
    case Phase::Seal: break;
    }
}
ObjectInventoryJobState ObjectInventoryJob::advance(std::stop_token stop) {
    if (state_ == ObjectInventoryJobState::Complete || state_ == ObjectInventoryJobState::Failed || state_ == ObjectInventoryJobState::Abandoned) return state_;
    if (stop.stop_requested()) { state_ = ObjectInventoryJobState::Paused; return state_; }
    try {
        CooperativeTimeSlice slice(*clock_, limits_.time_slice, last_reading_);
        for (std::size_t steps = 0; steps < steps_per_advance; ++steps) {
            if (stop.stop_requested()) { state_ = ObjectInventoryJobState::Paused; return state_; }
            const auto expired = slice.expired(); last_reading_ = slice.last_reading();
            if (!expired.value()) { fail(*expired.unavailable_detail()); return state_; }
            state_ = ObjectInventoryJobState::Collecting;
            if (*expired.value()) return state_;
            if (phase_ == Phase::Seal) {
                completed_ = Result(new const CompletedObjectInventory(std::move(object_), std::move(data_)));
                release_staging(); state_ = ObjectInventoryJobState::Complete; return state_;
            }
            step(stop); if (state_ != ObjectInventoryJobState::Collecting) return state_;
        }
        return state_;
    } catch (...) {
        fail({ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), {}}}); throw;
    }
}
} // namespace ck::files
