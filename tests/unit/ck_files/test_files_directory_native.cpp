// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include "windows_junction.hpp"
#include <cstddef>
#include <cstring>
#include <vector>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace ck::files;
namespace fs = std::filesystem;
using Spelling = fs::path::string_type;

void write_file(const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "fixture";
    file.close();
    if (!file) throw std::runtime_error("cannot create directory fixture");
}

std::unique_ptr<DirectoryCursor> open_directory(const NativeFileSystem& filesystem, const fs::path& path,
    DirectoryLinkPolicy policy = DirectoryLinkPolicy::RejectFinalLink) {
    auto result = filesystem.open_directory(path, policy);
    if (!result.value()) {
        const auto& detail = *result.unavailable_detail();
        throw std::runtime_error("directory open failed: " + detail.error.operation + " " + std::to_string(detail.error.code));
    }
    return std::move(result).take_value();
}

Spelling native_spelling(const NativeName& name) {
#if defined(_WIN32)
    if (!name.windows_units()) throw std::runtime_error("wrong directory name encoding");
    Spelling result;
    for (auto unit : *name.windows_units()) result.push_back(static_cast<wchar_t>(unit));
    return result;
#else
    if (!name.posix_bytes()) throw std::runtime_error("wrong directory name encoding");
    return *name.posix_bytes();
#endif
}

std::set<Spelling> collect(DirectoryCursor& cursor, std::size_t limit = 37) {
    std::set<Spelling> names;
    for (int batch_number = 0; batch_number < 10000; ++batch_number) {
        const auto batch = cursor.read(limit);
        if (batch.error) throw std::runtime_error("directory read failed: " + batch.error->error.operation + " " + std::to_string(batch.error->error.code));
        if (batch.names.size() > limit) throw std::runtime_error("unbounded directory batch");
        for (const auto& name : batch.names)
            if (!names.insert(native_spelling(name)).second) throw std::runtime_error("duplicate static directory entry");
        if (batch.state == DirectoryReadState::End) return names;
        if (batch.state != DirectoryReadState::More) throw std::runtime_error("unexpected directory cancellation");
    }
    throw std::runtime_error("directory cursor did not reach end");
}
}

CK_TEST(native_directory_reads_empty_and_multiple_native_buffers_without_navigation_entries) {
    const ck::testing::ScratchDirectory scratch("files-directory-batches");
    NativeFileSystem filesystem;
    auto empty = open_directory(filesystem, scratch.path());
    const auto ended = empty->read(4);
    CK_CHECK(ended.state == DirectoryReadState::End && ended.names.empty() && !ended.error);
    std::set<Spelling> expected;
    for (int index = 0; index < 600; ++index) {
        const auto name = "entry-" + std::to_string(index) + "-" + std::string(128, 'a');
        write_file(scratch.path() / name);
        expected.insert(fs::path(name).native());
    }
    fs::create_directory(scratch.path() / "subdirectory");
    expected.insert(fs::path("subdirectory").native());
    auto cursor = open_directory(filesystem, scratch.path());
    CK_CHECK(collect(*cursor) == expected);
    const auto again = cursor->read(1);
    CK_CHECK(again.state == DirectoryReadState::End && again.names.empty());
}

CK_TEST(native_directory_handle_survives_path_rename_replacement_and_adapter_lifetime) {
    const ck::testing::ScratchDirectory scratch("files-directory-pin");
    const auto original = scratch.path() / "original";
    const auto retired = scratch.path() / "renamed";
    fs::create_directory(original);
    write_file(original / "old-child");
    std::unique_ptr<DirectoryCursor> cursor;
    {
        NativeFileSystem filesystem;
        cursor = open_directory(filesystem, original);
        CK_CHECK(cursor->identity().value() != nullptr);
        fs::rename(original, retired);
        fs::create_directory(original);
        write_file(original / "replacement-child");
        const auto observed = filesystem.inspect_basic(original);
        const auto moved = filesystem.inspect_basic(retired);
        CK_CHECK(observed.value() && observed.value()->identity.value() && moved.value() && moved.value()->identity.value());
        CK_CHECK(compare_identity(*cursor->identity().value(), *observed.value()->identity.value()) == IdentityComparison::Different);
        CK_CHECK(compare_identity(*cursor->identity().value(), *moved.value()->identity.value()) == IdentityComparison::Same);
    }
    CK_CHECK(collect(*cursor, 1) == std::set<Spelling>{fs::path("old-child").native()});
}

