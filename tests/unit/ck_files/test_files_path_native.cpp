// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <stdexcept>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("native address fixture unavailable");
    return std::move(value).take_value();
}
void write(const fs::path& path) {
    std::ofstream stream(path, std::ios::binary); stream << "address"; stream.close();
    if (!stream) throw std::runtime_error("native address fixture write failed");
}
}
CK_TEST(native_path_roundtrip_inspects_raw_filename_and_held_child_without_text_conversion) {
    const ck::testing::ScratchDirectory scratch("files-path");
#ifdef _WIN32
    const std::wstring spelling = L"raw-e\u0301";
    const auto name = *NativeName::from_windows(u"raw-e\u0301");
#else
    const std::string spelling = "raw-e\xcc\x81\\literal";
    const auto name = *NativeName::from_posix(spelling);
#endif
    const auto file = scratch.path()/fs::path(spelling); write(file);
    const auto address = take(NativeAbsolutePath::from_host(file));
    CK_CHECK(take(address.host_path()).native() == file.native());
    NativeFileSystem filesystem;
    const auto basic = take(filesystem.inspect_native_basic(address));
    CK_CHECK(take(basic.kind) == ObjectKind::RegularFile && take(basic.logical_size) == 7);
    const auto directory = take(NativeAbsolutePath::from_host(scratch.path()));
    auto parent = take(filesystem.open_native_directory(directory)); const auto child = take(parent->open_child(name));
    CK_CHECK(compare_identity(take(basic.identity),take(take(child->inspect_basic()).identity)) == IdentityComparison::Same);
    CK_CHECK(fs::file_size(file) == 7);
}
CK_TEST(native_path_navigation_address_is_not_retained_object_identity_after_replacement) {
    const ck::testing::ScratchDirectory scratch("files-path-replace");
    const auto file = scratch.path()/"file", moved = scratch.path()/"moved"; write(file);
    NativeFileSystem filesystem; const auto address = take(NativeAbsolutePath::from_host(file));
    auto parent = take(filesystem.open_native_directory(take(NativeAbsolutePath::from_host(scratch.path()))));
#ifdef _WIN32
    const auto name = *NativeName::from_windows(u"file");
#else
    const auto name = *NativeName::from_posix("file");
#endif
    const auto child = take(parent->open_child(name)); const auto held = take(take(child->inspect_basic()).identity);
    fs::rename(file,moved); write(file);
    CK_CHECK(compare_identity(held,take(take(child->inspect_basic()).identity)) == IdentityComparison::Same);
    CK_CHECK(compare_identity(held,take(take(filesystem.inspect_native_basic(address)).identity)) == IdentityComparison::Different);
    CK_CHECK(take(address.host_path()).native() == file.native());
}
CK_TEST(native_path_bridge_refuses_foreign_encoding_and_bounds_direct_host_paths_before_native_io) {
    NativeFileSystem filesystem;
#ifdef _WIN32
    const auto foreign = take(NativeAbsolutePath::from_posix("/foreign"));
    const fs::path huge(std::wstring(L"V:\\") + std::wstring(NativePathLimits{}.max_native_bytes/2, L'a'));
#else
    const auto foreign = take(NativeAbsolutePath::from_windows(u"V:\\foreign"));
    const fs::path huge(std::string("/") + std::string(NativePathLimits{}.max_native_bytes, 'a'));
#endif
    CK_CHECK(filesystem.inspect_native_basic(foreign).state() == ObservationState::Unsupported);
    CK_CHECK(filesystem.open_native_directory(foreign).state() == ObservationState::Unsupported);
    const auto basic = filesystem.inspect_basic(huge); const auto expected = NativeAbsolutePath::from_host(huge);
    CK_CHECK(basic.state() == ObservationState::Unknown && basic.unavailable_detail()->error == expected.unavailable_detail()->error);
    const auto directory = filesystem.open_directory(huge);
    CK_CHECK(directory.state() == ObservationState::Unknown && directory.unavailable_detail()->error == expected.unavailable_detail()->error);
}

CK_TEST(native_path_raw_encoding_acceptance_or_refusal_matches_the_actual_filesystem_without_rewriting) {
    const ck::testing::ScratchDirectory scratch("files-path-encoding");
#ifdef _WIN32
    std::wstring spelling = L"raw"; spelling.push_back(static_cast<wchar_t>(0xd800));
    const auto expected_name = *NativeName::from_windows(std::u16string{u'r',u'a',u'w',char16_t{0xd800}});
#else
    const std::string spelling = "raw\xff"; const auto expected_name = *NativeName::from_posix(spelling);
#endif
    const auto file = scratch.path()/fs::path(spelling); const auto address = take(NativeAbsolutePath::from_host(file));
    CK_CHECK(take(address.host_path()).native() == file.native());
    NativeFileSystem filesystem;
#ifdef _WIN32
    HANDLE created = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    const DWORD create_error = created == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (created != INVALID_HANDLE_VALUE) CloseHandle(created);
#else
    const int created = open(file.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    const int create_error = created < 0 ? errno : 0;
    if (created >= 0) close(created);
#endif
    const auto report = filesystem.inspect_native_basic(address);
    auto parent = take(filesystem.open_native_directory(take(NativeAbsolutePath::from_host(scratch.path()))));
    if (create_error == 0) {
        CK_CHECK(take(take(report).logical_size) == 0);
        const auto child = take(parent->open_child(expected_name));
        CK_CHECK(compare_identity(take(take(report).identity),take(take(child->inspect_basic()).identity)) == IdentityComparison::Same);
        const auto listing = parent->read(16);
        CK_CHECK(listing.state == DirectoryReadState::End && listing.names.size() == 1 && listing.names[0] == expected_name);
        std::cout << "native-path: raw units accepted and retained exactly\n";
    } else {
#ifdef _WIN32
        CK_CHECK(!report.value());
        CK_CHECK(create_error == ERROR_INVALID_NAME || create_error == ERROR_NO_UNICODE_TRANSLATION);
        HANDLE queried = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
        const DWORD lookup_error = queried == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        if (queried != INVALID_HANDLE_VALUE) CloseHandle(queried);
        CK_CHECK(lookup_error != 0 && report.unavailable_detail()->error.domain == ErrorDomain::Win32 && report.unavailable_detail()->error.code == lookup_error);
#else
        CK_CHECK(!report.value());
        CK_CHECK(create_error == EILSEQ || create_error == EINVAL);
        struct stat native{}; const int queried = lstat(file.c_str(), &native); const int lookup_error = queried < 0 ? errno : 0;
        CK_CHECK(queried < 0 && report.unavailable_detail()->error.domain == ErrorDomain::Posix && report.unavailable_detail()->error.code == lookup_error);
#endif
        CK_CHECK(!report.value() && fs::is_empty(scratch.path()));
        std::cout << "native-path: raw units refused by filesystem, native creation code " << create_error << "\n";
    }
}
