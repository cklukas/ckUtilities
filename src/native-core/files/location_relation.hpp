// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_inventory_job.hpp"
#include "object_inventory_job.hpp"
namespace ck::files {
enum class NativeVolumeRelation { SameReportedIdentifier, DifferentReportedIdentifiers };
enum class LinuxMountRelation { SameReportedId, DifferentReportedIds };
struct LocationRelationData {
    Observation<ObjectIdentity> directory, object;
    Observation<LocationQueryFamily> family;
    Observation<NativeVolumeRelation> volumes;
    Observation<LinuxMountRelation> linux_mounts;
};
namespace detail {
struct LocationSubjectBookends {
    Observation<ObjectIdentity> anchor;
    Observation<BasicMetadata> before, after;
};
LocationRelationData assess_location_bookends(const LocationSubjectBookends& directory, const LocationSubjectBookends& object,
    const Observation<LocationInfo>& first_location, const Observation<LocationInfo>& last_location);
}
// Worker-owned comparison of cached, completed inventories. No new I/O, path
// reopen, enumeration, content read, mutation or refresh. Keeps both native
// owners alive; retire on their worker. The pair need not have parent/child
// membership. Equal reported volume IDs do not rule out distinct mounts,
// cloned-volume serial collisions, subvolumes or other filesystem boundaries.
// Neither relationship establishes a snapshot, ancestry, traversability,
// creatability, same-filesystem rename support or operation permission.
class ObjectLocationRelation final {
public:
    using Pointer = std::shared_ptr<const ObjectLocationRelation>;
    static Observation<Pointer> inspect(DirectoryInventoryJob::Result directory, ObjectInventoryJob::Result object);
    const LocationRelationData& data() const noexcept { return data_; }
    const DirectoryInventoryJob::Result& directory() const noexcept { return directory_; }
    const ObjectInventoryJob::Result& object() const noexcept { return object_; }
private:
    ObjectLocationRelation(DirectoryInventoryJob::Result directory, ObjectInventoryJob::Result object, LocationRelationData data) noexcept
        : directory_(std::move(directory)), object_(std::move(object)), data_(std::move(data)) {}
    DirectoryInventoryJob::Result directory_;
    ObjectInventoryJob::Result object_;
    LocationRelationData data_;
};
} // namespace ck::files
