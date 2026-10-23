// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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
#endif

namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template <class T>
const T& required(const Observation<T>& result) {
    if (!result.value()) {
        const auto& error = *result.unavailable_detail();
        std::cerr << "channel query unavailable: " << static_cast<int>(error.state) << " " << error.error.operation
                  << " " << error.error.code << std::endl;
        throw std::runtime_error("required native channel observation unavailable");
    }
    return *result.value();
}
void write_file(const fs::path& path, const std::string& bytes = "main data") {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("cannot write channel fixture");
}
Observation<DataChannelCatalog> inspect_channels(const OpenedEntry& entry, InventoryLimits limits = {}) {
    try { return entry.inspect_data_channels(limits); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "channel adapter exception: %s\n", error.what());
        std::fflush(stderr);
        throw;
    }
}
std::unique_ptr<OpenedEntry> open_file(const NativeFileSystem& filesystem, const fs::path& path) {
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
    if (!name) throw std::runtime_error("invalid channel fixture name");
    auto opened = parent->open_child(*name);
    (void)required(opened);
    return std::move(opened).take_value();
}
}

CK_TEST(native_channel_query_rejects_invalid_limits_and_keeps_unsupported_distinct_from_empty) {
    const ck::testing::ScratchDirectory scratch("files-channels-contract");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_file(filesystem, path);
    for (auto invalid : {InventoryLimits{0, 1}, InventoryLimits{1, 0}, InventoryLimits{1, InventoryLimits::hard_records + 1}}) {
        const auto result = inspect_channels(*opened, invalid);
        CK_CHECK(result.state() == ObservationState::Failed && !result.value());
        CK_CHECK(result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::InvalidLimit));
    }
#if defined(__linux__)
    const auto result = inspect_channels(*opened);
    CK_CHECK(result.state() == ObservationState::Unsupported && !result.value());
    CK_CHECK(result.unavailable_detail()->error.domain == ErrorDomain::Adapter);
    CK_CHECK(result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::UnsupportedQuery));
#else
    const auto result = inspect_channels(*opened);
    const auto metadata = opened->inspect_basic();
    CK_CHECK(compare_identity(required(required(result).owner), required(required(metadata).identity)) == IdentityComparison::Same);
#endif
}

#if defined(_WIN32)
CK_TEST(native_windows_channels_enumerate_main_empty_large_and_unpaired_utf16_streams_across_buffer_growth) {
    const ck::testing::ScratchDirectory scratch("files-channels-windows");
    const auto path = scratch.path() / "file";
    write_file(path);
    std::map<std::u16string, std::uint64_t> expected;
    expected.emplace(u"::$DATA", 9);
    for (int index = 0; index < 64; ++index) {
        const auto ascii = "channel-" + std::to_string(index) + "-" + std::string(80, 'a');
        const std::wstring stream(ascii.begin(), ascii.end());
        const auto contents = std::string(static_cast<std::size_t>(index), 'x');
        write_file(fs::path(path.native() + L":" + stream), contents);
        expected.emplace(std::u16string(u":") + std::u16string(ascii.begin(), ascii.end()) + u":$DATA", contents.size());
    }
    std::wstring unusual = L"native-";
    unusual.push_back(static_cast<wchar_t>(0xD800));
    write_file(fs::path(path.native() + L":" + unusual), "native");
    std::u16string raw = u":";
    for (const auto unit : unusual) raw.push_back(static_cast<char16_t>(unit));
    raw += u":$DATA";
    expected.emplace(raw, 6);
    constexpr std::uint64_t large_size = 10 * 1024 * 1024;
    {
        std::ofstream large(fs::path(path.native() + L":large"), std::ios::binary);
        large.seekp(static_cast<std::streamoff>(large_size - 1));
        large.put('x');
        large.close();
        if (!large) throw std::runtime_error("cannot create large channel fixture");
    }
    expected.emplace(u":large:$DATA", large_size);
    NativeFileSystem filesystem;
    auto opened = open_file(filesystem, path);
    const auto result = inspect_channels(*opened);
    const auto& catalog = required(result);
    CK_CHECK(catalog.coverage == DataChannelCoverage::WindowsFileStreamInfo);
    std::map<std::u16string, std::uint64_t> observed;
    for (const auto& channel : catalog.channels) {
        CK_CHECK(channel.identifier.windows_units() != nullptr);
        CK_CHECK(channel.allocated_size.state() == ObservationState::Available);
        const auto& id = *channel.identifier.windows_units();
        CK_CHECK(channel.kind == (id == u"::$DATA" ? DataChannelKind::MainData : DataChannelKind::NamedData));
        CK_CHECK(observed.emplace(id, required(channel.logical_size)).second);
    }
    CK_CHECK(observed == expected);
    const auto buffer_limit = inspect_channels(*opened, {128, 16384});
    CK_CHECK(buffer_limit.state() == ObservationState::Unknown && !buffer_limit.value());
    const auto record_limit = inspect_channels(*opened, {1024 * 1024, 1});
    CK_CHECK(record_limit.state() == ObservationState::Unknown && !record_limit.value());
}

