// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "integrity_info.hpp"
#include <algorithm>
namespace ck::files {
namespace {
template<class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
template<class T> Observation<T> unavailable(const UnavailableObservation& value) {
    return Observation<T>::unavailable(value.state, value.error);
}
}
std::uint16_t WindowsIntegrityData::u16(std::size_t offset) const noexcept {
    return static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes_[offset]) |
        static_cast<std::uint16_t>(std::to_integer<std::uint16_t>(bytes_[offset + 1]) << 8));
}
std::uint32_t WindowsIntegrityData::u32(std::size_t offset) const noexcept {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes_[offset + i]) << (8 * i);
    return value;
}
Observation<WindowsIntegrityData> WindowsIntegrityData::decode(std::span<const std::byte> bytes) {
    if (bytes.size() != wire_bytes) return error<WindowsIntegrityData>(ObservationState::Failed, AdapterError::InvalidNativeResult, "integrity.reply_size");
    std::array<std::byte, wire_bytes> retained{}; std::copy(bytes.begin(), bytes.end(), retained.begin());
    return Observation<WindowsIntegrityData>::available(WindowsIntegrityData(retained));
}
Observation<ReportedIntegrityAlgorithm> reported_integrity_algorithm(const WindowsIntegrityData& data) {
    switch (data.checksum_algorithm()) {
    case 0: return Observation<ReportedIntegrityAlgorithm>::available(ReportedIntegrityAlgorithm::None);
    case 1: return Observation<ReportedIntegrityAlgorithm>::available(ReportedIntegrityAlgorithm::Crc32);
    case 2: return Observation<ReportedIntegrityAlgorithm>::available(ReportedIntegrityAlgorithm::Crc64);
    default: return error<ReportedIntegrityAlgorithm>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "integrity.algorithm_code");
    }
}
ReportedFlag reported_integrity_enforcement_off(const WindowsIntegrityData& data) noexcept {
    return (data.flags() & 1U) != 0 ? ReportedFlag::Set : ReportedFlag::Clear;
}
Observation<ObjectIdentity> detail::qualify_integrity_subject(const Observation<BasicMetadata>& before, const Observation<BasicMetadata>& after) {
    for (const auto* item : {&before, &after}) {
        if (!item->value()) return unavailable<ObjectIdentity>(*item->unavailable_detail());
        const auto& m = *item->value();
        if (!m.kind.value()) return unavailable<ObjectIdentity>(*m.kind.unavailable_detail());
        if (*m.kind.value() < ObjectKind::RegularFile || *m.kind.value() > ObjectKind::Other)
            return error<ObjectIdentity>(ObservationState::Failed, AdapterError::InvalidNativeResult, "integrity.kind");
        if (*m.kind.value() != ObjectKind::RegularFile && *m.kind.value() != ObjectKind::Directory)
            return error<ObjectIdentity>(ObservationState::Unsupported, AdapterError::NotApplicable, "integrity.kind");
        if (!m.windows_attributes.value()) return unavailable<ObjectIdentity>(*m.windows_attributes.unavailable_detail());
        if ((*m.windows_attributes.value() & 0x400U) != 0)
            return error<ObjectIdentity>(ObservationState::Unknown, AdapterError::ProviderReviewRequired, "integrity.reparse");
        const auto provider = provider_review_required(m, StorageModel::WindowsHandleAttributes);
        if (!provider.value()) return unavailable<ObjectIdentity>(*provider.unavailable_detail());
        if (*provider.value()) return error<ObjectIdentity>(ObservationState::Unknown, AdapterError::ProviderReviewRequired, "integrity.provider");
        if (!m.identity.value()) return unavailable<ObjectIdentity>(*m.identity.unavailable_detail());
        const auto scheme = m.identity.value()->scheme;
        if (scheme != IdentityScheme::WindowsFileId128 && scheme != IdentityScheme::WindowsFileIndex64)
            return error<ObjectIdentity>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "integrity.identity_scheme");
    }
    if (*before.value()->kind.value() != *after.value()->kind.value())
        return error<ObjectIdentity>(ObservationState::Changed, AdapterError::MetadataChanged, "integrity.kind_changed");
    if (((*before.value()->windows_attributes.value() ^ *after.value()->windows_attributes.value()) & 0x8000U) != 0)
        return error<ObjectIdentity>(ObservationState::Changed, AdapterError::MetadataChanged, "integrity.attribute_changed");
    const auto compared = compare_identity(*before.value()->identity.value(), *after.value()->identity.value());
    if (compared != IdentityComparison::Same) return error<ObjectIdentity>(
        compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
        AdapterError::UnqualifiedIdentity, "integrity.identity");
    return Observation<ObjectIdentity>::available(*after.value()->identity.value());
}
} // namespace ck::files
