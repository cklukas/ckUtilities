#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "disk_usage_core.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <grp.h>
#include <iomanip>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

namespace ck::du
{
namespace
{
namespace fs = std::filesystem;

SizeUnit gCurrentUnit = SizeUnit::Auto;
SortKey gCurrentSortKey = SortKey::Unsorted;

std::string formatTimePoint(const std::chrono::system_clock::time_point &tp)
{
    if (tp.time_since_epoch().count() == 0)
        return "-";

    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    std::tm *tmp = std::localtime(&tt);
    if (!tmp)
        return "-";
    tm = *tmp;
#else
    if (!localtime_r(&tt, &tm))
        return "-";
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M");
    return out.str();
}

std::string ownerName(uid_t uid)
{
    if (passwd *pw = getpwuid(uid))
        return pw->pw_name;
    return std::to_string(uid);
}

std::string groupName(gid_t gid)
{
    if (group *gr = getgrgid(gid))
        return gr->gr_name;
    return std::to_string(gid);
}

DirectoryStats populateNode(DirectoryNode &node, const fs::path &path)
{
    node.path = path;
    DirectoryStats stats{};
    std::error_code ec;

    struct stat sb;
    if (stat(path.c_str(), &sb) == 0)
        node.modifiedTime = std::chrono::system_clock::from_time_t(sb.st_mtime);
    else
        node.modifiedTime = std::chrono::system_clock::time_point{};

    fs::directory_iterator endIter;
    fs::directory_iterator it(path, fs::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        node.stats = stats;
        return stats;
    }

    for (; it != endIter; it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }

        const fs::directory_entry &entry = *it;
        std::error_code entryEc;
        const fs::path &entryPath = entry.path();

        if (entry.is_symlink(entryEc))
        {
            if (!entryEc && entry.is_directory(entryEc))
                continue;
        }

        if (!entryEc && entry.is_directory(entryEc))
        {
            auto child = std::make_unique<DirectoryNode>();
            child->parent = &node;
            child->expanded = false;
            DirectoryStats childStats = populateNode(*child, entryPath);
            child->stats = childStats;
            stats.totalSize += childStats.totalSize;
            stats.fileCount += childStats.fileCount;
            stats.directoryCount += childStats.directoryCount + 1;
            node.children.push_back(std::move(child));
        }
        else
        {
            std::uintmax_t fileSize = 0;
            if (!entryEc && entry.is_regular_file(entryEc))
                fileSize = entry.file_size(entryEc);
            stats.totalSize += fileSize;
            ++stats.fileCount;
        }
    }

    node.stats = stats;
    return stats;
}

FileEntry makeFileEntry(const fs::path &path, const fs::path &base)
{
    FileEntry entry;
    entry.path = path;
    entry.displayPath = path.lexically_relative(base).string();
    if (entry.displayPath.empty())
        entry.displayPath = path.filename().string();

    struct stat sb;
    if (stat(path.c_str(), &sb) == 0)
    {
        entry.size = static_cast<std::uintmax_t>(sb.st_size);
        entry.owner = ownerName(sb.st_uid);
        entry.group = groupName(sb.st_gid);
        entry.modifiedTime = std::chrono::system_clock::from_time_t(sb.st_mtime);
        entry.modified = formatTimePoint(entry.modifiedTime);
#if defined(__linux__) && defined(STATX_BTIME)
        struct statx stx;
        if (statx(AT_FDCWD, path.c_str(), AT_STATX_SYNC_AS_STAT, STATX_BTIME, &stx) == 0 &&
            (stx.stx_mask & STATX_BTIME))
        {
            entry.createdTime =
                std::chrono::system_clock::from_time_t(stx.stx_btime.tv_sec) +
                std::chrono::nanoseconds(stx.stx_btime.tv_nsec);
            entry.created = formatTimePoint(entry.createdTime);
        }
        else
#endif
        {
            entry.createdTime = std::chrono::system_clock::from_time_t(sb.st_ctime);
            entry.created = formatTimePoint(entry.createdTime);
        }
    }
    else
    {
        entry.size = 0;
        entry.owner = "?";
        entry.group = "?";
        entry.created = "-";
        entry.modified = "-";
        entry.createdTime = std::chrono::system_clock::time_point{};
        entry.modifiedTime = std::chrono::system_clock::time_point{};
    }

    if (entry.displayPath.empty())
        entry.displayPath = path.filename().string();
    if (entry.displayPath.empty())
        entry.displayPath = path.string();

    return entry;
}

} // namespace

