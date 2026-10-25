// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "location_relation.hpp"
namespace ck::files {
namespace {
template<class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
template<class T> Observation<T> unavailable(const UnavailableObservation& e) { return Observation<T>::unavailable(e.state, e.error); }
Observation<ObjectIdentity> subject(const detail::LocationSubjectBookends& observations, bool directory) {
    if (!observations.anchor.value()) return unavailable<ObjectIdentity>(*observations.anchor.unavailable_detail());
    for (const auto* endpoint : {&observations.before,&observations.after}) {
        if (!endpoint->value()) return unavailable<ObjectIdentity>(*endpoint->unavailable_detail());
        const auto& m = *endpoint->value();
        if (!m.kind.value()) return unavailable<ObjectIdentity>(*m.kind.unavailable_detail());
        if (*m.kind.value() < ObjectKind::RegularFile || *m.kind.value() > ObjectKind::Other)
            return error<ObjectIdentity>(ObservationState::Failed, AdapterError::InvalidNativeResult, "location_relation.kind");
        if (!m.identity.value()) return unavailable<ObjectIdentity>(*m.identity.unavailable_detail());
        const auto compared = compare_identity(*observations.anchor.value(), *m.identity.value());
        if (compared != IdentityComparison::Same) return error<ObjectIdentity>(
            compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
            AdapterError::UnqualifiedIdentity, "location_relation.subject_identity");
    }
    if (*observations.before.value()->kind.value() != *observations.after.value()->kind.value())
        return error<ObjectIdentity>(ObservationState::Changed, AdapterError::MetadataChanged, "location_relation.kind_changed");
    if (directory && *observations.after.value()->kind.value() != ObjectKind::Directory)
        return error<ObjectIdentity>(ObservationState::Failed, AdapterError::NotDirectory, "location_relation.directory");
    return Observation<ObjectIdentity>::available(*observations.anchor.value());
}
Observation<LocationQueryFamily> family(const Observation<LocationInfo>& first, const Observation<LocationInfo>& last,
    const Observation<ObjectIdentity>& directory) {
    if (!directory.value()) return unavailable<LocationQueryFamily>(*directory.unavailable_detail());
    for (const auto* endpoint : {&first,&last}) {
        if (!endpoint->value()) return unavailable<LocationQueryFamily>(*endpoint->unavailable_detail());
        const auto& location = *endpoint->value();
        if (!location.directory.value()) return unavailable<LocationQueryFamily>(*location.directory.unavailable_detail());
        const auto compared = compare_identity(*directory.value(),*location.directory.value());
        if (compared != IdentityComparison::Same) return error<LocationQueryFamily>(
            compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
            AdapterError::UnqualifiedIdentity, "location_relation.location_owner");
        switch (location.family) {
        case LocationQueryFamily::Unknown: return error<LocationQueryFamily>(ObservationState::Unknown, AdapterError::UnsupportedQuery, "location_relation.family");
        case LocationQueryFamily::LinuxDescriptor: case LocationQueryFamily::DarwinDescriptor: case LocationQueryFamily::WindowsHandle: break;
        default: return error<LocationQueryFamily>(ObservationState::Failed, AdapterError::InvalidNativeResult, "location_relation.family");
        }
    }
    if (first.value()->family != last.value()->family)
        return error<LocationQueryFamily>(ObservationState::Changed, AdapterError::MetadataChanged, "location_relation.family_changed");
    return Observation<LocationQueryFamily>::available(first.value()->family);
}
Observation<std::uint64_t> mount(const detail::LocationSubjectBookends& value) {
    for (const auto* endpoint : {&value.before,&value.after}) {
        if (!endpoint->value()) return unavailable<std::uint64_t>(*endpoint->unavailable_detail());
        if (!endpoint->value()->mount_id.value())
            return unavailable<std::uint64_t>(*endpoint->value()->mount_id.unavailable_detail());
    }
    const auto& before = *value.before.value();
    const auto& after = *value.after.value();
    if (*before.mount_id.value() != *after.mount_id.value())
        return error<std::uint64_t>(ObservationState::Changed, AdapterError::MetadataChanged, "location_relation.mount_changed");
    return after.mount_id;
}
}
LocationRelationData detail::assess_location_bookends(const LocationSubjectBookends& directory, const LocationSubjectBookends& object,
    const Observation<LocationInfo>& first_location, const Observation<LocationInfo>& last_location) {
    LocationRelationData result;
    result.directory = subject(directory,true); result.object = subject(object,false);
    result.family = family(first_location,last_location,result.directory);
    for (const auto* id : {&result.directory,&result.object}) if (!id->value()) {
        result.volumes = unavailable<NativeVolumeRelation>(*id->unavailable_detail());
        result.linux_mounts = unavailable<LinuxMountRelation>(*id->unavailable_detail()); return result;
    }
    const auto& a = *result.directory.value(); const auto& b = *result.object.value();
    if (a.scope != b.scope || a.scheme != b.scheme) {
        result.volumes = error<NativeVolumeRelation>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "location_relation.identity_namespace");
        result.linux_mounts = unavailable<LinuxMountRelation>(*result.volumes.unavailable_detail()); return result;
    }
    result.volumes = Observation<NativeVolumeRelation>::available(a.volume == b.volume ? NativeVolumeRelation::SameReportedIdentifier : NativeVolumeRelation::DifferentReportedIdentifiers);
    if (!result.family.value()) { result.linux_mounts = unavailable<LinuxMountRelation>(*result.family.unavailable_detail()); return result; }
    if (*result.family.value() != LocationQueryFamily::LinuxDescriptor) {
        result.linux_mounts = error<LinuxMountRelation>(ObservationState::Unsupported, AdapterError::NotApplicable, "location_relation.linux_mount_query"); return result;
    }
    if (a.scheme != IdentityScheme::PosixDeviceInode) {
        result.linux_mounts = error<LinuxMountRelation>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "location_relation.linux_identity"); return result;
    }
    const auto first = mount(directory), last = mount(object);
    for (const auto* id : {&first,&last}) if (!id->value()) { result.linux_mounts = unavailable<LinuxMountRelation>(*id->unavailable_detail()); return result; }
    result.linux_mounts = Observation<LinuxMountRelation>::available(*first.value() == *last.value() ? LinuxMountRelation::SameReportedId : LinuxMountRelation::DifferentReportedIds);
    return result;
}
Observation<ObjectLocationRelation::Pointer> ObjectLocationRelation::inspect(DirectoryInventoryJob::Result directory, ObjectInventoryJob::Result object) {
    if (!directory || !object) return error<Pointer>(ObservationState::Failed, AdapterError::InvalidIdentifier, "location_relation.null_inventory");
    const auto& d = directory->data(); const auto& o = object->data();
    Observation<ObjectIdentity> anchor;
    if (o.before.value()) anchor = o.before.value()->identity;
    else anchor = unavailable<ObjectIdentity>(*o.before.unavailable_detail());
    auto data = detail::assess_location_bookends({d.anchor,d.before,d.after},{std::move(anchor),o.before,o.after},d.location_before,d.location_after);
    return Observation<Pointer>::available(Pointer(new ObjectLocationRelation(std::move(directory),std::move(object),std::move(data))));
}
} // namespace ck::files
