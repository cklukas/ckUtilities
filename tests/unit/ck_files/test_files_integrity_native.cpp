// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "object_inventory_job.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> result) {
    if (!result.value()) throw std::runtime_error("native integrity observation unavailable");
    return std::move(result).take_value();
}
NativeName name() {
#ifdef _WIN32
    return *NativeName::from_windows(u"file");
#else
    return *NativeName::from_posix("file");
#endif
}
void write(const fs::path& path) { std::ofstream file(path); file << "fixture"; file.close(); if (!file) throw std::runtime_error("integrity fixture write failed"); }
ObjectInventoryJob::Result finish(ObjectInventoryJob& job) {
    for (int i = 0; i < 1000; ++i) {
        if (job.advance() == ObjectInventoryJobState::Complete) return take(job.completed());
        if (job.state() == ObjectInventoryJobState::Failed) throw std::runtime_error("integrity object inventory failed");
    }
    throw std::runtime_error("integrity object inventory step bound");
}
#ifdef _WIN32
bool compare_independent(const fs::path& path, const Observation<FileIntegrityInfo>& report) {
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("independent integrity handle failed");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close{handle};
    FSCTL_GET_INTEGRITY_INFORMATION_BUFFER value{}; DWORD bytes = 0;
    const auto success = DeviceIoControl(handle, FSCTL_GET_INTEGRITY_INFORMATION, nullptr, 0, &value, sizeof(value), &bytes, nullptr);
    const auto code = success ? ERROR_SUCCESS : GetLastError();
    if (!success) {
        CK_CHECK(!report.value() && report.unavailable_detail()->error.domain == ErrorDomain::Win32 &&
            report.unavailable_detail()->error.code == code);
        return false;
    }
    CK_CHECK(report.value());
    CK_CHECK(bytes == WindowsIntegrityData::wire_bytes);
    const auto raw = std::as_bytes(std::span(&value, 1));
    const auto retained = take(report);
    CK_CHECK(std::equal(raw.begin(), raw.end(), retained.data.raw().begin(), retained.data.raw().end()));
    return true;
}
#endif
}
CK_TEST(native_integrity_query_uses_metadata_rights_and_matches_independent_file_and_directory_controls) {
    const ck::testing::ScratchDirectory scratch("files-integrity"); const auto file = scratch.path()/"file"; write(file);
    NativeFileSystem filesystem; auto parent = take(filesystem.open_directory(scratch.path())); auto entry = take(parent->open_child(name()));
    const auto report = entry->inspect_integrity();
#ifdef _WIN32
    const auto file_supported = compare_independent(file, report);
    if (file_supported) {
        const auto value = take(report); CK_CHECK(value.kind == ObjectKind::RegularFile && value.family == IntegrityQueryFamily::WindowsFsctl);
        CK_CHECK(compare_identity(take(value.owner), take(take(entry->inspect_basic()).identity)) == IdentityComparison::Same);
    }
    fs::create_directory(scratch.path()/"folder"); auto folder = take(parent->open_child(*NativeName::from_windows(u"folder")));
    const auto directory = folder->inspect_integrity(); const auto directory_supported = compare_independent(scratch.path()/"folder", directory);
    if (directory_supported) CK_CHECK(take(directory).kind == ObjectKind::Directory);
    if (file_supported && directory_supported) std::cout << "native-integrity: metadata-only file and directory controls qualified\n";
#else
    CK_CHECK(report.state() == ObservationState::Unsupported && report.unavailable_detail()->error.operation == "integrity.native_query");
#endif
    CK_CHECK(fs::file_size(file) == 7);
}
CK_TEST(native_integrity_query_retains_its_held_object_after_rename_and_path_replacement) {
    const ck::testing::ScratchDirectory scratch("files-integrity-held"); const auto file = scratch.path()/"file", moved = scratch.path()/"moved"; write(file);
    NativeFileSystem filesystem; auto parent = take(filesystem.open_directory(scratch.path())); auto entry = take(parent->open_child(name()));
    const auto id = take(take(entry->inspect_basic()).identity);
    fs::rename(file,moved); write(file); const auto report = entry->inspect_integrity();
#ifdef _WIN32
    if (compare_independent(moved, report)) {
        const auto value = take(report); CK_CHECK(compare_identity(take(value.owner),id) == IdentityComparison::Same);
        CK_CHECK(compare_identity(take(value.owner),take(take(filesystem.inspect_basic(file)).identity)) == IdentityComparison::Different);
        std::cout << "native-integrity: retained-object replacement isolation qualified\n";
    }
#else
    CK_CHECK(report.state() == ObservationState::Unsupported);
    CK_CHECK(compare_identity(take(take(entry->inspect_basic()).identity),id) == IdentityComparison::Same);
#endif
    CK_CHECK(fs::file_size(file) == 7 && fs::file_size(moved) == 7);
}
CK_TEST(native_integrity_object_inventory_preserves_query_result_and_small_buffer_refusal) {
    const ck::testing::ScratchDirectory scratch("files-integrity-inventory"); write(scratch.path()/"file");
    NativeFileSystem filesystem; auto parent = take(filesystem.open_directory(scratch.path())); auto entry = take(parent->open_child(name()));
    InventoryLimits invalid; invalid.max_native_bytes = 0; CK_CHECK(entry->inspect_integrity(invalid).state() == ObservationState::Failed);
    InventoryLimits small; small.max_native_bytes = 15; const auto limited = entry->inspect_integrity(small);
#ifdef _WIN32
    CK_CHECK(limited.state() == ObservationState::Unknown && limited.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit));
#else
    CK_CHECK(limited.state() == ObservationState::Unsupported);
#endif
    ObjectInventoryJob job(std::move(entry)); const auto result = finish(job);
#ifdef _WIN32
    if (compare_independent(scratch.path()/"file", result->data().integrity)) {
        const auto& report = take(result->data().integrity); CK_CHECK(take(result->compare_owner(report.owner)) == IdentityComparison::Same);
        std::cout << "native-integrity: object inventory and wire budget qualified\n";
    }
#else
    CK_CHECK(result->data().integrity.state() == ObservationState::Unsupported);
#endif
}
