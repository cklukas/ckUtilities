// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace ck::files;
namespace fs = std::filesystem;

template <class T>
const T& available(const Observation<T>& result) {
    if (!result.value()) throw std::runtime_error("required native observation unavailable");
    return *result.value();
}

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) throw std::runtime_error("cannot create native fixture");
}

#if !defined(_WIN32)
NativeName component(const char *text) {
    auto result = NativeName::from_posix(text);
    if (!result) throw std::runtime_error("cannot construct native test component");
    return std::move(*result);
}
#endif
}

CK_TEST(native_inspection_reports_missing_and_invalid_paths_without_cwd_fallback) {
    const ck::testing::ScratchDirectory scratch("files-missing");
    NativeFileSystem filesystem;
    const auto missing = filesystem.inspect_basic(scratch.path() / "missing");
    CK_CHECK(missing.state() == ObservationState::Gone && !missing.value());
    CK_CHECK(missing.unavailable_detail()->error.code != 0);
    const auto relative = filesystem.inspect_basic(fs::path("relative"));
    CK_CHECK(relative.state() == ObservationState::Failed);
    CK_CHECK(relative.unavailable_detail()->error.domain == ErrorDomain::Adapter);
    auto embedded = scratch.path().native();
    embedded.push_back(fs::path::value_type{});
    const auto nul = filesystem.inspect_basic(fs::path(embedded));
    CK_CHECK(nul.state() == ObservationState::Failed);
    CK_CHECK(nul.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidPath));
}

