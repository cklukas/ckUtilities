// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "inode_flags.hpp"
namespace ck::files {
namespace {
template<class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
template<class T> Observation<T> unavailable(const UnavailableObservation& e) { return Observation<T>::unavailable(e.state,e.error); }
}
Observation<ReportedFlag> reported_inode_flag(const FileInodeFlags& info, LinuxInodeFlag flag) {
    if (info.family != InodeFlagQueryFamily::LinuxIoctlGetFlags)
        return error<ReportedFlag>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "inode_flags.family");
    if (info.kind != ObjectKind::RegularFile) return error<ReportedFlag>(ObservationState::Failed, AdapterError::InvalidNativeResult, "inode_flags.kind");
    std::uint32_t bit = 0;
    switch (flag) {
    case LinuxInodeFlag::Compression: bit = 0x4U; break;
    case LinuxInodeFlag::Synchronous: bit = 0x8U; break;
    case LinuxInodeFlag::Immutable: bit = 0x10U; break;
    case LinuxInodeFlag::AppendOnly: bit = 0x20U; break;
    case LinuxInodeFlag::NoDump: bit = 0x40U; break;
    case LinuxInodeFlag::NoAtime: bit = 0x80U; break;
    case LinuxInodeFlag::NoCompression: bit = 0x400U; break;
    case LinuxInodeFlag::Encryption: bit = 0x800U; break;
    case LinuxInodeFlag::JournalData: bit = 0x4000U; break;
    case LinuxInodeFlag::DirectorySync: bit = 0x10000U; break;
    case LinuxInodeFlag::Verity: bit = 0x100000U; break;
    case LinuxInodeFlag::NoCopyOnWrite: bit = 0x800000U; break;
    case LinuxInodeFlag::Dax: bit = 0x2000000U; break;
    case LinuxInodeFlag::ProjectInherit: bit = 0x20000000U; break;
    case LinuxInodeFlag::Casefold: bit = 0x40000000U; break;
    default: return error<ReportedFlag>(ObservationState::Failed, AdapterError::InvalidIdentifier, "inode_flags.flag");
    }
    return Observation<ReportedFlag>::available((info.raw & bit) != 0 ? ReportedFlag::Set : ReportedFlag::Clear);
}
Observation<ObjectIdentity> detail::qualify_inode_flags_subject(const Observation<BasicMetadata>& before, const Observation<BasicMetadata>& after) {
    for (const auto* endpoint : {&before,&after}) {
        if (!endpoint->value()) return unavailable<ObjectIdentity>(*endpoint->unavailable_detail());
        const auto& kind = endpoint->value()->kind;
        if (!kind.value()) return unavailable<ObjectIdentity>(*kind.unavailable_detail());
        if (*kind.value() < ObjectKind::RegularFile || *kind.value() > ObjectKind::Other)
            return error<ObjectIdentity>(ObservationState::Failed, AdapterError::InvalidNativeResult, "inode_flags.kind");
    }
    if (*before.value()->kind.value() != *after.value()->kind.value())
        return error<ObjectIdentity>(ObservationState::Changed, AdapterError::MetadataChanged, "inode_flags.kind_changed");
    if (*before.value()->kind.value() != ObjectKind::RegularFile)
        return error<ObjectIdentity>(ObservationState::Unsupported, AdapterError::NotApplicable, "inode_flags.regular_file_only");
    for (const auto* endpoint : {&before,&after}) {
        const auto& id = endpoint->value()->identity;
        if (!id.value()) return unavailable<ObjectIdentity>(*id.unavailable_detail());
        if (id.value()->scheme != IdentityScheme::PosixDeviceInode)
            return error<ObjectIdentity>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "inode_flags.identity_scheme");
    }
    const auto compared = compare_identity(*before.value()->identity.value(),*after.value()->identity.value());
    if (compared != IdentityComparison::Same) return error<ObjectIdentity>(
        compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
        AdapterError::UnqualifiedIdentity, "inode_flags.identity");
    return Observation<ObjectIdentity>::available(*after.value()->identity.value());
}
} // namespace ck::files
