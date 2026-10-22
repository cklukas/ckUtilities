// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Disk Usage file metadata on Windows, from Microsoft's documentation of
// CreateFileW (FILE_FLAG_OPEN_REPARSE_POINT, FILE_FLAG_BACKUP_SEMANTICS),
// GetFileInformationByHandleEx (FileBasicInfo, FileStandardInfo,
// FileAttributeTagInfo, FileIdInfo), GetNamedSecurityInfoW and
// LookupAccountSidW.
#include "disk_usage_platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <cstring>
#include <memory>

namespace ck::du::platform
{

namespace
{

struct HandleCloser
{
    void operator()(HANDLE handle) const noexcept
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};

// FILETIME-style 100 ns ticks since 1601 as a system_clock time point.
std::chrono::system_clock::time_point from_file_time(LARGE_INTEGER ticks)
{
    constexpr long long kUnixEpochTicks = 116444736000000000LL;
    if (ticks.QuadPart <= 0)
        return {};
    const auto since_unix = std::chrono::duration<long long, std::ratio<1, 10000000>>(ticks.QuadPart - kUnixEpochTicks);
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(since_unix));
}

std::string narrow(const wchar_t *text, int length = -1)
{
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), bytes, nullptr, nullptr);
    if (length < 0 && !result.empty() && result.back() == '\0')
        result.pop_back();
    return result;
}

// "DOMAIN\name" for a resolvable account, its SID text otherwise.
std::string account_name(PSID sid)
{
    if (sid == nullptr)
        return {};
    wchar_t name[256];
    wchar_t domain[256];
    DWORD name_length = 256;
    DWORD domain_length = 256;
    SID_NAME_USE use{};
    if (LookupAccountSidW(nullptr, sid, name, &name_length, domain, &domain_length, &use))
        return domain_length > 0 ? narrow(domain) + "\\" + narrow(name) : narrow(name);
    wchar_t *text = nullptr;
    std::string result;
    if (ConvertSidToStringSidW(sid, &text))
    {
        result = narrow(text);
        LocalFree(text);
    }
    return result;
}

} // namespace

bool read_metadata(const std::filesystem::path &path, FileMetadata &metadata, std::error_code &error)
{
    // Directories need backup semantics; a reparse point is opened itself,
    // never its target.
    const std::unique_ptr<void, HandleCloser> file(CreateFileW(
        path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
    {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard, sizeof(standard)))
    {
        error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return false;
    }
    error.clear();
    metadata = FileMetadata{};
    metadata.isDirectory = standard.Directory != FALSE;
    metadata.logicalSize = standard.EndOfFile.QuadPart > 0 ? static_cast<std::uintmax_t>(standard.EndOfFile.QuadPart) : 0;
    metadata.allocatedSize =
        standard.AllocationSize.QuadPart > 0 ? static_cast<std::uintmax_t>(standard.AllocationSize.QuadPart) : 0;
    metadata.modified = from_file_time(basic.LastWriteTime);
    metadata.created = from_file_time(basic.CreationTime);

    const DWORD attributes = basic.FileAttributes;
    metadata.offlinePlaceholder =
        (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        // Only links are links: cloud-file placeholders and deduplicated
        // files are reparse points too, and must be counted as files.
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag, sizeof(tag)))
            metadata.isSymlink = tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;
    }

    // The 128-bit file id where the filesystem provides one (ReFS), else the
    // classic 64-bit index; either way unique together with the volume.
    FILE_ID_INFO id{};
    if (GetFileInformationByHandleEx(file.get(), FileIdInfo, &id, sizeof(id)))
    {
        metadata.device = static_cast<std::uintmax_t>(id.VolumeSerialNumber);
        std::uint64_t low = 0;
        std::uint64_t high = 0;
        std::memcpy(&low, id.FileId.Identifier, sizeof(low));
        std::memcpy(&high, id.FileId.Identifier + sizeof(low), sizeof(high));
        metadata.fileId = static_cast<std::uintmax_t>(low ^ (high * 0x9E3779B97F4A7C15ULL));
    }
    else
    {
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(file.get(), &info))
        {
            metadata.device = info.dwVolumeSerialNumber;
            metadata.fileId = (static_cast<std::uintmax_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
        }
    }
    return true;
}

std::pair<std::string, std::string> owner_and_group(const std::filesystem::path &path, const FileMetadata &)
{
    PSID owner = nullptr;
    PSID group = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    std::wstring native = path.wstring();
    std::pair<std::string, std::string> names;
    if (GetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
                              &owner, &group, nullptr, nullptr, &descriptor) == ERROR_SUCCESS)
    {
        names.first = account_name(owner);
        names.second = account_name(group);
        LocalFree(descriptor);
    }
    if (names.first.empty())
        names.first = "-";
    if (names.second.empty())
        names.second = "-";
    return names;
}

} // namespace ck::du::platform
