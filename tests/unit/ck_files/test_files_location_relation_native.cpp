// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "location_relation.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("native location relation fixture unavailable");
    return std::move(value).take_value();
}
NativeName name(const char* spelling) {
#ifdef _WIN32
    std::u16string units; while (*spelling) units.push_back(static_cast<char16_t>(*spelling++));
    return *NativeName::from_windows(std::move(units));
#else
    return *NativeName::from_posix(spelling);
#endif
}
DirectoryInventoryJob::Result finish(DirectoryInventoryJob& job) {
    for (int i = 0; i < 1000; ++i) {
        if (job.advance() == DirectoryInventoryState::Complete) return take(job.completed());
        if (job.state() == DirectoryInventoryState::Failed) throw std::runtime_error("native relation directory inventory failed");
    }
    throw std::runtime_error("native relation directory step bound");
}
ObjectInventoryJob::Result finish(ObjectInventoryJob& job) {
    for (int i = 0; i < 1000; ++i) {
        if (job.advance() == ObjectInventoryJobState::Complete) return take(job.completed());
        if (job.state() == ObjectInventoryJobState::Failed) throw std::runtime_error("native relation object inventory failed");
    }
    throw std::runtime_error("native relation object step bound");
}
void write(const fs::path& path) {
    std::ofstream out(path); out << "fixture"; out.close(); if (!out) throw std::runtime_error("relation fixture write failed");
}
ObjectLocationRelation::Pointer inspect(NativeFileSystem& filesystem, const fs::path& directory, const fs::path& parent, const char* child) {
    std::shared_ptr<const DirectoryCursor> cursor = take(filesystem.open_directory(directory));
    auto object = take(take(filesystem.open_directory(parent))->open_child(name(child)));
    DirectoryInventoryJob d(cursor); ObjectInventoryJob o(std::move(object));
    return take(ObjectLocationRelation::inspect(finish(d),finish(o)));
}
#ifdef __linux__
Observation<std::uint64_t> independent_mount(const fs::path& path) {
#ifdef STATX_MNT_ID
    struct statx value{};
    if (statx(AT_FDCWD,path.c_str(),AT_SYMLINK_NOFOLLOW,STATX_MNT_ID,&value) == 0 && (value.stx_mask & STATX_MNT_ID) != 0)
        return Observation<std::uint64_t>::available(value.stx_mnt_id);
#else
    (void)path;
#endif
    return Observation<std::uint64_t>::unavailable(ObservationState::Unknown, {ErrorDomain::Adapter,3,"independent.mount_unavailable"});
}
#endif
}
CK_TEST(native_location_relation_compares_actual_held_volume_and_linux_mount_observations) {
    const ck::testing::ScratchDirectory scratch("files-location-relation"); write(scratch.path()/"file"); NativeFileSystem filesystem;
    const auto result = inspect(filesystem,scratch.path(),scratch.path(),"file");
    CK_CHECK(take(result->data().volumes) == NativeVolumeRelation::SameReportedIdentifier);
#ifdef __linux__
    const auto directory_mount = independent_mount(scratch.path()), object_mount = independent_mount(scratch.path()/"file");
    if (result->data().linux_mounts.value()) {
        CK_CHECK(take(result->data().linux_mounts) == LinuxMountRelation::SameReportedId);
        CK_CHECK(take(directory_mount) == take(object_mount));
        CK_CHECK(take(result->directory()->data().after.value()->mount_id) == take(directory_mount));
        CK_CHECK(take(result->object()->data().after.value()->mount_id) == take(object_mount));
        std::cout << "native-location-relation: Linux mount IDs qualified\n";
    } else CK_CHECK(!directory_mount.value() || !object_mount.value());
#else
    CK_CHECK(result->data().linux_mounts.state() == ObservationState::Unsupported);
#endif
    CK_CHECK(fs::file_size(scratch.path()/"file") == 7);
    std::cout << "native-location-relation: held ordinary pair qualified\n";
}
CK_TEST(native_location_relation_remains_bound_to_retained_inventories_after_path_replacement) {
    try {
    const ck::testing::ScratchDirectory scratch("files-location-retained"); const auto directory = scratch.path()/"directory", moved = scratch.path()/"moved";
    fs::create_directory(directory); write(directory/"file"); NativeFileSystem filesystem;
    const auto result = inspect(filesystem,directory,directory,"file"); const auto held = take(result->data().object);
    // Move the held child out first: Windows can refuse renaming a directory
    // containing an open child even when both handles share delete access.
    // Keep both references alive and replace BOTH original addresses. The pair
    // now deliberately has no parent/child membership to infer from its IDs.
    fs::rename(directory/"file",scratch.path()/"retired-file");
    fs::rename(directory,moved); fs::create_directory(directory); write(directory/"file");
    const auto refreshed = take(ObjectLocationRelation::inspect(result->directory(),result->object()));
    CK_CHECK(compare_identity(held,take(refreshed->data().object)) == IdentityComparison::Same);
    CK_CHECK(compare_identity(held,take(take(filesystem.inspect_basic(directory/"file")).identity)) == IdentityComparison::Different);
    CK_CHECK(compare_identity(held,take(take(result->object()->object_for_worker().inspect_basic()).identity)) == IdentityComparison::Same);
    const auto held_directory = take(result->data().directory);
    CK_CHECK(compare_identity(held_directory,take(take(result->directory()->pin_directory()->inspect_basic()).identity)) == IdentityComparison::Same);
    CK_CHECK(compare_identity(held_directory,take(take(filesystem.inspect_basic(directory)).identity)) == IdentityComparison::Different);
    CK_CHECK(take(refreshed->data().volumes) == NativeVolumeRelation::SameReportedIdentifier);
    std::cout << "native-location-relation: retained pair replacement isolation qualified\n";
    } catch (const std::exception& error) {
        std::cerr << "native-location-relation: replacement fixture failed: " << error.what() << std::endl;
        throw;
    }
}

#ifdef __linux__
CK_TEST(native_location_relation_private_bind_and_tmpfs_fixture_distinguishes_mount_from_volume) {
    const char* configured = std::getenv("CKTOOLS_LOCATION_RELATION_FIXTURE"); if (!configured || !*configured) return;
    const fs::path root(configured); CK_CHECK(root.is_absolute()); NativeFileSystem filesystem;
    const auto bound = inspect(filesystem,root/"source",root,"alias");
    CK_CHECK(take(bound->data().volumes) == NativeVolumeRelation::SameReportedIdentifier);
    CK_CHECK(take(bound->data().linux_mounts) == LinuxMountRelation::DifferentReportedIds);
    CK_CHECK(compare_identity(take(bound->data().directory),take(bound->data().object)) == IdentityComparison::Same);
    CK_CHECK(take(independent_mount(root/"source")) != take(independent_mount(root/"alias")));
    const auto different = inspect(filesystem,root/"source",root,"other");
    CK_CHECK(take(different->data().volumes) == NativeVolumeRelation::DifferentReportedIdentifiers);
    CK_CHECK(take(different->data().linux_mounts) == LinuxMountRelation::DifferentReportedIds);
    CK_CHECK(take(independent_mount(root/"source")) != take(independent_mount(root/"other")));
    std::cout << "native-location-relation: same-volume bind mount and distinct-volume tmpfs qualified\n";
}
#endif