CK_TEST(native_regular_files_and_directories_have_explicit_applicable_metadata) {
    const ck::testing::ScratchDirectory scratch("files-metadata");
    NativeFileSystem filesystem;
    const auto path = scratch.path() / "data.bin";
    write_file(path, "abcdef");
    const auto result = filesystem.inspect_basic(path);
    const auto& metadata = available(result);
    CK_CHECK(available(metadata.kind) == ObjectKind::RegularFile);
    CK_CHECK(available(metadata.logical_size) == 6);
    CK_CHECK(metadata.allocated_size.state() == ObservationState::Available);
    CK_CHECK(available(metadata.link_count) >= 1);
    CK_CHECK(metadata.identity.state() == ObservationState::Available);
    CK_CHECK(metadata.modified.value() && metadata.modified.value()->nanoseconds() < 1'000'000'000U);
    const auto directory = filesystem.inspect_basic(scratch.path());
    CK_CHECK(available(available(directory).kind) == ObjectKind::Directory);
    CK_CHECK(available(directory).logical_size.state() == ObservationState::Unsupported);
    const auto empty = scratch.path() / "empty";
    write_file(empty, {});
    CK_CHECK(available(available(filesystem.inspect_basic(empty)).logical_size) == 0);
#if defined(_WIN32)
    CK_CHECK(metadata.windows_attributes.value() != nullptr);
    CK_CHECK(metadata.posix_permissions.state() == ObservationState::Unsupported);
#else
    CK_CHECK(metadata.posix_permissions.value() != nullptr);
    CK_CHECK(metadata.posix_uid.value() != nullptr && metadata.posix_gid.value() != nullptr);
    CK_CHECK(metadata.windows_attributes.state() == ObservationState::Unsupported);
#endif
}

#if !defined(_WIN32)
CK_TEST(native_trash_boundary_refuses_unsupported_platforms_without_retiring_source) {
    const ck::testing::ScratchDirectory scratch("files-trash-boundary");
    write_file(scratch.path() / "source", "must remain");
    NativeFileSystem filesystem;
    auto directory = filesystem.open_directory(scratch.path());
    CK_CHECK(directory.value());
    if (!directory.value()) return;
    auto child = (*directory.value())->open_child(component("source"), EntryAccess::MetadataOnly);
    CK_CHECK(child.value());
    if (!child.value()) return;
    const auto metadata = (*child.value())->inspect_basic();
    CK_CHECK(metadata.value() && metadata.value()->identity.value());
    if (!metadata.value() || !metadata.value()->identity.value()) return;
    const auto moved = (*directory.value())->trash_child(component("source"),
                                                          *metadata.value()->identity.value());
    CK_CHECK(!moved.value() && moved.state() == ObservationState::Unsupported);
    CK_CHECK(fs::exists(scratch.path() / "source"));
}
#endif

CK_TEST(native_hard_links_share_identity_but_adapter_scopes_do_not) {
    const ck::testing::ScratchDirectory scratch("files-identity");
    const auto first = scratch.path() / "first";
    const auto alias = scratch.path() / "alias";
    const auto other = scratch.path() / "other";
    write_file(first, "same bytes");
    write_file(other, "same bytes");
    fs::create_hard_link(first, alias);
    NativeFileSystem filesystem;
    NativeFileSystem another;
    const auto source = filesystem.inspect_basic(first);
    const auto link = filesystem.inspect_basic(alias);
    const auto different = filesystem.inspect_basic(other);
    const auto foreign = another.inspect_basic(first);
    const auto& source_id = available(available(source).identity);
    CK_CHECK(compare_identity(source_id, available(available(link).identity)) == IdentityComparison::Same);
    CK_CHECK(compare_identity(source_id, available(available(different).identity)) == IdentityComparison::Different);
    CK_CHECK(compare_identity(source_id, available(available(foreign).identity)) == IdentityComparison::Unknown);
    CK_CHECK(available(available(link).link_count) >= 2);
}

#if !defined(_WIN32)
CK_TEST(native_posix_inspection_never_follows_final_links_or_reads_fifos) {
    const ck::testing::ScratchDirectory scratch("files-links");
    NativeFileSystem filesystem;
    write_file(scratch.path() / "target", "content");
    fs::create_symlink("target", scratch.path() / "link");
    fs::create_symlink("missing", scratch.path() / "dangling");
    for (const auto* name : {"link", "dangling"}) {
        const auto result = filesystem.inspect_basic(scratch.path() / name);
        CK_CHECK(available(available(result).kind) == ObjectKind::SymbolicLink);
        CK_CHECK(available(result).logical_size.state() == ObservationState::Unsupported);
    }
    const auto fifo = scratch.path() / "fifo";
    if (mkfifo(fifo.c_str(), 0600) != 0) throw std::runtime_error("cannot create FIFO fixture");
    const auto result = filesystem.inspect_basic(fifo);
    CK_CHECK(available(available(result).kind) == ObjectKind::Fifo);
}

CK_TEST(native_posix_denial_is_not_reported_as_a_missing_or_empty_file) {
    const ck::testing::ScratchDirectory scratch("files-denied");
    if (geteuid() == 0) throw std::runtime_error("native access-denial qualification requires an unprivileged test user");
    const auto directory = scratch.path() / "private";
    fs::create_directory(directory);
    const auto path = directory / "file";
    write_file(path, "private");
    struct RestorePermissions {
        fs::path directory;
        ~RestorePermissions() {
            std::error_code ignored;
            fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace, ignored);
        }
    } restore{directory};
    fs::permissions(directory, fs::perms::none);
    NativeFileSystem filesystem;
    const auto denied = filesystem.inspect_basic(path);
    CK_CHECK(denied.state() == ObservationState::Denied && denied.value() == nullptr);
    CK_CHECK(denied.unavailable_detail() && denied.unavailable_detail()->error.domain == ErrorDomain::Posix);
    // The adapter needs directory search permission, not file read permission.
    fs::permissions(directory, fs::perms::owner_all);
    fs::permissions(path, fs::perms::none);
    const auto observed = filesystem.inspect_basic(path);
    CK_CHECK(available(available(observed).logical_size) == 7);
    CK_CHECK(available(available(observed).posix_permissions) == 0);
}

CK_TEST(native_posix_times_preserve_pre_epoch_seconds_and_nanoseconds) {
    const ck::testing::ScratchDirectory scratch("files-times");
    const auto path = scratch.path() / "file";
    write_file(path, "timestamp");
    const timespec values[2]{{0, 123'456'789}, {-1, 999'999'999}};
    if (utimensat(AT_FDCWD, path.c_str(), values, 0) != 0)
        throw std::runtime_error("cannot set qualified native timestamp fixture");
    NativeFileSystem filesystem;
    const auto observed = filesystem.inspect_basic(path);
    const auto& accessed = available(available(observed).accessed);
    const auto& modified = available(available(observed).modified);
    CK_CHECK(accessed.seconds() == 0 && accessed.nanoseconds() == 123'456'789U);
    CK_CHECK(modified.seconds() == -1 && modified.nanoseconds() == 999'999'999U);
}
#endif

#if defined(__linux__)
CK_TEST(native_linux_inspection_preserves_non_UTF8_path_bytes) {
    const ck::testing::ScratchDirectory scratch("files-byte-path");
    const auto path = scratch.path() / std::string("raw-\xFF\xFE");
    write_file(path, "bytes");
    NativeFileSystem filesystem;
    CK_CHECK(available(available(filesystem.inspect_basic(path)).logical_size) == 5);
}
#endif

