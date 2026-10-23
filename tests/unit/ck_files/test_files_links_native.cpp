// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <bit>
#include <cstring>
#include <winioctl.h>
#include <cstring>
#include "windows_junction.hpp"
#else
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif


namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template <class T> const T& required(const Observation<T>& result) {
    if (!result.value()) {
        const auto& e = *result.unavailable_detail();
        std::cerr << "link observation: " << static_cast<int>(e.state) << " " << e.error.operation << " " << e.error.code << std::endl;
        throw std::runtime_error("required link observation unavailable");
    }
    return *result.value();
}
void write_file(const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "fixture";
    file.close();
    if (!file) throw std::runtime_error("cannot write link fixture");
}
std::unique_ptr<OpenedEntry> open_entry(const NativeFileSystem& filesystem, const fs::path& path,
    EntryAccess access = EntryAccess::MetadataOnly) {
    auto parent_result = filesystem.open_directory(path.parent_path());
    (void)required(parent_result);
    auto parent = std::move(parent_result).take_value();
    const auto filename = path.filename();
#if defined(_WIN32)
    std::u16string units;
    for (const auto unit : filename.native()) units.push_back(static_cast<char16_t>(unit));
    auto name = NativeName::from_windows(std::move(units));
#else
    auto name = NativeName::from_posix(filename.native());
#endif
    if (!name) throw std::runtime_error("invalid link fixture name");
    auto opened = parent->open_child(*name, access);
    (void)required(opened);
    return std::move(opened).take_value();
}
}

CK_TEST(native_link_metadata_rejects_invalid_limits_and_ordinary_files_without_traversal) {
    const ck::testing::ScratchDirectory scratch("files-links-contract");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    CK_CHECK(opened->read_link_metadata({0, 1}).state() == ObservationState::Failed);
    const auto result = opened->read_link_metadata();
    CK_CHECK(result.state() == ObservationState::Unsupported);
    CK_CHECK(result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::NotApplicable));
}