CK_TEST(native_directory_cancellation_does_not_consume_names_or_close_the_cursor) {
    const ck::testing::ScratchDirectory scratch("files-directory-cancel");
    write_file(scratch.path() / "child");
    NativeFileSystem filesystem;
    auto cursor = open_directory(filesystem, scratch.path());
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = cursor->read(1, stop.get_token());
    CK_CHECK(cancelled.state == DirectoryReadState::Cancelled && cancelled.names.empty() && !cancelled.error);
    CK_CHECK(collect(*cursor, 1) == std::set<Spelling>{fs::path("child").native()});
}

CK_TEST(native_directory_open_distinguishes_invalid_missing_and_non_directory_paths) {
    const ck::testing::ScratchDirectory scratch("files-directory-open-errors");
    NativeFileSystem filesystem;
    CK_CHECK(filesystem.open_directory("relative").state() == ObservationState::Failed);
    auto nul = scratch.path().native();
    nul.push_back(fs::path::value_type{});
    CK_CHECK(filesystem.open_directory(fs::path(nul)).state() == ObservationState::Failed);
    CK_CHECK(filesystem.open_directory(scratch.path() / "missing").state() == ObservationState::Gone);
    write_file(scratch.path() / "file");
    CK_CHECK(filesystem.open_directory(scratch.path() / "file").state() == ObservationState::Failed);
    CK_CHECK(filesystem.open_directory(scratch.path(), static_cast<DirectoryLinkPolicy>(255)).state() == ObservationState::Failed);
}

CK_TEST(native_directory_enumeration_keeps_exact_unusual_native_name_units) {
    const ck::testing::ScratchDirectory scratch("files-directory-names");
    Spelling name = fs::path("native-").native();
#if defined(_WIN32)
    name.push_back(static_cast<wchar_t>(0xD800));
#else
#if defined(__linux__)
    name.push_back(static_cast<char>(0xFF));
#else
    name += "\xC3\xA4"; // APFS accepts Unicode names, not arbitrary invalid UTF-8.
#endif
    name.push_back('\\');
    name.push_back('\n');
#endif
    write_file(scratch.path() / fs::path(name));
    NativeFileSystem filesystem;
    auto cursor = open_directory(filesystem, scratch.path());
    CK_CHECK(collect(*cursor) == std::set<Spelling>{name});
}

#if !defined(_WIN32)
CK_TEST(native_posix_directory_final_symlink_requires_explicit_follow_including_trailing_slash) {
    const ck::testing::ScratchDirectory scratch("files-directory-links");
    fs::create_directory(scratch.path() / "target");
    write_file(scratch.path() / "target" / "child");
    fs::create_directory_symlink("target", scratch.path() / "link");
    NativeFileSystem filesystem;
    for (const auto& path : {scratch.path() / "link", scratch.path() / "link" / ""}) {
        CK_CHECK(!filesystem.open_directory(path).value());
        auto cursor = open_directory(filesystem, path, DirectoryLinkPolicy::FollowFinalLink);
        CK_CHECK(collect(*cursor) == std::set<Spelling>{fs::path("child").native()});
    }
}

