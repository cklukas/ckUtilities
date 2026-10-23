// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/mount.h>
#else
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <cerrno>
#endif
#endif

namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template <class T> const T& required(const Observation<T>& result) {
    if (!result.value()) {
        const auto& e = *result.unavailable_detail();
        std::cerr << "location query: " << static_cast<int>(e.state) << " " << e.error.operation << " " << e.error.code << std::endl;
        throw std::runtime_error("required location observation unavailable");
    }
    return *result.value();
}
std::unique_ptr<DirectoryCursor> open_directory(const NativeFileSystem& filesystem, const fs::path& path) {
    auto result = filesystem.open_directory(path);
    (void)required(result);
    return std::move(result).take_value();
}
void write_file(const fs::path& path) {
    std::ofstream file(path); file << "fixture"; file.close();
    if (!file) throw std::runtime_error("cannot write location fixture");
}
}

CK_TEST(native_location_queries_remain_attached_to_the_held_directory_after_rename_replacement_and_unlink) {
    const ck::testing::ScratchDirectory scratch("files-location-owner");
    const auto path = scratch.path() / "directory";
    const auto retired = scratch.path() / "retired";
    fs::create_directory(path);
    NativeFileSystem filesystem;
    auto directory = open_directory(filesystem, path);
    const auto before = directory->inspect_location();
    fs::rename(path, retired);
    fs::create_directory(path);
    fs::remove(retired);
    const auto after = directory->inspect_location();
    CK_CHECK(compare_identity(required(required(after).directory), required(required(before).directory)) == IdentityComparison::Same);
    CK_CHECK(compare_identity(required(required(after).directory), required(required(filesystem.inspect_basic(path)).identity)) == IdentityComparison::Different);
    CK_CHECK(required(after).family == required(before).family);
    CK_CHECK(required(required(after).space_accounting_unit) == required(required(before).space_accounting_unit));
}

CK_TEST(native_location_inspection_does_not_reset_or_consume_directory_enumeration) {
    const ck::testing::ScratchDirectory scratch("files-location-enumeration");
    for (int index = 0; index < 8; ++index) write_file(scratch.path() / ("file-" + std::to_string(index)));
    NativeFileSystem filesystem;
    auto directory = open_directory(filesystem, scratch.path());
    std::set<std::string> names;
    for (std::size_t steps = 0; steps < 100; ++steps) {
        const auto batch = directory->read(1);
        for (const auto& name : batch.names) CK_CHECK(names.insert(name.display_name()).second);
        const auto location = directory->inspect_location();
        CK_CHECK(compare_identity(required(required(location).directory), required(directory->identity())) == IdentityComparison::Same);
        if (batch.state == DirectoryReadState::End) break;
        CK_CHECK(batch.state == DirectoryReadState::More);
    }
    CK_CHECK(names.size() == 8);
    for (int index = 0; index < 8; ++index) CK_CHECK(names.contains("file-" + std::to_string(index)));
}