CK_TEST(native_windows_channel_query_keeps_the_opened_owner_after_rename_replacement_and_unlink) {
    const ck::testing::ScratchDirectory scratch("files-channels-windows-pin");
    const auto path = scratch.path() / "file";
    write_file(path);
    write_file(fs::path(path.native() + L":retained"), "additional");
    NativeFileSystem filesystem;
    auto opened = open_file(filesystem, path);
    const auto before = opened->inspect_basic();
    fs::rename(path, scratch.path() / "retired");
    write_file(path, "replacement");
    fs::remove(scratch.path() / "retired");
    const auto result = inspect_channels(*opened);
    CK_CHECK(compare_identity(required(required(result).owner), required(required(before).identity)) == IdentityComparison::Same);
    bool retained = false;
    for (const auto& channel : required(result).channels)
        if (*channel.identifier.windows_units() == u":retained:$DATA") retained = required(channel.logical_size) == 10;
    CK_CHECK(retained);
}

CK_TEST(native_windows_directory_channels_are_queried_on_the_directory_handle) {
    const ck::testing::ScratchDirectory scratch("files-channels-windows-directory");
    const auto path = scratch.path() / "directory";
    fs::create_directory(path);
    NativeFileSystem filesystem;
    auto opened = open_file(filesystem, path);
    // Independent native enumeration establishes the no-stream fixture. The
    // product uses FileStreamInfo and must retain its documented EOF outcome.
    WIN32_FIND_STREAM_DATA stream_info{};
    const HANDLE search = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &stream_info, 0);
    const DWORD search_error = GetLastError();
    if (search != INVALID_HANDLE_VALUE) FindClose(search);
    CK_CHECK(search == INVALID_HANDLE_VALUE && search_error == ERROR_HANDLE_EOF);
    CK_CHECK(required(inspect_channels(*opened)).channels.empty());
    write_file(fs::path(path.native() + L":directory-data"), "directory channel");
    const auto result = inspect_channels(*opened);
    bool found = false;
    for (const auto& channel : required(result).channels)
        if (*channel.identifier.windows_units() == u":directory-data:$DATA") found = required(channel.logical_size) == 17;
    CK_CHECK(found);
}
#elif defined(__APPLE__)
CK_TEST(native_macos_resource_fork_catalog_distinguishes_absence_and_tracks_the_opened_owner) {
    const ck::testing::ScratchDirectory scratch("files-channels-macos");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_file(filesystem, path);
    const auto absent = inspect_channels(*opened);
    CK_CHECK(required(absent).coverage == DataChannelCoverage::MacResourceFork && required(absent).channels.empty());
    // Independent named-fork writer; the product query uses fgetxattr on its fd.
    write_file(path / "..namedfork" / "rsrc", std::string("fork\0data", 9));
    const auto before = opened->inspect_basic();
    fs::rename(path, scratch.path() / "retired");
    write_file(path, "replacement");
    fs::remove(scratch.path() / "retired");
    const auto result = inspect_channels(*opened);
    const auto& catalog = required(result);
    CK_CHECK(catalog.coverage == DataChannelCoverage::MacResourceFork && catalog.channels.size() == 1);
    CK_CHECK(catalog.channels[0].kind == DataChannelKind::ResourceFork);
    CK_CHECK(*catalog.channels[0].identifier.posix_bytes() == "com.apple.ResourceFork");
    CK_CHECK(required(catalog.channels[0].logical_size) == 9);
    CK_CHECK(catalog.channels[0].allocated_size.state() == ObservationState::Unknown);
    CK_CHECK(compare_identity(required(catalog.owner), required(required(before).identity)) == IdentityComparison::Same);
}

CK_TEST(native_macos_resource_fork_size_query_is_live_and_does_not_buffer_channel_contents) {
    const ck::testing::ScratchDirectory scratch("files-channels-macos-live");
    const auto path = scratch.path() / "file";
    write_file(path);
    write_file(path / "..namedfork" / "rsrc", "small");
    NativeFileSystem filesystem;
    auto opened = open_file(filesystem, path);
    CK_CHECK(required(required(inspect_channels(*opened)).channels[0].logical_size) == 5);
    constexpr std::uint64_t large_size = 10 * 1024 * 1024;
    std::ofstream fork(path / "..namedfork" / "rsrc", std::ios::binary);
    fork.seekp(static_cast<std::streamoff>(large_size - 1));
    fork.put('x');
    fork.close();
    if (!fork) throw std::runtime_error("cannot create large fork fixture");
    const auto result = inspect_channels(*opened, {1, 1});
    CK_CHECK(required(required(result).channels[0].logical_size) == large_size);
}

CK_TEST(native_macos_resource_fork_query_does_not_follow_a_pinned_final_symlink) {
    const ck::testing::ScratchDirectory scratch("files-channels-macos-link");
    const auto target = scratch.path() / "target";
    write_file(target);
    write_file(target / "..namedfork" / "rsrc", "target fork");
    fs::create_symlink("target", scratch.path() / "link");
    NativeFileSystem filesystem;
    auto link = open_file(filesystem, scratch.path() / "link");
    const auto result = inspect_channels(*link);
    CK_CHECK(required(result).channels.empty());
}
#endif
