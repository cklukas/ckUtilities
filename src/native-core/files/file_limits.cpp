// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_limits.hpp"
#include <bit>
namespace ck::files {
namespace {
template<class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
template<class T> Observation<T> unavailable(const UnavailableObservation& value) {
    return Observation<T>::unavailable(value.state, value.error);
}
template<class T> std::optional<Observation<T>> family_error(LocationQueryFamily family) {
    switch (family) {
    case LocationQueryFamily::LinuxDescriptor: case LocationQueryFamily::DarwinDescriptor: return {};
    case LocationQueryFamily::WindowsHandle: return error<T>(ObservationState::Unsupported, AdapterError::NotApplicable, "file_limits.posix_family");
    case LocationQueryFamily::Unknown: return error<T>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "file_limits.family");
    default: return error<T>(ObservationState::Failed, AdapterError::InvalidIdentifier, "file_limits.family");
    }
}
}
Observation<FileSizeBitAssessment> assess_file_size_bits(std::uint64_t bytes, const LocationInfo& location) {
    if (const auto issue = family_error<FileSizeBitAssessment>(location.family)) return *issue;
    if (!location.posix_file_size_bits.value()) return unavailable<FileSizeBitAssessment>(*location.posix_file_size_bits.unavailable_detail());
    const auto bits = *location.posix_file_size_bits.value();
    if (bits < 32) return error<FileSizeBitAssessment>(ObservationState::Failed, AdapterError::InvalidNativeResult, "file_limits.file_size_bits");
    return Observation<FileSizeBitAssessment>::available({location.directory, bytes, bits, static_cast<unsigned>(std::bit_width(bytes)) + 1U});
}
Observation<SymlinkLengthAssessment> assess_posix_symlink_length(std::uint64_t bytes, const LocationInfo& location) {
    if (const auto issue = family_error<SymlinkLengthAssessment>(location.family)) return *issue;
    if (!location.posix_symlink_max.value()) return unavailable<SymlinkLengthAssessment>(*location.posix_symlink_max.unavailable_detail());
    if (*location.posix_symlink_max.value() == 0) return error<SymlinkLengthAssessment>(ObservationState::Failed, AdapterError::InvalidNativeResult, "file_limits.symlink_max");
    return Observation<SymlinkLengthAssessment>::available({location.directory, bytes, *location.posix_symlink_max.value()});
}
Observation<HardLinkCountAssessment> assess_hard_link_total(std::uint64_t total, const FileLinkLimits& limits) {
    if (limits.family != FileLinkLimitFamily::PosixFpathconf) return error<HardLinkCountAssessment>(
        limits.family == FileLinkLimitFamily::Unknown ? ObservationState::Unknown : ObservationState::Failed,
        AdapterError::UnsupportedQuery, "file_limits.link_family");
    if (!limits.maximum_links.value()) return unavailable<HardLinkCountAssessment>(*limits.maximum_links.unavailable_detail());
    if (*limits.maximum_links.value() == 0) return error<HardLinkCountAssessment>(ObservationState::Failed, AdapterError::InvalidNativeResult, "file_limits.link_max");
    if (limits.kind < ObjectKind::RegularFile || limits.kind > ObjectKind::Other) return error<HardLinkCountAssessment>(
        ObservationState::Failed, AdapterError::InvalidNativeResult, "file_limits.link_kind");
    return Observation<HardLinkCountAssessment>::available({limits.owner, limits.kind, total, *limits.maximum_links.value()});
}
Observation<std::uint64_t> detail::decode_posix_limit(std::int64_t result, ObservationState error_state, NativeError native) {
    if (result == -1) return Observation<std::uint64_t>::unavailable(native.code == 0 ? ObservationState::Unknown : error_state, std::move(native));
    if (result <= 0) return Observation<std::uint64_t>::unavailable(result == 0 ? ObservationState::Unknown : ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), std::move(native.operation)});
    return Observation<std::uint64_t>::available(static_cast<std::uint64_t>(result));
}
Observation<ObjectIdentity> detail::qualify_file_link_subject(const Observation<BasicMetadata>& before, const Observation<BasicMetadata>& after) {
    for (const auto* item : {&before, &after}) {
        if (!item->value()) return unavailable<ObjectIdentity>(*item->unavailable_detail());
        const auto& value = *item->value();
        if (!value.kind.value()) return unavailable<ObjectIdentity>(*value.kind.unavailable_detail());
        if (*value.kind.value() < ObjectKind::RegularFile || *value.kind.value() > ObjectKind::Other)
            return error<ObjectIdentity>(ObservationState::Failed, AdapterError::InvalidNativeResult, "file_limits.subject_kind");
        if (!value.identity.value()) return unavailable<ObjectIdentity>(*value.identity.unavailable_detail());
        if (value.identity.value()->scheme != IdentityScheme::PosixDeviceInode)
            return error<ObjectIdentity>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "file_limits.identity_scheme");
    }
    if (*before.value()->kind.value() != *after.value()->kind.value())
        return error<ObjectIdentity>(ObservationState::Changed, AdapterError::MetadataChanged, "file_limits.subject_kind_changed");
    const auto compared = compare_identity(*before.value()->identity.value(), *after.value()->identity.value());
    if (compared != IdentityComparison::Same) return error<ObjectIdentity>(
        compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
        AdapterError::UnqualifiedIdentity, "file_limits.subject_identity");
    return Observation<ObjectIdentity>::available(*after.value()->identity.value());
}
} // namespace ck::files
