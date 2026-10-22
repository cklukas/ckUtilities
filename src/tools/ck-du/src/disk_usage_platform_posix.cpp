// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Disk Usage file metadata on macOS and Linux: lstat, statx and the user and
// group databases.
#include "disk_usage_platform.hpp"

#include <cerrno>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace ck::du::platform
{

namespace
{

std::chrono::system_clock::time_point from_seconds(time_t seconds)
{
    return std::chrono::system_clock::from_time_t(seconds);
}

} // namespace

bool read_metadata(const std::filesystem::path &path, FileMetadata &metadata, std::error_code &error)
{
    struct stat sb{};
    if (lstat(path.c_str(), &sb) != 0)
    {
        error = std::error_code(errno, std::generic_category());
        return false;
    }
    error.clear();
    metadata = FileMetadata{};
    metadata.isDirectory = S_ISDIR(sb.st_mode);
    metadata.isSymlink = S_ISLNK(sb.st_mode);
    metadata.logicalSize = sb.st_size > 0 ? static_cast<std::uintmax_t>(sb.st_size) : 0;
    metadata.allocatedSize = sb.st_blocks > 0 ? static_cast<std::uintmax_t>(sb.st_blocks) * 512ULL : 0;
    metadata.device = static_cast<std::uintmax_t>(sb.st_dev);
    metadata.fileId = static_cast<std::uintmax_t>(sb.st_ino);
#ifdef UF_NODUMP
    metadata.noDump = (sb.st_flags & UF_NODUMP) != 0;
#endif
#if defined(__APPLE__) && defined(UF_OFFLINE)
    metadata.offlinePlaceholder = (sb.st_flags & UF_OFFLINE) != 0;
#endif
    metadata.modified = from_seconds(sb.st_mtime);
    metadata.created = from_seconds(sb.st_ctime);
#if defined(__APPLE__)
    metadata.created = from_seconds(sb.st_birthtimespec.tv_sec);
#elif defined(__linux__) && defined(STATX_BTIME)
    struct statx stx{};
    if (statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW | AT_STATX_SYNC_AS_STAT, STATX_BTIME, &stx) == 0 &&
        (stx.stx_mask & STATX_BTIME))
        metadata.created = from_seconds(static_cast<time_t>(stx.stx_btime.tv_sec)) +
                           std::chrono::duration_cast<std::chrono::system_clock::duration>(
                               std::chrono::nanoseconds(stx.stx_btime.tv_nsec));
#endif
    metadata.uid = static_cast<std::uint32_t>(sb.st_uid);
    metadata.gid = static_cast<std::uint32_t>(sb.st_gid);
    return true;
}

std::pair<std::string, std::string> owner_and_group(const std::filesystem::path &, const FileMetadata &metadata)
{
    std::pair<std::string, std::string> names;
    if (const passwd *pw = getpwuid(static_cast<uid_t>(metadata.uid)))
        names.first = pw->pw_name;
    else
        names.first = std::to_string(metadata.uid);
    if (const group *gr = getgrgid(static_cast<gid_t>(metadata.gid)))
        names.second = gr->gr_name;
    else
        names.second = std::to_string(metadata.gid);
    return names;
}

} // namespace ck::du::platform
