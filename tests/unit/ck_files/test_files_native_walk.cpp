// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_directory_walk.hpp"

#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "windows_name.hpp"
#include <windows.h>
#else
#include <fstream>
#endif

namespace {
using namespace ck::files;
namespace fs = std::filesystem;

#if defined(_WIN32)
void create_directory_wide(const std::wstring& path) {
    const auto extended = windows_extended_path(path);
    if (!extended) throw std::runtime_error("cannot qualify long directory fixture");
    if (!CreateDirectoryW(extended->c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw std::runtime_error("cannot create long directory fixture");
}

void create_file_wide(const std::wstring& path) {
    const auto extended = windows_extended_path(path);
    if (!extended) throw std::runtime_error("cannot qualify long file fixture");
    const auto handle = CreateFileW(extended->c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create long file fixture");
    const char byte = 'x';
    DWORD written = 0;
    const bool written_ok = WriteFile(handle, &byte, 1, &written, nullptr) && written == 1;
    CloseHandle(handle);
    if (!written_ok) throw std::runtime_error("cannot write long file fixture");
}
#endif
} // namespace

CK_TEST(native_directory_walk_handles_a_path_beyond_legacy_windows_limit) {
    const ck::testing::ScratchDirectory scratch("files-native-walk");
    const auto root = scratch.path();

#if defined(_WIN32)
    std::wstring current = root.native();
    const std::wstring component(90, L'x');
    for (int index = 0; index < 3; ++index) {
        current += L"\\" + component + std::to_wstring(index);
        create_directory_wide(current);
    }
    create_file_wide(current + L"\\long-fixture.txt");
#else
    auto current = root;
    const std::string component(90, 'x');
    for (int index = 0; index < 3; ++index) {
        current /= component + std::to_string(index);
        fs::create_directory(current);
    }
    std::ofstream file(current / "long-fixture.txt", std::ios::binary);
    file << 'x';
    if (!file) throw std::runtime_error("cannot write long file fixture");
#endif

    std::size_t long_file_count = 0;
    std::size_t long_directory_count = 0;
    const auto result = walk_native_directories(root, true, false, false,
        [&](const NativeWalkEntry& entry) {
            const auto units = entry.address.windows_units();
            const auto bytes = entry.address.posix_bytes();
            const auto length = units ? units->size() : bytes->size();
            if (length > 260) {
                if (entry.metadata.kind.value() && *entry.metadata.kind.value() == ObjectKind::RegularFile)
                    ++long_file_count;
                if (entry.metadata.kind.value() && *entry.metadata.kind.value() == ObjectKind::Directory)
                    ++long_directory_count;
            }
            return NativeWalkAction::Continue;
        },
        [&](const fs::path&, std::error_code error) {
            throw std::system_error(error, "native long-path walk");
        },
        [] { return false; },
        [](const fs::path&, int) {});

    CK_CHECK(result.completed);
    CK_CHECK(!result.cancelled);
    CK_CHECK(long_directory_count >= 1);
    CK_CHECK(long_file_count == 1);
}