CK_TEST(native_posix_symbolic_link_creation_is_relative_no_replace_and_verifies_payload) {
    const ck::testing::ScratchDirectory scratch("files-directory-create-symlink");
    fs::create_directory(scratch.path() / "target");
    NativeFileSystem filesystem;
    auto cursor = open_directory(filesystem, scratch.path());
    const auto name = NativeName::from_posix("link");
    CK_CHECK(name.has_value());
    const auto created = cursor->create_symbolic_link(SymbolicLinkTarget{std::string("../target")}, *name);
    CK_CHECK(created.value() && *created.value());
    std::vector<char> bytes(128, '\0');
    const auto length = ::readlink((scratch.path() / "link").c_str(), bytes.data(), bytes.size());
    CK_CHECK(length == 9 && std::string(bytes.data(), static_cast<std::size_t>(length)) == "../target");
    const auto duplicate = cursor->create_symbolic_link(SymbolicLinkTarget{std::string("other")}, *name);
    CK_CHECK(!duplicate.value() && duplicate.state() == ObservationState::Changed);
    auto opened = cursor->open_child(*name);
    CK_CHECK(opened.value());
    if (opened.value()) {
        const auto metadata = (*opened.value())->read_link_metadata();
        CK_CHECK(metadata.value() && std::holds_alternative<PosixSymlinkData>(metadata.value()->payload));
        if (metadata.value() && std::holds_alternative<PosixSymlinkData>(metadata.value()->payload))
            CK_CHECK(std::get<PosixSymlinkData>(metadata.value()->payload).target == "../target");
    }
}

CK_TEST(native_posix_directory_access_denial_is_not_an_empty_listing) {
    const ck::testing::ScratchDirectory scratch("files-directory-denied");
    if (geteuid() == 0) throw std::runtime_error("directory denial qualification needs an unprivileged user");
    const auto path = scratch.path() / "denied";
    fs::create_directory(path);
    struct Restore {
        fs::path path;
        ~Restore() { chmod(path.c_str(), 0700); }
    } restore{path};
    if (chmod(path.c_str(), 0000) != 0) throw std::runtime_error("cannot set directory mode");
    NativeFileSystem filesystem;
    const auto denied = filesystem.open_directory(path);
    CK_CHECK(denied.state() == ObservationState::Denied && !denied.value());
    CK_CHECK(denied.unavailable_detail()->error.domain == ErrorDomain::Posix);
}
#endif

#if defined(_WIN32)
namespace {
struct TestHandleCloser {
    void operator()(void* handle) const noexcept { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using TestHandle = std::unique_ptr<void, TestHandleCloser>;

}

CK_TEST(native_windows_directory_reparse_navigation_requires_explicit_follow) {
    const ck::testing::ScratchDirectory scratch("files-directory-junction");
    const auto target = scratch.path() / "target";
    const auto link = scratch.path() / "junction";
    fs::create_directory(target);
    fs::create_directory(link);
    write_file(target / "child");
    ck::testing::make_junction(link, target);
    struct RemoveJunction {
        fs::path link;
        ~RemoveJunction() { RemoveDirectoryW(link.c_str()); }
    } remove{link};
    NativeFileSystem filesystem;
    for (const auto& path : {link, link / ""}) {
        const auto rejected = filesystem.open_directory(path);
        CK_CHECK(rejected.state() == ObservationState::Failed && !rejected.value());
        CK_CHECK(rejected.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::FinalLinkRejected));
        auto followed = open_directory(filesystem, path, DirectoryLinkPolicy::FollowFinalLink);
        CK_CHECK(collect(*followed) == std::set<Spelling>{fs::path("child").native()});
    }
}

CK_TEST(native_windows_directory_sharing_denial_and_device_names_never_become_empty_listings) {
    const ck::testing::ScratchDirectory scratch("files-directory-sharing");
    const TestHandle locked(CreateFileW(scratch.path().c_str(), FILE_LIST_DIRECTORY, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (locked.get() == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot lock directory fixture");
    NativeFileSystem filesystem;
    const auto denied = filesystem.open_directory(scratch.path());
    CK_CHECK(denied.state() == ObservationState::Denied && !denied.value());
    if (denied.unavailable_detail()) CK_CHECK(denied.unavailable_detail()->error.code == ERROR_SHARING_VIOLATION);
    for (const auto* device : {L"\\\\.\\NUL", L"\\\\.\\pipe\\ck-files-must-never-connect", L"V:\\NUL"}) {
        const auto rejected = filesystem.open_directory(device);
        CK_CHECK(rejected.state() == ObservationState::Failed && !rejected.value());
        CK_CHECK(rejected.unavailable_detail()->error.domain == ErrorDomain::Adapter);
    }
}
#endif
