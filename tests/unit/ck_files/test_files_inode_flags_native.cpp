// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "object_inventory_job.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef __linux__
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) { const auto& e = *value.unavailable_detail(); std::cerr << "inode flags unavailable: " << e.error.operation << ' ' << e.error.code << std::endl; throw std::runtime_error("native inode flags fixture unavailable"); }
    return std::move(value).take_value();
}
NativeName name() {
#ifdef _WIN32
    return *NativeName::from_windows(u"file");
#else
    return *NativeName::from_posix("file");
#endif
}
void write(const fs::path& path) { std::ofstream out(path); out << "fixture"; out.close(); if (!out) throw std::runtime_error("inode flags fixture write failed"); }
#ifdef __linux__
struct File {
    int descriptor;
    explicit File(const fs::path& path) : descriptor(::open(path.c_str(),O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) {
        if (descriptor < 0) throw std::runtime_error("independent inode flags open failed");
    }
    ~File() { close(descriptor); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
struct Control { std::uint32_t flags; int error; };
Control independent(const fs::path& path) {
    File file(path); int value = 0;
    if (ioctl(file.descriptor,FS_IOC_GETFLAGS,&value) != 0) { const int code = errno; return {0,code}; }
    return {static_cast<std::uint32_t>(value),0};
}
int no_dump(const fs::path& path) {
    File file(path); int value = 0;
    if (ioctl(file.descriptor,FS_IOC_GETFLAGS,&value) != 0) return errno;
    value |= FS_NODUMP_FL;
    return ioctl(file.descriptor,FS_IOC_SETFLAGS,&value) == 0 ? 0 : errno;
}
bool matches(const Observation<FileInodeFlags>& report, Control control) {
    if (control.error != 0) {
        CK_CHECK(!report.value() && report.unavailable_detail()->error.domain == ErrorDomain::Posix &&
            report.unavailable_detail()->error.code == control.error && report.unavailable_detail()->error.operation == "FS_IOC_GETFLAGS.file");
        return false;
    }
    CK_CHECK(report.value() && report.value()->raw == control.flags); return true;
}
#endif
ObjectInventoryJob::Result finish(ObjectInventoryJob& job) {
    for (int i = 0; i < 1000; ++i) {
        if (job.advance() == ObjectInventoryJobState::Complete) return take(job.completed());
        if (job.state() == ObjectInventoryJobState::Failed) throw std::runtime_error("inode flags inventory failed");
    }
    throw std::runtime_error("inode flags inventory step bound");
}
}
CK_TEST(native_inode_flags_explicit_access_is_independent_and_matches_direct_read_only_control) {
    const ck::testing::ScratchDirectory scratch("files-inode-flags"); write(scratch.path()/"file"); NativeFileSystem filesystem;
    auto parent = take(filesystem.open_directory(scratch.path()));
    auto metadata = take(parent->open_child(name()));
#ifdef __linux__
    CK_CHECK(metadata->inspect_inode_flags().state() == ObservationState::Denied);
    auto layout = take(parent->open_child(name(),EntryAccess::MetadataAndLayout));
    CK_CHECK(layout->inspect_inode_flags().state() == ObservationState::Denied);
#endif
    auto entry = take(parent->open_child(name(),EntryAccess::MetadataAndInodeFlags)); const auto report = entry->inspect_inode_flags();
#ifdef __linux__
    if (matches(report,independent(scratch.path()/"file"))) {
        const auto flags = take(report);
        CK_CHECK(compare_identity(take(flags.owner),take(take(entry->inspect_basic()).identity)) == IdentityComparison::Same);
        std::cout << "native-inode-flags: explicit access and independent control qualified\n";
    }
    CK_CHECK(entry->inspect_data_layout().state() == ObservationState::Denied);
#else
    CK_CHECK(metadata->inspect_inode_flags().state() == ObservationState::Unsupported && report.state() == ObservationState::Unsupported);
#endif
    InventoryLimits invalid; invalid.max_native_bytes = 0; CK_CHECK(entry->inspect_inode_flags(invalid).state() == ObservationState::Failed);
    InventoryLimits bounded_limits; bounded_limits.max_native_bytes = 3; const auto bounded = entry->inspect_inode_flags(bounded_limits);
#ifdef __linux__
    CK_CHECK(bounded.state() == ObservationState::Unknown && bounded.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
#else
    CK_CHECK(bounded.state() == ObservationState::Unsupported);
#endif
    CK_CHECK(fs::file_size(scratch.path()/"file") == 7);
}
CK_TEST(native_inode_flags_inventory_retains_held_inode_after_replacement_and_no_dump_metadata) {
    const ck::testing::ScratchDirectory scratch("files-inode-flags-held"); const auto file = scratch.path()/"file", moved = scratch.path()/"moved"; write(file);
#ifdef __linux__
    const int fixture_error = no_dump(file);
    CK_CHECK(fixture_error == 0 || fixture_error == EOPNOTSUPP || fixture_error == ENOTTY || fixture_error == EINVAL || fixture_error == EPERM || fixture_error == EACCES);
    if (fixture_error) std::cout << "native-inode-flags: private no-dump fixture refused, native code " << fixture_error << "\n";
#endif
    NativeFileSystem filesystem; auto parent = take(filesystem.open_directory(scratch.path()));
    auto entry = take(parent->open_child(name(),EntryAccess::MetadataAndInodeFlags)); const auto identity = take(take(entry->inspect_basic()).identity);
    fs::rename(file,moved); write(file); ObjectInventoryJob job(std::move(entry)); const auto result = finish(job);
#ifdef __linux__
    if (matches(result->data().inode_flags,independent(moved))) {
        const auto flags = take(result->data().inode_flags);
        CK_CHECK(compare_identity(take(flags.owner),identity) == IdentityComparison::Same);
        CK_CHECK(compare_identity(take(flags.owner),take(take(filesystem.inspect_basic(file)).identity)) == IdentityComparison::Different);
        CK_CHECK(take(result->compare_owner(flags.owner)) == IdentityComparison::Same);
        if (fixture_error == 0) {
            CK_CHECK(take(reported_inode_flag(flags,LinuxInodeFlag::NoDump)) == ReportedFlag::Set);
            std::cout << "native-inode-flags: retained no-dump inode and inventory qualified\n";
        }
    }
    CK_CHECK(compare_identity(take(take(result->object_for_worker().inspect_basic()).identity),identity) == IdentityComparison::Same);
#else
    CK_CHECK(result->data().inode_flags.state() == ObservationState::Unsupported);
    CK_CHECK(compare_identity(take(take(result->object_for_worker().inspect_basic()).identity),identity) == IdentityComparison::Same);
#endif
    CK_CHECK(fs::file_size(file) == 7 && fs::file_size(moved) == 7);
}
CK_TEST(native_inode_flags_read_access_never_opens_special_objects_for_content) {
    const ck::testing::ScratchDirectory scratch("files-inode-flags-special"); NativeFileSystem filesystem;
#ifdef __linux__
    if (mkfifo((scratch.path()/"file").c_str(),0600) != 0) throw std::runtime_error("inode flags FIFO fixture failed");
    auto parent = take(filesystem.open_directory(scratch.path())); const auto fifo = parent->open_child(name(),EntryAccess::MetadataAndInodeFlags);
    CK_CHECK(fifo.state() == ObservationState::Unsupported && fifo.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::NotApplicable));
#else
    fs::create_directory(scratch.path()/"file"); auto parent = take(filesystem.open_directory(scratch.path())); auto entry = take(parent->open_child(name(),EntryAccess::MetadataAndInodeFlags));
    CK_CHECK(entry->inspect_inode_flags().state() == ObservationState::Unsupported);
#endif
}

#ifdef __linux__
CK_TEST(native_inode_flags_read_permission_denial_is_not_bypassed_by_the_metadata_reference) {
    const ck::testing::ScratchDirectory scratch("files-inode-flags-denied"); write(scratch.path()/"file"); NativeFileSystem filesystem;
    if (geteuid() == 0) throw std::runtime_error("inode flags permission qualification requires an unprivileged user");
    struct Restore { fs::path path; ~Restore() { chmod(path.c_str(),0600); } } restore{scratch.path()/"file"};
    if (chmod(restore.path.c_str(),0) != 0) throw std::runtime_error("cannot set inode flags permission fixture");
    auto parent = take(filesystem.open_directory(scratch.path()));
    const auto metadata = take(parent->open_child(name(),EntryAccess::MetadataOnly));
    CK_CHECK(metadata->inspect_basic().value() && metadata->inspect_inode_flags().state() == ObservationState::Denied);
    const auto denied = parent->open_child(name(),EntryAccess::MetadataAndInodeFlags);
    CK_CHECK(denied.state() == ObservationState::Denied && !denied.value() && denied.unavailable_detail()->error.operation == "openat.read_access_fd");
    std::cout << "native-inode-flags: unprivileged read denial qualified\n";
}
#endif
