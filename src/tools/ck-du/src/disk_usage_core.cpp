#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "disk_usage_core.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <grp.h>
#include <iomanip>
#include <map>
#include <pwd.h>
#include <sstream>
#include <string>
#include <unordered_set>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>

#if !defined(_WIN32)
#include <fnmatch.h>
#endif

namespace ck::du
{
namespace
{
namespace fs = std::filesystem;

SizeUnit gCurrentUnit = SizeUnit::Auto;
SortKey gCurrentSortKey = SortKey::Unsorted;

struct FileIdentity
{
    std::uintmax_t device = 0;
    std::uintmax_t inode = 0;

    bool operator==(const FileIdentity &) const noexcept = default;
};

struct FileIdentityHash
{
    std::size_t operator()(const FileIdentity &id) const noexcept
    {
        std::size_t h1 = std::hash<std::uintmax_t>{}(id.device);
        std::size_t h2 = std::hash<std::uintmax_t>{}(id.inode);
        return h1 ^ (h2 << 1);
    }
};

struct ScanContext
{
    const BuildDirectoryTreeOptions &options;
    std::unordered_set<FileIdentity, FileIdentityHash> visited;
    std::uintmax_t rootDevice = 0;
    fs::path rootPath;
};

std::string trimWhitespace(const std::string &value)
{
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        ++start;
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(start, end - start);
}

std::string shellEscape(const std::string &text)
{
    std::string escaped = "'";
    for (char ch : text)
    {
        if (ch == '\'')
            escaped += "'\\''";
        else
            escaped.push_back(ch);
    }
    escaped.push_back('\'');
    return escaped;
}

std::string extensionFallback(const fs::path &path)
{
    std::string ext = path.extension().string();
    if (!ext.empty())
    {
        std::string lowered;
        lowered.reserve(ext.size());
        for (char ch : ext)
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        return lowered;
    }
    return "unknown";
}

std::string detectFileType(const fs::path &path)
{
    std::string command = "file -b --mime-type " + shellEscape(path.string());
    std::array<char, 256> buffer{};
    std::string output;
    if (FILE *pipe = popen(command.c_str(), "r"))
    {
        while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
            output.append(buffer.data());
        pclose(pipe);
        output = trimWhitespace(output);
        if (!output.empty())
            return output;
    }
    return extensionFallback(path);
}

bool hasNoDumpFlag(const struct stat &sb)
{
#ifdef UF_NODUMP
    return (sb.st_flags & UF_NODUMP) != 0;
#else
    (void)sb;
    return false;
#endif
}

bool matchPattern(const std::string &pattern, const std::string &value)
{
#if !defined(_WIN32)
    return fnmatch(pattern.c_str(), value.c_str(), 0) == 0;
#else
    const char *p = pattern.c_str();
    const char *s = value.c_str();
    const char *star = nullptr;
    const char *ss = nullptr;
    while (*s)
    {
        if (*p == '*')
        {
            star = p++;
            ss = s;
        }
        else if (*p == '?' || *p == *s)
        {
            ++p;
            ++s;
        }
        else if (star)
        {
            p = star + 1;
            s = ++ss;
        }
        else
        {
            return false;
        }
    }
    while (*p == '*')
        ++p;
    return *p == '\0';
#endif
}

bool shouldIgnorePath(const fs::path &path, const ScanContext &context)
{
    if (context.options.ignoreMasks.empty())
        return false;

    std::string filename = path.filename().string();
    fs::path relativePath = path.lexically_relative(context.rootPath);
    std::string relativeString;
    if (relativePath.empty() || relativePath.native() == ".")
        relativeString = filename;
    else
        relativeString = relativePath.generic_string();

    for (const auto &pattern : context.options.ignoreMasks)
    {
        if (pattern.empty())
            continue;
        if (matchPattern(pattern, filename))
            return true;
        if (!relativeString.empty() && matchPattern(pattern, relativeString))
            return true;
    }
    return false;
}

std::uintmax_t fileSizeFromStat(const struct stat &sb)
{
    if (sb.st_size < 0)
        return 0;
    return static_cast<std::uintmax_t>(sb.st_size);
}

bool passesThreshold(std::uintmax_t size, const BuildDirectoryTreeOptions &options)
{
    if (options.threshold == 0)
        return true;
    std::uintmax_t threshold = static_cast<std::uintmax_t>(std::llabs(options.threshold));
    if (options.threshold > 0)
        return size >= threshold;
    return size <= threshold;
}

void reportError(const ScanContext &context, const fs::path &path, const std::error_code &ec)
{
    if (!context.options.reportErrors)
        return;
    if (context.options.errorCallback)
        context.options.errorCallback(path, ec);
}

ScanContext makeScanContext(const fs::path &root, const BuildDirectoryTreeOptions &options)
{
    ScanContext context{options};
    context.rootPath = root;
    struct stat sb;
    if (stat(root.c_str(), &sb) == 0)
        context.rootDevice = static_cast<std::uintmax_t>(sb.st_dev);
    return context;
}

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

struct ScanCancelled
{
};

DirectoryStats populateNode(DirectoryNode &node, const fs::path &path, ScanContext &context)
{
    if (context.options.cancelRequested && context.options.cancelRequested())
        throw ScanCancelled{};
    if (context.options.progressCallback)
        context.options.progressCallback(path);

    node.path = path;
    DirectoryStats stats{};

    struct stat sb;
    if (stat(path.c_str(), &sb) == 0)
        node.modifiedTime = std::chrono::system_clock::from_time_t(sb.st_mtime);
    else
        node.modifiedTime = std::chrono::system_clock::time_point{};

    if (context.options.ignoreNodumpFlag && hasNoDumpFlag(sb))
        return stats;

    if (!context.options.countHardLinksMultipleTimes)
    {
        FileIdentity identity{static_cast<std::uintmax_t>(sb.st_dev), static_cast<std::uintmax_t>(sb.st_ino)};
        auto [it, inserted] = context.visited.insert(identity);
        if (!inserted)
            return stats;
    }

    fs::directory_options dirOptions = fs::directory_options::skip_permission_denied;
    if (context.options.symlinkPolicy == BuildDirectoryTreeOptions::SymlinkPolicy::Always)
        dirOptions |= fs::directory_options::follow_directory_symlink;

    std::error_code ec;
    fs::directory_iterator it(path, dirOptions, ec);
    if (ec)
    {
        reportError(context, path, ec);
        node.stats = stats;
        return stats;
    }

    fs::directory_iterator endIter;
    for (; it != endIter; it.increment(ec))
    {
        if (ec)
        {
            reportError(context, path, ec);
            ec.clear();
            continue;
        }

        if (context.options.cancelRequested && context.options.cancelRequested())
            throw ScanCancelled{};

        const fs::directory_entry &entry = *it;
        const fs::path &entryPath = entry.path();

        if (shouldIgnorePath(entryPath, context))
            continue;

        std::error_code entryEc;
        bool isDirectory = entry.is_directory(entryEc);
        bool isSymlink = entry.is_symlink(entryEc);
        if (entryEc)
        {
            reportError(context, entryPath, entryEc);
            continue;
        }

        struct stat entryStat;
        if (stat(entryPath.c_str(), &entryStat) != 0)
        {
            reportError(context, entryPath, std::error_code(errno, std::generic_category()));
            continue;
        }

        if (context.options.ignoreNodumpFlag && hasNoDumpFlag(entryStat))
            continue;

        if (context.options.stayOnFilesystem && context.rootDevice != 0 &&
            static_cast<std::uintmax_t>(entryStat.st_dev) != context.rootDevice)
        {
            continue;
        }

        if (isDirectory)
        {
            if (isSymlink && context.options.symlinkPolicy != BuildDirectoryTreeOptions::SymlinkPolicy::Always)
                continue;

            auto childNode = std::make_unique<DirectoryNode>();
            childNode->parent = &node;
            childNode->expanded = false;
            DirectoryStats childStats = populateNode(*childNode, entryPath, context);
            childNode->stats = childStats;

            stats.totalSize += childStats.totalSize;
            stats.fileCount += childStats.fileCount;
            stats.directoryCount += childStats.directoryCount + 1;

            if (passesThreshold(childStats.totalSize, context.options))
                node.children.push_back(std::move(childNode));
        }
        else
        {
            bool count = true;
            if (!context.options.countHardLinksMultipleTimes)
            {
                FileIdentity identity{static_cast<std::uintmax_t>(entryStat.st_dev),
                                       static_cast<std::uintmax_t>(entryStat.st_ino)};
                auto [it2, inserted] = context.visited.insert(identity);
                count = inserted;
            }
            if (!count)
                continue;

            std::uintmax_t fileSize = fileSizeFromStat(entryStat);
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

BuildDirectoryTreeResult buildDirectoryTree(const std::filesystem::path &rootPath,
                                           const BuildDirectoryTreeOptions &options)
{
    BuildDirectoryTreeResult result;
    std::error_code ec;
    fs::path basePath = fs::absolute(rootPath, ec);
    if (ec)
        basePath = rootPath;

    fs::path scanPath = basePath;
    if (options.followCommandLineSymlinks)
    {
        std::error_code symEc;
        if (fs::is_symlink(basePath, symEc))
        {
            fs::path resolved = fs::weakly_canonical(basePath, symEc);
            if (!symEc)
                scanPath = resolved;
        }
    }

    scanPath = fs::absolute(scanPath, ec);
    if (ec)
        scanPath = basePath;

    auto root = std::make_unique<DirectoryNode>();
    root->parent = nullptr;
    root->expanded = true;

    ScanContext context = makeScanContext(scanPath, options);
    context.rootPath = scanPath;

    try
    {
        DirectoryStats stats = populateNode(*root, scanPath, context);
        root->stats = stats;
        result.root = std::move(root);
    }
    catch (const ScanCancelled &)
    {
        result.cancelled = true;
    }

    return result;
}

std::vector<FileEntry> listFiles(const std::filesystem::path &directory, bool recursive,
                                const BuildDirectoryTreeOptions &options)
{
    std::vector<FileEntry> files;

    std::error_code ec;
    fs::path basePath = fs::absolute(directory, ec);
    if (ec)
        basePath = directory;

    fs::path scanPath = basePath;
    if (options.followCommandLineSymlinks)
    {
        std::error_code symEc;
        if (fs::is_symlink(basePath, symEc))
        {
            fs::path resolved = fs::weakly_canonical(basePath, symEc);
            if (!symEc)
                scanPath = resolved;
        }
    }
    scanPath = fs::absolute(scanPath, ec);
    if (ec)
        scanPath = basePath;

    ScanContext context = makeScanContext(scanPath, options);
    context.rootPath = scanPath;

    auto considerFile = [&](const fs::path &path, const struct stat &sb) {
        if (shouldIgnorePath(path, context))
            return;
        if (options.ignoreNodumpFlag && hasNoDumpFlag(sb))
            return;
        if (options.stayOnFilesystem && context.rootDevice != 0 &&
            static_cast<std::uintmax_t>(sb.st_dev) != context.rootDevice)
            return;
        if (!options.countHardLinksMultipleTimes)
        {
            FileIdentity identity{static_cast<std::uintmax_t>(sb.st_dev), static_cast<std::uintmax_t>(sb.st_ino)};
            auto [it, inserted] = context.visited.insert(identity);
            if (!inserted)
                return;
        }
        std::uintmax_t size = fileSizeFromStat(sb);
        if (!passesThreshold(size, options))
            return;
        files.push_back(makeFileEntry(path, scanPath));
    };

    auto shouldSkipDirectory = [&](const fs::path &path, const struct stat &sb, bool isSymlink) {
        if (shouldIgnorePath(path, context))
            return true;
        if (options.ignoreNodumpFlag && hasNoDumpFlag(sb))
            return true;
        if (options.stayOnFilesystem && context.rootDevice != 0 &&
            static_cast<std::uintmax_t>(sb.st_dev) != context.rootDevice)
            return true;
        if (isSymlink && options.symlinkPolicy != BuildDirectoryTreeOptions::SymlinkPolicy::Always)
            return true;
        return false;
    };

    if (recursive)
    {
        fs::directory_options dirOptions = fs::directory_options::skip_permission_denied;
        if (options.symlinkPolicy == BuildDirectoryTreeOptions::SymlinkPolicy::Always)
            dirOptions |= fs::directory_options::follow_directory_symlink;
        fs::recursive_directory_iterator it(scanPath, dirOptions, ec);
        fs::recursive_directory_iterator end;
        if (ec)
            return files;

        for (; it != end; it.increment(ec))
        {
            if (ec)
            {
                reportError(context, it->path(), ec);
                ec.clear();
                continue;
            }

            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();

            struct stat sb;
            if (stat(path.c_str(), &sb) != 0)
            {
                reportError(context, path, std::error_code(errno, std::generic_category()));
                continue;
            }

            std::error_code entryEc;
            bool isSymlink = entry.is_symlink(entryEc) && !entryEc;
            bool isDirectory = S_ISDIR(sb.st_mode);

            if (isDirectory)
            {
                if (shouldSkipDirectory(path, sb, isSymlink))
                    it.disable_recursion_pending();
                continue;
            }

            if (!entry.is_regular_file(entryEc) || entryEc)
                continue;

            considerFile(path, sb);
        }
    }
    else
    {
        fs::directory_options dirOptions = fs::directory_options::skip_permission_denied;
        if (options.symlinkPolicy == BuildDirectoryTreeOptions::SymlinkPolicy::Always)
            dirOptions |= fs::directory_options::follow_directory_symlink;
        fs::directory_iterator it(scanPath, dirOptions, ec);
        fs::directory_iterator end;
        if (ec)
            return files;

        for (; it != end; it.increment(ec))
        {
            if (ec)
            {
                reportError(context, scanPath, ec);
                ec.clear();
                continue;
            }

            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc)
                continue;

            struct stat sb;
            if (stat(path.c_str(), &sb) != 0)
            {
                reportError(context, path, std::error_code(errno, std::generic_category()));
                continue;
            }

            considerFile(path, sb);
        }
    }

    return files;
}

std::vector<FileTypeSummary> summarizeFileTypes(const std::filesystem::path &directory, bool recursive,
                                                const BuildDirectoryTreeOptions &options)
{
    std::map<std::string, FileTypeSummary> summaries;

    std::error_code ec;
    fs::path basePath = fs::absolute(directory, ec);
    if (ec)
        basePath = directory;

    fs::path scanPath = basePath;
    if (options.followCommandLineSymlinks)
    {
        std::error_code symEc;
        if (fs::is_symlink(basePath, symEc))
        {
            fs::path resolved = fs::weakly_canonical(basePath, symEc);
            if (!symEc)
                scanPath = resolved;
        }
    }
    scanPath = fs::absolute(scanPath, ec);
    if (ec)
        scanPath = basePath;

    ScanContext context = makeScanContext(scanPath, options);
    context.rootPath = scanPath;

    auto considerFile = [&](const fs::path &path, const struct stat &sb) {
        if (shouldIgnorePath(path, context))
            return;
        if (options.ignoreNodumpFlag && hasNoDumpFlag(sb))
            return;
        if (options.stayOnFilesystem && context.rootDevice != 0 &&
            static_cast<std::uintmax_t>(sb.st_dev) != context.rootDevice)
            return;
        if (!options.countHardLinksMultipleTimes)
        {
            FileIdentity identity{static_cast<std::uintmax_t>(sb.st_dev), static_cast<std::uintmax_t>(sb.st_ino)};
            auto [it, inserted] = context.visited.insert(identity);
            if (!inserted)
                return;
        }
        std::uintmax_t size = fileSizeFromStat(sb);
        if (!passesThreshold(size, options))
            return;
        std::string type = detectFileType(path);
        FileTypeSummary &summary = summaries[type];
        if (summary.type.empty())
            summary.type = type;
        summary.totalSize += size;
        ++summary.count;
    };

    auto shouldSkipDirectory = [&](const fs::path &path, const struct stat &sb, bool isSymlink) {
        if (shouldIgnorePath(path, context))
            return true;
        if (options.ignoreNodumpFlag && hasNoDumpFlag(sb))
            return true;
        if (options.stayOnFilesystem && context.rootDevice != 0 &&
            static_cast<std::uintmax_t>(sb.st_dev) != context.rootDevice)
            return true;
        if (isSymlink && options.symlinkPolicy != BuildDirectoryTreeOptions::SymlinkPolicy::Always)
            return true;
        return false;
    };

    if (recursive)
    {
        fs::directory_options dirOptions = fs::directory_options::skip_permission_denied;
        if (options.symlinkPolicy == BuildDirectoryTreeOptions::SymlinkPolicy::Always)
            dirOptions |= fs::directory_options::follow_directory_symlink;
        fs::recursive_directory_iterator it(scanPath, dirOptions, ec);
        fs::recursive_directory_iterator end;
        if (ec)
            return {};

        for (; it != end; it.increment(ec))
        {
            if (ec)
            {
                reportError(context, it->path(), ec);
                ec.clear();
                continue;
            }

            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();

            struct stat sb;
            if (stat(path.c_str(), &sb) != 0)
            {
                reportError(context, path, std::error_code(errno, std::generic_category()));
                continue;
            }

            std::error_code entryEc;
            bool isSymlink = entry.is_symlink(entryEc) && !entryEc;
            bool isDirectory = S_ISDIR(sb.st_mode);

            if (isDirectory)
            {
                if (shouldSkipDirectory(path, sb, isSymlink))
                    it.disable_recursion_pending();
                continue;
            }

            if (!entry.is_regular_file(entryEc) || entryEc)
                continue;

            considerFile(path, sb);
        }
    }
    else
    {
        fs::directory_options dirOptions = fs::directory_options::skip_permission_denied;
        if (options.symlinkPolicy == BuildDirectoryTreeOptions::SymlinkPolicy::Always)
            dirOptions |= fs::directory_options::follow_directory_symlink;
        fs::directory_iterator it(scanPath, dirOptions, ec);
        fs::directory_iterator end;
        if (ec)
            return {};

        for (; it != end; it.increment(ec))
        {
            if (ec)
            {
                reportError(context, scanPath, ec);
                ec.clear();
                continue;
            }

            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc)
                continue;

            struct stat sb;
            if (stat(path.c_str(), &sb) != 0)
            {
                reportError(context, path, std::error_code(errno, std::generic_category()));
                continue;
            }

            considerFile(path, sb);
        }
    }

    std::vector<FileTypeSummary> result;
    result.reserve(summaries.size());
    for (auto &[type, summary] : summaries)
        result.push_back(summary);
    return result;
}

std::vector<FileEntry> listFilesByType(const std::filesystem::path &directory, bool recursive,
                                       const std::string &type,
                                       const BuildDirectoryTreeOptions &options)
{
    std::vector<FileEntry> files = listFiles(directory, recursive, options);
    files.erase(std::remove_if(files.begin(), files.end(), [&](const FileEntry &entry) {
                     return detectFileType(entry.path) != type;
                 }),
                 files.end());
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