std::unique_ptr<DirectoryNode> buildDirectoryTree(const std::filesystem::path &rootPath)
{
    auto root = std::make_unique<DirectoryNode>();
    root->parent = nullptr;
    root->expanded = true;

    DirectoryStats stats = populateNode(*root, fs::absolute(rootPath));
    root->stats = stats;
    return root;
}

std::vector<FileEntry> listFiles(const std::filesystem::path &directory, bool recursive)
{
    std::vector<FileEntry> files;
    const fs::path base = fs::absolute(directory);
    std::error_code ec;

    if (recursive)
    {
        fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        if (ec)
            return files;

        for (; it != end; it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }

            const fs::directory_entry &entry = *it;
            std::error_code entryEc;
            if (entry.is_directory(entryEc) && !entryEc)
                continue;
            if (!entryEc && entry.is_regular_file(entryEc))
                files.push_back(makeFileEntry(entry.path(), base));
        }
    }
    else
    {
        fs::directory_iterator it(base, fs::directory_options::skip_permission_denied, ec);
        fs::directory_iterator end;
        if (ec)
            return files;

        for (; it != end; it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }

            const fs::directory_entry &entry = *it;
            std::error_code entryEc;
            if (!entryEc && entry.is_regular_file(entryEc))
                files.push_back(makeFileEntry(entry.path(), base));
        }
    }

    return files;
}

SizeUnit getCurrentUnit() noexcept
{
    return gCurrentUnit;
}

void setCurrentUnit(SizeUnit unit) noexcept
{
    gCurrentUnit = unit;
}

const char *unitName(SizeUnit unit) noexcept
{
    switch (unit)
    {
    case SizeUnit::Auto:
        return "Auto";
    case SizeUnit::Bytes:
        return "Bytes";
    case SizeUnit::Kilobytes:
        return "Kilobytes";
    case SizeUnit::Megabytes:
        return "Megabytes";
    case SizeUnit::Gigabytes:
        return "Gigabytes";
    case SizeUnit::Terabytes:
        return "Terabytes";
    case SizeUnit::Blocks:
        return "Blocks";
    }
    return "";
}

std::string formatSize(std::uintmax_t bytes, SizeUnit unit)
{
    auto renderValue = [](double value) {
        std::ostringstream out;
        if (value >= 100)
            out << std::fixed << std::setprecision(0);
        else if (value >= 10)
            out << std::fixed << std::setprecision(1);
        else
            out << std::fixed << std::setprecision(2);
        out << value;
        return out.str();
    };

    SizeUnit effectiveUnit = unit;
    if (unit == SizeUnit::Auto)
    {
        if (bytes >= (1ULL << 40))
            effectiveUnit = SizeUnit::Terabytes;
        else if (bytes >= (1ULL << 30))
            effectiveUnit = SizeUnit::Gigabytes;
        else if (bytes >= (1ULL << 20))
            effectiveUnit = SizeUnit::Megabytes;
        else if (bytes >= (1ULL << 10))
            effectiveUnit = SizeUnit::Kilobytes;
        else
            effectiveUnit = SizeUnit::Bytes;
    }

    switch (effectiveUnit)
    {
    case SizeUnit::Auto:
        break;
    case SizeUnit::Bytes:
    {
        std::ostringstream out;
        out << bytes << " B";
        return out.str();
    }
    case SizeUnit::Kilobytes:
        return renderValue(static_cast<double>(bytes) / 1024.0) + " KB";
    case SizeUnit::Megabytes:
        return renderValue(static_cast<double>(bytes) / (1024.0 * 1024.0)) + " MB";
    case SizeUnit::Gigabytes:
        return renderValue(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) + " GB";
    case SizeUnit::Terabytes:
        return renderValue(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0 * 1024.0)) + " TB";
    case SizeUnit::Blocks:
    {
        std::uintmax_t blocks = (bytes + 511) / 512;
        std::ostringstream out;
        out << blocks << " blocks";
        return out.str();
    }
    }
    return std::to_string(bytes) + " B";
}

SortKey getCurrentSortKey() noexcept
{
    return gCurrentSortKey;
}

void setCurrentSortKey(SortKey key) noexcept
{
    gCurrentSortKey = key;
}

const char *sortKeyName(SortKey key) noexcept
{
    switch (key)
    {
    case SortKey::Unsorted:
        return "Unsorted";
    case SortKey::NameAscending:
        return "Name (A→Z)";
    case SortKey::NameDescending:
        return "Name (Z→A)";
    case SortKey::SizeDescending:
        return "Size (Largest)";
    case SortKey::SizeAscending:
        return "Size (Smallest)";
    case SortKey::ModifiedDescending:
        return "Modified (Newest)";
    case SortKey::ModifiedAscending:
        return "Modified (Oldest)";
    }
    return "";
}

} // namespace ck::du

