// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// What Disk Usage needs to know about one file, independent of the platform.
// disk_usage_platform_posix.cpp reads it with lstat, disk_usage_platform_windows.cpp
// with the Win32 file-information and security APIs.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace ck::du::platform
{

struct FileMetadata
{
    bool isDirectory = false;
    // A symbolic link, or on Windows a junction; never followed.
    bool isSymlink = false;
    std::uintmax_t logicalSize = 0;
    // Bytes the file occupies on disk (allocation units), which can be
    // smaller (sparse, compressed, cloud placeholder) or larger than logicalSize.
    std::uintmax_t allocatedSize = 0;
    // The volume, and the file's identity on it: together they recognise hard
    // links, and the volume alone a filesystem boundary.
    std::uintmax_t device = 0;
    std::uintmax_t fileId = 0;
    // BSD "do not back up" flag.
    bool noDump = false;
    // The data lives elsewhere (macOS UF_OFFLINE; Windows offline/recall attributes).
    bool offlinePlaceholder = false;
    std::chrono::system_clock::time_point modified{};
    // Creation time where the platform records it, else the last status change.
    std::chrono::system_clock::time_point created{};
    // POSIX owner and group ids, for owner_and_group().
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
};

// Reads `path` without following a final symbolic link.
bool read_metadata(const std::filesystem::path &path, FileMetadata &metadata, std::error_code &error);

// The owning user's and group's names, or their numeric ids when unnamed.
std::pair<std::string, std::string> owner_and_group(const std::filesystem::path &path, const FileMetadata &metadata);

// Shell-style matching as fnmatch(3) without flags: '*' and '?' match any
// characters (including '/'), "[...]" a set or range, "[!...]" its complement,
// and a backslash makes the next character literal.
bool glob_match(const std::string &pattern, const std::string &value);

} // namespace ck::du::platform