#if defined(_WIN32)
CK_TEST(native_windows_location_flags_limits_and_case_query_match_independent_sdk_queries) {
    const ck::testing::ScratchDirectory scratch("files-location-windows");
    NativeFileSystem filesystem;
    auto directory = open_directory(filesystem, scratch.path());
    const auto queried = directory->inspect_location();
    const auto& info = required(queried);
    CK_CHECK(info.family == LocationQueryFamily::WindowsHandle);
    DWORD serial = 0, maximum = 0, flags = 0;
    wchar_t name[MAX_PATH + 1]{};
    std::vector<wchar_t> mount_path(32768);
    if (!GetVolumePathNameW(scratch.path().c_str(), mount_path.data(), static_cast<DWORD>(mount_path.size())))
        throw std::runtime_error("cannot identify independent fixture mount path");
    if (!GetVolumeInformationW(mount_path.data(), nullptr, 0, &serial, &maximum, &flags, name, MAX_PATH + 1))
        throw std::runtime_error("cannot independently query Windows volume");
    CK_CHECK(required(info.volume_query_serial) == serial && required(info.windows_component_utf16_max) == maximum);
    CK_CHECK(required(info.features).value[0] == flags);
    CK_CHECK(required(info.read_only_mount) == ((flags & FILE_READ_ONLY_VOLUME) != 0));
    const std::pair<VolumeFeature, DWORD> mapping[] = {
        {VolumeFeature::NamedStreams, FILE_NAMED_STREAMS}, {VolumeFeature::ExtendedAttributes, FILE_SUPPORTS_EXTENDED_ATTRIBUTES},
        {VolumeFeature::PersistentAcls, FILE_PERSISTENT_ACLS}, {VolumeFeature::SparseFiles, FILE_SUPPORTS_SPARSE_FILES},
        {VolumeFeature::HardLinks, FILE_SUPPORTS_HARD_LINKS}, {VolumeFeature::CaseSensitiveNames, FILE_CASE_SENSITIVE_SEARCH},
        {VolumeFeature::CasePreservedNames, FILE_CASE_PRESERVED_NAMES}, {VolumeFeature::FileCompression, FILE_FILE_COMPRESSION},
        {VolumeFeature::FileEncryption, FILE_SUPPORTS_ENCRYPTION}, {VolumeFeature::BlockRefcounting, FILE_SUPPORTS_BLOCK_REFCOUNTING}};
    for (const auto& [feature, bit] : mapping) {
        CK_CHECK(required(advertised_feature(info.features, feature)) == ((flags & bit) != 0 ? FeatureAdvertisement::Advertised : FeatureAdvertisement::NotAdvertised));
        NativeVolumeFeatures single;
        single.domain = VolumeFeatureDomain::WindowsVolumeFlags;
        single.value[0] = single.valid[0] = bit;
        CK_CHECK(required(advertised_feature(Observation<NativeVolumeFeatures>::available(single), feature)) == FeatureAdvertisement::Advertised);
    }
    const auto reference = CreateFileW(scratch.path().c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (reference == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open native case fixture");
    FILE_CASE_SENSITIVE_INFO case_info{};
    const auto success = GetFileInformationByHandleEx(reference, FileCaseSensitiveInfo, &case_info, sizeof(case_info));
    const auto error = GetLastError();
    CloseHandle(reference);
    if (success) CK_CHECK(required(info.case_sensitive_directory) == ((case_info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0));
    else CK_CHECK(info.case_sensitive_directory.unavailable_detail() && info.case_sensitive_directory.unavailable_detail()->error.code == error);
    DWORD sectors = 0, bytes = 0, free_clusters = 0, total_clusters = 0;
    if (!GetDiskFreeSpaceW(mount_path.data(), &sectors, &bytes, &free_clusters, &total_clusters))
        throw std::runtime_error("cannot query independent allocation unit");
    CK_CHECK(required(info.space_accounting_unit) == static_cast<std::uint64_t>(sectors) * bytes);
    CK_CHECK(required(info.total_bytes) > 0);
    (void)required(info.free_bytes); (void)required(info.available_bytes);
}
#else
CK_TEST(native_posix_location_queries_match_descriptor_mount_and_pathconf_results) {
    const ck::testing::ScratchDirectory scratch("files-location-posix");
    NativeFileSystem filesystem;
    auto directory = open_directory(filesystem, scratch.path());
    const auto observed = directory->inspect_location();
    const auto& info = required(observed);
    const int fd = open(scratch.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("cannot open independent location descriptor");
    struct Close { int value; ~Close() { close(value); } } close_fd{fd};
    CK_CHECK(required(info.posix_name_max) == static_cast<std::uint64_t>(fpathconf(fd, _PC_NAME_MAX)));
    CK_CHECK(required(info.posix_path_max) == static_cast<std::uint64_t>(fpathconf(fd, _PC_PATH_MAX)));
#if defined(__APPLE__)
    CK_CHECK(info.family == LocationQueryFamily::DarwinDescriptor);
    struct statfs native{};
    if (fstatfs(fd, &native) != 0) throw std::runtime_error("cannot inspect APFS location fixture");
    CK_CHECK(required(info.read_only_mount) == ((native.f_flags & MNT_RDONLY) != 0));
    CK_CHECK(required(info.native_mount_flags) == native.f_flags);
    CK_CHECK(required(info.space_accounting_unit) == native.f_bsize);
    CK_CHECK(required(info.case_sensitive_directory) == (fpathconf(fd, _PC_CASE_SENSITIVE) != 0));
    struct Caps { std::uint32_t size; vol_capabilities_attr_t data; } caps{};
    struct attrlist request{}; request.bitmapcount = ATTR_BIT_MAP_COUNT; request.volattr = ATTR_VOL_INFO | ATTR_VOL_CAPABILITIES;
    if (fgetattrlist(fd, &request, &caps, sizeof(caps), 0) != 0) throw std::runtime_error("native capability fixture unavailable");
    const std::pair<VolumeFeature, std::pair<std::size_t, std::uint32_t>> mapping[] = {
        {VolumeFeature::SparseFiles, {VOL_CAPABILITIES_FORMAT, VOL_CAP_FMT_SPARSE_FILES}},
        {VolumeFeature::SymbolicLinks, {VOL_CAPABILITIES_FORMAT, VOL_CAP_FMT_SYMBOLICLINKS}},
        {VolumeFeature::HardLinks, {VOL_CAPABILITIES_FORMAT, VOL_CAP_FMT_HARDLINKS}},
        {VolumeFeature::CaseSensitiveNames, {VOL_CAPABILITIES_FORMAT, VOL_CAP_FMT_CASE_SENSITIVE}},
        {VolumeFeature::CasePreservedNames, {VOL_CAPABILITIES_FORMAT, VOL_CAP_FMT_CASE_PRESERVING}},
        {VolumeFeature::FileCompression, {VOL_CAPABILITIES_FORMAT, VOL_CAP_FMT_DECMPFS_COMPRESSION}},
        {VolumeFeature::NamedStreams, {VOL_CAPABILITIES_INTERFACES, VOL_CAP_INT_NAMEDSTREAMS}},
        {VolumeFeature::ExtendedAttributes, {VOL_CAPABILITIES_INTERFACES, VOL_CAP_INT_EXTENDED_ATTR}},
        {VolumeFeature::PersistentAcls, {VOL_CAPABILITIES_INTERFACES, VOL_CAP_INT_EXTENDED_SECURITY}},
        {VolumeFeature::NativeClones, {VOL_CAPABILITIES_INTERFACES, VOL_CAP_INT_CLONE}}};
    for (const auto& [feature, position] : mapping) {
        const auto [group, bit] = position;
        const auto result = advertised_feature(info.features, feature);
        if ((caps.data.valid[group] & bit) == 0) CK_CHECK(result.state() == ObservationState::Unknown);
        else CK_CHECK(required(result) == ((caps.data.capabilities[group] & bit) != 0 ? FeatureAdvertisement::Advertised : FeatureAdvertisement::NotAdvertised));
        NativeVolumeFeatures single;
        single.domain = VolumeFeatureDomain::DarwinVolumeCapabilities;
        single.value[group] = single.valid[group] = bit;
        CK_CHECK(required(advertised_feature(Observation<NativeVolumeFeatures>::available(single), feature)) == FeatureAdvertisement::Advertised);
    }
#else
    CK_CHECK(info.family == LocationQueryFamily::LinuxDescriptor);
    struct statvfs native{};
    if (fstatvfs(fd, &native) != 0) throw std::runtime_error("cannot inspect Linux location fixture");
    CK_CHECK(required(info.read_only_mount) == ((native.f_flag & ST_RDONLY) != 0));
    CK_CHECK(required(info.native_mount_flags) == native.f_flag);
    CK_CHECK(required(info.space_accounting_unit) == native.f_frsize);
    CK_CHECK(info.features.state() == ObservationState::Unsupported);
    CK_CHECK(info.case_sensitive_directory.state() == ObservationState::Unsupported);
#endif
    CK_CHECK(required(info.total_bytes) > 0);
    (void)required(info.free_bytes); (void)required(info.available_bytes);
}
#endif
CK_TEST(native_directory_inode_flags_are_linux_scoped_and_match_an_independent_held_descriptor) {
    const ck::testing::ScratchDirectory scratch("files-location-inode-flags");
    NativeFileSystem filesystem;
    auto directory = open_directory(filesystem, scratch.path());
    const auto queried = directory->inspect_location(); const auto& info = required(queried);
#if defined(__linux__)
    const int descriptor = open(scratch.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) throw std::runtime_error("cannot open independent inode-flags descriptor");
    int flags = 0; const int status = ioctl(descriptor, FS_IOC_GETFLAGS, &flags); const int code = errno;
    close(descriptor);
    if (status == 0) {
        CK_CHECK(required(info.linux_directory_inode_flags) == static_cast<std::uint32_t>(flags));
        const auto casefold = reported_linux_directory_flag(info, LinuxDirectoryFeature::Casefold);
        CK_CHECK(required(casefold) == ((flags & FS_CASEFOLD_FL) != 0 ? ReportedFlag::Set : ReportedFlag::Clear));
    } else {
        CK_CHECK(!info.linux_directory_inode_flags.value() && info.linux_directory_inode_flags.unavailable_detail()->error.domain == ErrorDomain::Posix);
        CK_CHECK(info.linux_directory_inode_flags.unavailable_detail() && info.linux_directory_inode_flags.unavailable_detail()->error.code == code);
    }
    CK_CHECK(info.case_sensitive_directory.state() == ObservationState::Unsupported);
#else
    CK_CHECK(info.linux_directory_inode_flags.state() == ObservationState::Unsupported);
    CK_CHECK(reported_linux_directory_flag(info, LinuxDirectoryFeature::Casefold).state() == ObservationState::Unsupported);
#endif
}
#if defined(__linux__)
CK_TEST(native_linux_casefold_query_preserves_reported_flags_without_emulating_lookup_equivalence) {
    const ck::testing::ScratchDirectory scratch("files-location-casefold");
    const int descriptor = open(scratch.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) throw std::runtime_error("cannot open owned casefold fixture");
    int flags = 0;
    if (ioctl(descriptor, FS_IOC_GETFLAGS, &flags) != 0) {
        const int code = errno; close(descriptor);
#if defined(CK_FILES_REQUIRE_CASEFOLD)
        throw std::runtime_error("required casefold fixture GETFLAGS failed: " + std::to_string(code));
#else
        if (code != ENOTTY && code != EOPNOTSUPP && code != EINVAL) throw std::runtime_error("unexpected casefold GETFLAGS failure");
        std::cout << "CASEFOLD_UNAVAILABLE query_errno=" << code << std::endl; return;
#endif
    }
    flags |= FS_CASEFOLD_FL;
    const int changed = ioctl(descriptor, FS_IOC_SETFLAGS, &flags); const int code = errno; close(descriptor);
    if (changed != 0) {
#if defined(CK_FILES_REQUIRE_CASEFOLD)
        throw std::runtime_error("required casefold fixture SETFLAGS failed: " + std::to_string(code));
#else
        if (code != ENOTTY && code != EOPNOTSUPP && code != EINVAL) throw std::runtime_error("unexpected casefold SETFLAGS failure");
        std::cout << "CASEFOLD_UNAVAILABLE set_errno=" << code << std::endl; return;
#endif
    }
    write_file(scratch.path() / "MixedCase");
    NativeFileSystem filesystem; auto directory = open_directory(filesystem, scratch.path());
    const auto queried = directory->inspect_location(); const auto& info = required(queried);
    CK_CHECK(required(reported_linux_directory_flag(info, LinuxDirectoryFeature::Casefold)) == ReportedFlag::Set);
    CK_CHECK(info.case_sensitive_directory.state() == ObservationState::Unsupported);
    const auto lookup = directory->lookup_child(*NativeName::from_posix("mixedcase"), ChildLookupMode::NativeDefault);
    CK_CHECK(required(lookup).state() == ChildLookupState::Found);
    std::cout << "CASEFOLD_QUALIFIED flags=" << required(info.linux_directory_inode_flags) << std::endl;
}
#endif