#if defined(_WIN32)
CK_TEST(native_windows_metadata_refuses_device_namespaces_before_any_open) {
    const ck::testing::ScratchDirectory scratch("files-namespace");
    NativeFileSystem filesystem;
    for (const auto* path : {L"\\\\.\\pipe\\ck-files-no-connection", L"\\\\.\\PhysicalDrive0",
                            L"\\\\?\\GLOBALROOT\\Device\\NamedPipe\\ck-files-no-connection",
                            L"\\\\localhost\\pipe\\ck-files-no-connection",
                            L"\\\\localhost\\pipe .\\ck-files-no-connection",
                            L"\\\\?\\UNC\\localhost\\IPC$\\ck-files-no-connection"}) {
        const auto observed = filesystem.inspect_basic(fs::path(path));
        CK_CHECK(observed.state() == ObservationState::Failed);
        CK_CHECK(observed.unavailable_detail() && observed.unavailable_detail()->error.domain == ErrorDomain::Adapter);
    }
    for (const auto* name : {L"NUL", L"con.txt", L"NUL .txt", L"nul:stream", L"COM1.bin", L"LPT\u00B9.txt", L"CONOUT$"}) {
        const auto observed = filesystem.inspect_basic(scratch.path() / name);
        CK_CHECK(observed.state() == ObservationState::Failed);
        CK_CHECK(observed.unavailable_detail() && observed.unavailable_detail()->error.domain == ErrorDomain::Adapter);
    }
}

CK_TEST(native_windows_extended_and_volume_GUID_paths_identify_the_same_file) {
    const ck::testing::ScratchDirectory scratch("files-volume-path");
    const auto path = scratch.path() / L"file";
    write_file(path, "namespace");
    NativeFileSystem filesystem;
    const auto ordinary = filesystem.inspect_basic(path);
    auto native_path = path;
    native_path.make_preferred(); // Extended Win32 paths require backslashes.
    const auto extended = filesystem.inspect_basic(fs::path(std::wstring(L"\\\\?\\") + native_path.native()));
    CK_CHECK(compare_identity(available(available(ordinary).identity), available(available(extended).identity)) == IdentityComparison::Same);
    wchar_t volume[MAX_PATH + 1]{};
    if (!GetVolumeNameForVolumeMountPointW(native_path.root_path().c_str(), volume, MAX_PATH + 1))
        throw std::runtime_error("native Windows namespace qualification requires a local volume");
    const auto guid_path = fs::path(std::wstring(volume) + native_path.native().substr(native_path.root_path().native().size()));
    const auto guid = filesystem.inspect_basic(guid_path);
    CK_CHECK(available(available(guid).logical_size) == 9);
    CK_CHECK(compare_identity(available(available(ordinary).identity), available(available(guid).identity)) == IdentityComparison::Same);
}

CK_TEST(native_windows_inspection_preserves_unpaired_UTF16_path_units) {
    const ck::testing::ScratchDirectory scratch("files-unit-path");
    std::wstring name = L"raw-";
    name += static_cast<wchar_t>(0xD800);
    const auto path = scratch.path() / name;
    write_file(path, "units");
    NativeFileSystem filesystem;
    CK_CHECK(available(available(filesystem.inspect_basic(path)).logical_size) == 5);
}

CK_TEST(native_windows_times_preserve_exact_100ns_values) {
    const ck::testing::ScratchDirectory scratch("files-times");
    const auto path = scratch.path() / L"file";
    write_file(path, "timestamp");
    const HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open timestamp fixture");
    constexpr std::uint64_t ticks = 116'444'736'001'234'567ULL;
    const FILETIME value{static_cast<DWORD>(ticks & 0xFFFFFFFFU), static_cast<DWORD>(ticks >> 32U)};
    const BOOL changed = SetFileTime(handle, nullptr, &value, &value);
    CloseHandle(handle);
    if (!changed) throw std::runtime_error("cannot set timestamp fixture");
    NativeFileSystem filesystem;
    const auto observed = filesystem.inspect_basic(path);
    const auto& accessed = available(available(observed).accessed);
    const auto& modified = available(available(observed).modified);
    CK_CHECK(accessed.seconds() == 0 && accessed.nanoseconds() == 123'456'700U);
    CK_CHECK(modified.seconds() == 0 && modified.nanoseconds() == 123'456'700U);
}
#endif