#if !defined(_WIN32)
CK_TEST(native_posix_link_reads_keep_relative_dangling_long_and_non_utf8_targets_exact) {
    const ck::testing::ScratchDirectory scratch("files-links-posix-targets");
    NativeFileSystem filesystem;
    const std::string targets[] = {"../does-not-exist", std::string(700, 'q'), std::string("../raw-") + static_cast<char>(0xff) + '\n'};
    int index = 0;
    for (const auto& target : targets) {
        const auto path = scratch.path() / ("link-" + std::to_string(index++));
        fs::create_symlink(fs::path(target), path);
        auto opened = open_entry(filesystem, path);
        const auto result = opened->read_link_metadata({target.size(), 1});
        const auto* payload = std::get_if<PosixSymlinkData>(&required(result).payload);
        CK_CHECK(payload && payload->target == target);
        CK_CHECK(opened->read_link_metadata({target.size() - 1, 1}).state() == ObservationState::Unknown);
    }
}
CK_TEST(native_posix_link_target_is_read_from_the_held_object_after_rename_replacement_and_unlink) {
    const ck::testing::ScratchDirectory scratch("files-links-posix-owner");
    const auto path = scratch.path() / "link";
    const auto retired = scratch.path() / "retired";
    fs::create_symlink("../original", path);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto before = opened->inspect_basic();
    fs::rename(path, retired);
    fs::create_symlink("replacement", path);
    fs::remove(retired);
    const auto result = opened->read_link_metadata();
    const auto* payload = std::get_if<PosixSymlinkData>(&required(result).payload);
    CK_CHECK(payload && payload->target == "../original");
    CK_CHECK(compare_identity(required(required(result).owner), required(required(before).identity)) == IdentityComparison::Same);
}
#else
namespace {
std::u16string units(const fs::path& path) {
    std::u16string result;
    for (const auto unit : path.native()) result.push_back(static_cast<char16_t>(unit));
    return result;
}
}
CK_TEST(native_windows_junction_metadata_preserves_long_dangling_names_through_buffer_growth) {
    const ck::testing::ScratchDirectory scratch("files-links-windows-long");
    const auto link = scratch.path() / "link";
    auto target = scratch.path() / fs::path(std::wstring(1200, L'q'));
    target.make_preferred();
    fs::create_directory(link);
    ck::testing::make_junction(link, target);
    struct Remove { fs::path path; ~Remove() { RemoveDirectoryW(path.c_str()); } } remove{link};
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, link);
    const auto result = opened->read_link_metadata();
    const auto* payload = std::get_if<WindowsReparseData>(&required(result).payload);
    CK_CHECK(payload && payload->kind == WindowsReparseKind::MountPoint);
    if (!payload) return;
    CK_CHECK(payload->tag == IO_REPARSE_TAG_MOUNT_POINT && payload->names);
    CK_CHECK(payload->names->substitute == std::u16string(u"\\??\\") + units(target));
    CK_CHECK(payload->names->print == units(target));
    CK_CHECK(payload->raw.size() > 1024);
    CK_CHECK(opened->read_link_metadata({payload->raw.size(), 1}).state() == ObservationState::Available);
    CK_CHECK(opened->read_link_metadata({payload->raw.size() - 1, 1}).state() == ObservationState::Unknown);
}
CK_TEST(native_windows_junction_payload_stays_with_the_opened_object_after_unlink) {
    const ck::testing::ScratchDirectory scratch("files-links-windows-owner");
    const auto link = scratch.path() / "link";
    const auto retired = scratch.path() / "retired";
    fs::create_directory(link);
    ck::testing::make_junction(link, scratch.path() / "original");
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, link);
    const auto before = opened->read_link_metadata();
    fs::rename(link, retired);
    fs::create_directory(link);
    ck::testing::make_junction(link, scratch.path() / "replacement");
    struct Remove { fs::path path; ~Remove() { RemoveDirectoryW(path.c_str()); } } remove{link};
    if (!RemoveDirectoryW(retired.c_str())) throw std::runtime_error("cannot unlink junction fixture");
    const auto after = opened->read_link_metadata();
    const auto& original = std::get<WindowsReparseData>(required(before).payload);
    const auto& observed = std::get<WindowsReparseData>(required(after).payload);
    CK_CHECK(original.raw == observed.raw);
    CK_CHECK(compare_identity(required(required(before).owner), required(required(after).owner)) == IdentityComparison::Same);
}
CK_TEST(native_windows_opaque_guid_reparse_payload_is_retained_without_following_or_decoding_it_as_a_link) {
    const ck::testing::ScratchDirectory scratch("files-links-windows-guid");
    const auto path = scratch.path() / "file";
    write_file(path);
    struct Header { DWORD tag; WORD length; WORD reserved; GUID guid; };
    static_assert(sizeof(Header) == 24);
    Header header{0x42, 1500, 0, {0x77352b8a, 0x6521, 0x4ef1, {0x92, 0x77, 0x84, 0x12, 0x37, 0x63, 0x11, 0x09}}};
    NativeBytes bytes(sizeof(header) + header.length, std::byte{0x9b});
    std::memcpy(bytes.data(), &header, sizeof(header));
    const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (writer == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open opaque fixture writer");
    struct Cleanup {
        HANDLE handle; Header header;
        ~Cleanup() { DWORD returned = 0; DeviceIoControl(handle, FSCTL_DELETE_REPARSE_POINT, &header, sizeof(header), nullptr, 0, &returned, nullptr); CloseHandle(handle); }
    } cleanup{writer, header};
    cleanup.header.length = 0;
    DWORD returned = 0;
    if (!DeviceIoControl(writer, FSCTL_SET_REPARSE_POINT, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, 0, &returned, nullptr))
        throw std::runtime_error("cannot set opaque GUID fixture: " + std::to_string(GetLastError()));
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto queried = opened->read_link_metadata();
    const auto& value = std::get<WindowsReparseData>(required(queried).payload);
    CK_CHECK(value.kind == WindowsReparseKind::OpaqueGuid && value.raw == bytes && value.owner_guid && !value.names);
    NativeIdentifier expected;
    std::memcpy(expected.bytes.data(), &header.guid, sizeof(header.guid));
    CK_CHECK(*value.owner_guid == expected);
    CK_CHECK(opened->read_link_metadata({128, 1}).state() == ObservationState::Unknown);
}
#endif

