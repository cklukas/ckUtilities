// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"

#include <filesystem>
#include <fstream>
#include <array>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

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

#if defined(_WIN32)
std::wstring final_path(const std::filesystem::path &path) {
    const auto raw = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (raw == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open Recycle Bin fixture");
    std::vector<wchar_t> buffer(32768);
    const auto written = GetFinalPathNameByHandleW(raw, buffer.data(), static_cast<DWORD>(buffer.size()),
                                                   FILE_NAME_NORMALIZED);
    CloseHandle(raw);
    if (!written || written >= buffer.size()) throw std::runtime_error("cannot resolve Recycle Bin fixture");
    return std::wstring(buffer.data(), written);
}

std::size_t remove_recycle_fixture(const std::wstring &root, const std::wstring &original) {
    const auto pattern = root + L"\\$Recycle.Bin\\*";
    WIN32_FIND_DATAW directory_data{};
    const auto directory_search = FindFirstFileW(pattern.c_str(), &directory_data);
    if (directory_search == INVALID_HANDLE_VALUE) return 0;
    std::size_t removed = 0;
    do {
        if ((directory_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            std::wstring(directory_data.cFileName) == L"." || std::wstring(directory_data.cFileName) == L"..")
            continue;
        const auto directory = root + L"\\$Recycle.Bin\\" + directory_data.cFileName;
        WIN32_FIND_DATAW metadata_data{};
        const auto metadata_search = FindFirstFileW((directory + L"\\$I*").c_str(), &metadata_data);
        if (metadata_search == INVALID_HANDLE_VALUE) continue;
        do {
            const std::wstring metadata_name(metadata_data.cFileName);
            if (metadata_name.size() < 3 || metadata_name[0] != L'$' || metadata_name[1] != L'I') continue;
            const auto metadata_path = directory + L"\\" + metadata_name;
            const auto handle = CreateFileW(metadata_path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) continue;
            std::array<std::byte, 544> bytes{};
            DWORD read = 0;
            const bool read_ok = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                                 read == bytes.size();
            CloseHandle(handle);
            if (!read_ok) continue;
            std::wstring recorded;
            for (std::size_t offset = 24; offset + sizeof(wchar_t) <= bytes.size(); offset += sizeof(wchar_t)) {
                wchar_t unit{};
                std::memcpy(&unit, bytes.data() + offset, sizeof(unit));
                if (!unit) break;
                recorded.push_back(unit);
            }
            if (recorded != original) continue;
            const auto restored_path = directory + L"\\$R" + metadata_name.substr(2);
            const bool restored_removed = DeleteFileW(restored_path.c_str()) != FALSE;
            const bool metadata_removed = DeleteFileW(metadata_path.c_str()) != FALSE;
            if (restored_removed && metadata_removed) ++removed;
        } while (FindNextFileW(metadata_search, &metadata_data));
        FindClose(metadata_search);
    } while (FindNextFileW(directory_search, &directory_data));
    FindClose(directory_search);
    return removed;
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
CK_TEST(native_trash_boundary_uses_platform_metadata_and_retires_source) {
    const ck::testing::ScratchDirectory scratch("files-trash-boundary");
    const ck::testing::ScratchDirectory trash_home("files-trash-boundary-home");
#if defined(__APPLE__)
    const char *previous_home = std::getenv("HOME");
    const std::string previous_home_value = previous_home ? previous_home : "";
    const bool had_home = previous_home != nullptr;
    CK_CHECK(::setenv("HOME", trash_home.path().c_str(), 1) == 0);
#else
    const ck::testing::ScratchDirectory trash_data("files-trash-boundary-data");
    const char *previous_home = std::getenv("HOME");
    const char *previous_xdg = std::getenv("XDG_DATA_HOME");
    const std::string previous_home_value = previous_home ? previous_home : "";
    const std::string previous_xdg_value = previous_xdg ? previous_xdg : "";
    const bool had_home = previous_home != nullptr;
    const bool had_xdg = previous_xdg != nullptr;
    CK_CHECK(::setenv("HOME", trash_home.path().c_str(), 1) == 0);
    CK_CHECK(::setenv("XDG_DATA_HOME", trash_data.path().c_str(), 1) == 0);
#endif
    write_file(scratch.path() / "source", "must be retired through native Trash");
    NativeFileSystem filesystem;
    auto directory = filesystem.open_directory(scratch.path());
    CK_CHECK(directory.value());
    if (directory.value()) {
        auto child = (*directory.value())->open_child(component("source"), EntryAccess::MetadataOnly);
        CK_CHECK(child.value());
        if (child.value()) {
            const auto metadata = (*child.value())->inspect_basic();
            CK_CHECK(metadata.value() && metadata.value()->identity.value());
            if (metadata.value() && metadata.value()->identity.value()) {
                const auto moved = (*directory.value())->trash_child(component("source"),
                                                                      *metadata.value()->identity.value());
                CK_CHECK(moved.value() && *moved.value());
                CK_CHECK(!fs::exists(scratch.path() / "source"));
#if defined(__APPLE__)
                const auto trash_root = trash_home.path() / ".Trash";
#else
                const auto trash_root = trash_data.path() / "Trash";
#endif
                CK_CHECK(fs::exists(trash_root / "files" / "source"));
                std::ifstream info(trash_root / "info" / "source.trashinfo");
                const std::string metadata_text((std::istreambuf_iterator<char>(info)),
                                                std::istreambuf_iterator<char>());
                CK_CHECK(metadata_text.find("[Trash Info]") != std::string::npos &&
                         metadata_text.find("Path=file://") != std::string::npos &&
                         metadata_text.find("DeletionDate=") != std::string::npos);
            }
        }
    }
#if defined(__APPLE__)
    if (had_home) CK_CHECK(::setenv("HOME", previous_home_value.c_str(), 1) == 0);
    else CK_CHECK(::unsetenv("HOME") == 0);
#else
    if (had_home) CK_CHECK(::setenv("HOME", previous_home_value.c_str(), 1) == 0);
    else CK_CHECK(::unsetenv("HOME") == 0);
    if (had_xdg) CK_CHECK(::setenv("XDG_DATA_HOME", previous_xdg_value.c_str(), 1) == 0);
    else CK_CHECK(::unsetenv("XDG_DATA_HOME") == 0);
#endif
}

fs::path foreign_mount_root(fs::path path) {
    struct stat current{};
    if (::stat(path.c_str(), &current) != 0) return {};
    while (path.has_parent_path()) {
        const auto parent = path.parent_path();
        if (parent == path) break;
        struct stat parent_stat{};
        if (::stat(parent.c_str(), &parent_stat) != 0 || parent_stat.st_dev != current.st_dev) break;
        path = parent;
        current = parent_stat;
    }
    return path;
}

CK_TEST(native_trash_mount_specific_layout_is_identity_checked_and_cleaned) {
    const char *foreign_root_text = std::getenv("CKTOOLS_FILES_FOREIGN_TRASH_ROOT");
    const char *home_root_text = std::getenv("CKTOOLS_FILES_TRASH_HOME_ROOT");
    // This is an opt-in cross-mount qualification. The ordinary native suite
    // remains self-contained and must not require a second filesystem.
    if (!foreign_root_text || !home_root_text || !foreign_root_text[0] || !home_root_text[0]) return;

    const ck::testing::ScratchDirectory foreign_root("files-trash-foreign", foreign_root_text);
    const ck::testing::ScratchDirectory trash_home("files-trash-foreign-home", home_root_text);
    const auto basename = std::string("native-foreign-") + foreign_root.path().filename().string();
    const auto source = foreign_root.path() / basename;
    write_file(source, "mount-specific Trash must retain restore data");

    const auto mount = foreign_mount_root(foreign_root.path());
    CK_CHECK(!mount.empty());
    if (mount.empty()) return;

    const auto mount_stat = [&] {
        struct stat value{};
        return ::stat(mount.c_str(), &value) == 0 ? value.st_dev : dev_t{};
    }();
    struct stat source_stat{};
    CK_CHECK(::stat(source.c_str(), &source_stat) == 0 && mount_stat == source_stat.st_dev);

    const char *previous_home = std::getenv("HOME");
#if !defined(__APPLE__)
    const char *previous_xdg = std::getenv("XDG_DATA_HOME");
#endif
    const std::string previous_home_value = previous_home ? previous_home : "";
#if !defined(__APPLE__)
    const std::string previous_xdg_value = previous_xdg ? previous_xdg : "";
#endif
    const bool had_home = previous_home != nullptr;
#if !defined(__APPLE__)
    const bool had_xdg = previous_xdg != nullptr;
#endif
    CK_CHECK(::setenv("HOME", trash_home.path().c_str(), 1) == 0);
#if !defined(__APPLE__)
    CK_CHECK(::setenv("XDG_DATA_HOME", trash_home.path().c_str(), 1) == 0);
#endif

    NativeFileSystem filesystem;
    auto directory = filesystem.open_directory(foreign_root.path());
    CK_CHECK(directory.value());
    if (directory.value()) {
        auto child_name = NativeName::from_posix(basename);
        CK_CHECK(child_name);
        if (child_name) {
            auto child = (*directory.value())->open_child(*child_name, EntryAccess::MetadataOnly);
            CK_CHECK(child.value());
            if (child.value()) {
                const auto metadata = (*child.value())->inspect_basic();
                CK_CHECK(metadata.value() && metadata.value()->identity.value());
                if (metadata.value() && metadata.value()->identity.value()) {
                    const auto moved = (*directory.value())->trash_child(*child_name,
                                                                          *metadata.value()->identity.value());
                    CK_CHECK(moved.value() && *moved.value());
                    CK_CHECK(!fs::exists(source));
#if defined(__APPLE__)
                    const auto trash_root = mount / ".Trashes" / std::to_string(static_cast<unsigned long long>(::geteuid()));
#else
                    const auto trash_root = mount / (std::string(".Trash-") +
                                                      std::to_string(static_cast<unsigned long long>(::geteuid())));
#endif
                    const auto trashed = trash_root / "files" / basename;
                    const auto info_path = trash_root / "info" / (basename + ".trashinfo");
                    CK_CHECK(fs::exists(trashed) && fs::exists(info_path));
                    std::ifstream info(info_path);
                    const std::string metadata_text((std::istreambuf_iterator<char>(info)),
                                                    std::istreambuf_iterator<char>());
                    CK_CHECK(metadata_text.find("[Trash Info]") != std::string::npos &&
                             metadata_text.find("Path=file://") != std::string::npos &&
                             metadata_text.find("DeletionDate=") != std::string::npos);
                    std::error_code cleanup;
                    fs::remove(trashed, cleanup);
                    fs::remove(info_path, cleanup);
                    fs::remove(trash_root / "files", cleanup);
                    fs::remove(trash_root / "info", cleanup);
                    fs::remove(trash_root, cleanup);
#if defined(__APPLE__)
                    fs::remove(mount / ".Trashes", cleanup);
#endif
                }
            }
        }
    }
#if defined(__APPLE__)
    if (had_home) CK_CHECK(::setenv("HOME", previous_home_value.c_str(), 1) == 0);
    else CK_CHECK(::unsetenv("HOME") == 0);
#else
    if (had_home) CK_CHECK(::setenv("HOME", previous_home_value.c_str(), 1) == 0);
    else CK_CHECK(::unsetenv("HOME") == 0);
    if (had_xdg) CK_CHECK(::setenv("XDG_DATA_HOME", previous_xdg_value.c_str(), 1) == 0);
    else CK_CHECK(::unsetenv("XDG_DATA_HOME") == 0);
#endif
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
CK_TEST(native_windows_trash_moves_fixture_to_recycle_bin_with_restore_metadata) {
    const ck::testing::ScratchDirectory scratch("files-trash");
    const auto path = scratch.path() / L"trash-fixture.bin";
    write_file(path, "recycle fixture");
    const auto original = final_path(path);
    wchar_t volume[MAX_PATH]{};
    if (!GetVolumePathNameW(path.c_str(), volume, ARRAYSIZE(volume)))
        throw std::runtime_error("cannot resolve Recycle Bin volume");
    NativeFileSystem filesystem;
    auto directory_observed = filesystem.open_directory(scratch.path());
    auto directory = std::move(directory_observed).take_value();
    auto child_observed = directory->open_child(*NativeName::from_windows(u"trash-fixture.bin"), EntryAccess::MetadataOnly);
    auto child = std::move(child_observed).take_value();
    const auto metadata = available(child->inspect_basic());
    const auto moved = directory->trash_child(*NativeName::from_windows(u"trash-fixture.bin"), *metadata.identity.value());
    const auto removed = remove_recycle_fixture(volume, original);
    CK_CHECK(moved.value() && *moved.value());
    CK_CHECK(!std::filesystem::exists(path));
    CK_CHECK(removed == 1);
}

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
