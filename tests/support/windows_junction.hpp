// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace ck::testing {
inline void make_junction(const std::filesystem::path& link, std::filesystem::path target) {
    target.make_preferred();
    const auto print = target.native();
    const auto substitute = std::wstring(L"\\??\\") + print;
    struct Header {
        DWORD tag;
        WORD data_length, reserved, substitute_offset, substitute_length, print_offset, print_length;
    };
    static_assert(sizeof(Header) == 16);
    const auto substitute_bytes = substitute.size() * sizeof(wchar_t);
    const auto print_bytes = print.size() * sizeof(wchar_t);
    const auto data_bytes = 8 + substitute_bytes + print_bytes + 2 * sizeof(wchar_t);
    if (data_bytes > 16000) throw std::runtime_error("junction fixture path too long");
    Header header{IO_REPARSE_TAG_MOUNT_POINT, static_cast<WORD>(data_bytes), 0, 0,
        static_cast<WORD>(substitute_bytes), static_cast<WORD>(substitute_bytes + sizeof(wchar_t)), static_cast<WORD>(print_bytes)};
    std::vector<std::byte> buffer(8 + data_bytes);
    std::memcpy(buffer.data(), &header, sizeof(header));
    std::memcpy(buffer.data() + sizeof(header), substitute.data(), substitute_bytes);
    std::memcpy(buffer.data() + sizeof(header) + header.print_offset, print.data(), print_bytes);
    struct CloseHandleOnExit {
        HANDLE value;
        ~CloseHandleOnExit() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    } handle{CreateFileW(link.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open junction fixture: " + std::to_string(GetLastError()));
    DWORD returned = 0;
    if (!DeviceIoControl(handle.value, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr))
        throw std::runtime_error("cannot set junction fixture: " + std::to_string(GetLastError()));
}
} // namespace ck::testing
#endif
