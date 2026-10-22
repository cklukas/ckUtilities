#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "disk_usage_core.hpp"
#include "disk_usage_platform.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <unordered_map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <system_error>


#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
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

std::string lowercase(const std::string &value)
{
    std::string lowered;
    lowered.reserve(value.size());
    for (char ch : value)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    return lowered;
}

std::string extensionWithoutDot(const fs::path &path)
{
    std::string ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    return lowercase(ext);
}

const std::unordered_map<std::string, const char *> &extensionMimeMap()
{
    static const std::unordered_map<std::string, const char *> kMap = {
        {"txt", "text/plain"},           {"text", "text/plain"},           {"md", "text/markdown"},
        {"markdown", "text/markdown"},   {"log", "text/plain"},            {"csv", "text/csv"},
        {"tsv", "text/tab-separated-values"},                              {"json", "application/json"},
        {"xml", "application/xml"},      {"html", "text/html"},            {"htm", "text/html"},
        {"css", "text/css"},             {"js", "application/javascript"}, {"ts", "application/typescript"},
        {"c", "text/x-c"},               {"cc", "text/x-c++"},             {"cpp", "text/x-c++"},
        {"cxx", "text/x-c++"},           {"h", "text/x-c"},                {"hpp", "text/x-c++"},
        {"hh", "text/x-c++"},            {"mm", "text/x-objective-c"},
        {"m", "text/x-objective-c"},     {"swift", "text/x-swift"},        {"java", "text/x-java"},
        {"kt", "text/x-kotlin"},         {"py", "text/x-python"},          {"rb", "text/x-ruby"},
        {"go", "text/x-go"},             {"rs", "text/x-rust"},            {"php", "text/x-php"},
        {"sh", "text/x-shellscript"},    {"bash", "text/x-shellscript"},   {"zsh", "text/x-shellscript"},
        {"pl", "text/x-perl"},           {"lua", "text/x-lua"},            {"sql", "application/sql"},
        {"yml", "text/yaml"},            {"yaml", "text/yaml"},            {"toml", "text/x-toml"},
        {"ini", "text/plain"},           {"conf", "text/plain"},           {"cfg", "text/plain"},
        {"png", "image/png"},            {"jpg", "image/jpeg"},            {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},            {"bmp", "image/bmp"},             {"svg", "image/svg+xml"},
        {"webp", "image/webp"},          {"tiff", "image/tiff"},           {"tif", "image/tiff"},
        {"heic", "image/heic"},          {"mp3", "audio/mpeg"},            {"aac", "audio/aac"},
        {"wav", "audio/wav"},            {"flac", "audio/flac"},           {"aiff", "audio/aiff"},
        {"ogg", "audio/ogg"},            {"m4a", "audio/mp4"},             {"mp4", "video/mp4"},
        {"m4v", "video/x-m4v"},          {"mov", "video/quicktime"},       {"mkv", "video/x-matroska"},
        {"avi", "video/x-msvideo"},      {"wmv", "video/x-ms-wmv"},        {"webm", "video/webm"},
        {"mpg", "video/mpeg"},           {"mpeg", "video/mpeg"},           {"pdf", "application/pdf"},
        {"rtf", "application/rtf"},      {"doc", "application/msword"},    {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xls", "application/vnd.ms-excel"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"ppt", "application/vnd.ms-powerpoint"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"zip", "application/zip"},      {"gz", "application/gzip"},       {"tgz", "application/gzip"},
        {"tar", "application/x-tar"},    {"bz2", "application/x-bzip2"},   {"xz", "application/x-xz"},
        {"7z", "application/x-7z-compressed"},                             {"sit", "application/x-stuffit"},
        {"rar", "application/vnd.rar"},  {"dmg", "application/x-apple-diskimage"},
        {"iso", "application/x-iso9660-image"},                            {"apk", "application/vnd.android.package-archive"},
        {"ipa", "application/octet-stream"},                                {"app", "application/octet-stream"},
        {"exe", "application/vnd.microsoft.portable-executable"},
        {"dll", "application/x-msdownload"},                                {"so", "application/x-sharedlib"},
        {"dylib", "application/x-mach-o-binary"},                          {"o", "application/x-object"},
        {"obj", "application/x-tgif"},   {"psd", "image/vnd.adobe.photoshop"},
        {"ai", "application/postscript"}
    };
    return kMap;
}

struct SizeBreakdown
{
    std::uintmax_t logicalSize = 0;
    std::uintmax_t onDiskSize = 0;
    std::uintmax_t cloudOnlySize = 0;
    bool isICloudItem = false;
    bool isICloudDownloaded = true;
    bool isICloudDownloading = false;
    bool isOfflinePlaceholder = false;
    std::string iCloudStatus;
};

#if defined(__APPLE__)
std::string cfStringToStd(CFStringRef value)
{
    if (!value)
        return {};
    CFIndex length = CFStringGetLength(value);
    CFIndex maxSize = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
    if (maxSize < 0)
        return {};
    std::string buffer(static_cast<std::size_t>(maxSize) + 1, '\0');
    if (CFStringGetCString(value, buffer.data(), static_cast<CFIndex>(buffer.size()), kCFStringEncodingUTF8))
    {
        buffer.resize(std::strlen(buffer.c_str()));
        return buffer;
    }
    return {};
}

bool copyBoolResource(CFURLRef url, CFStringRef key, bool &out)
{
    CFTypeRef value = nullptr;
    Boolean ok = CFURLCopyResourcePropertyForKey(url, key, &value, nullptr);
    if (!ok)
        return false;
    if (value)
    {
        if (CFGetTypeID(value) == CFBooleanGetTypeID())
            out = CFBooleanGetValue(static_cast<CFBooleanRef>(value));
        CFRelease(value);
    }
    return ok;
}

bool updateDownloadStatus(CFURLRef url, SizeBreakdown &breakdown)
{
    CFTypeRef value = nullptr;
    Boolean ok = CFURLCopyResourcePropertyForKey(url, kCFURLUbiquitousItemDownloadingStatusKey, &value, nullptr);
    if (!ok || !value)
        return false;

    if (CFGetTypeID(value) == CFStringGetTypeID())
    {
        CFStringRef status = static_cast<CFStringRef>(value);
        breakdown.iCloudStatus = cfStringToStd(status);

        if (CFEqual(status, kCFURLUbiquitousItemDownloadingStatusDownloaded) ||
            CFEqual(status, kCFURLUbiquitousItemDownloadingStatusCurrent))
        {
            breakdown.isICloudDownloaded = true;
        }
        else if (CFEqual(status, kCFURLUbiquitousItemDownloadingStatusNotDownloaded))
        {
            breakdown.isICloudDownloaded = false;
        }
    }

    CFRelease(value);
    return true;
}

std::string mimeFromExtensionUTType(const std::string &ext)
{
    if (ext.empty())
        return {};
    CFStringRef tag = CFStringCreateWithCString(kCFAllocatorDefault, ext.c_str(), kCFStringEncodingUTF8);
    if (!tag)
        return {};
    std::string result;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    CFStringRef uti = UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, tag, nullptr);
    if (uti)
    {
        CFStringRef mime = UTTypeCopyPreferredTagWithClass(uti, kUTTagClassMIMEType);
        if (mime)
        {
            result = cfStringToStd(mime);
            CFRelease(mime);
        }
        CFRelease(uti);
    }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    CFRelease(tag);
    return result;
}
#endif

SizeBreakdown computeSizeBreakdown(const fs::path &path, const platform::FileMetadata &sb)
{
    SizeBreakdown breakdown;
    breakdown.logicalSize = sb.logicalSize;
    breakdown.onDiskSize = sb.allocatedSize;

#if defined(__APPLE__)
    if (sb.offlinePlaceholder)
    {
        breakdown.isOfflinePlaceholder = true;
        breakdown.isICloudItem = true;
        breakdown.isICloudDownloaded = false;
    }
    const std::string native = path.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8 *>(native.c_str()),
        static_cast<CFIndex>(native.size()),
        sb.isDirectory);
    if (url)
    {
        bool boolValue = false;
        if (copyBoolResource(url, kCFURLIsUbiquitousItemKey, boolValue) && boolValue)
            breakdown.isICloudItem = true;
        if (updateDownloadStatus(url, breakdown))
            breakdown.isICloudItem = true;
        if (copyBoolResource(url, kCFURLUbiquitousItemIsDownloadingKey, boolValue))
            breakdown.isICloudDownloading = boolValue;
        CFRelease(url);
    }

    if (breakdown.isICloudItem)
    {
        if (!breakdown.isICloudDownloaded)
        {
            if (breakdown.logicalSize > breakdown.onDiskSize)
                breakdown.cloudOnlySize = breakdown.logicalSize - breakdown.onDiskSize;
            else
                breakdown.cloudOnlySize = breakdown.logicalSize;
        }
        else if (breakdown.logicalSize > breakdown.onDiskSize)
        {
            breakdown.cloudOnlySize = breakdown.logicalSize - breakdown.onDiskSize;
        }
    }
#else
    (void)path;
    (void)sb;
#endif

    if (breakdown.cloudOnlySize > breakdown.logicalSize)
        breakdown.cloudOnlySize = breakdown.logicalSize;
    return breakdown;
}

std::string detectFileType(const fs::path &path)
{
    std::string ext = extensionWithoutDot(path);
#if defined(__APPLE__)
    if (!ext.empty())
    {
        std::string mime = mimeFromExtensionUTType(ext);
        if (!mime.empty())
            return mime;
    }
#endif
    if (ext.empty())
        return "application/octet-stream";
    const auto &map = extensionMimeMap();
    if (auto it = map.find(ext); it != map.end())
        return it->second;
    return "application/x-" + ext;
}

bool shouldIgnorePath(const fs::path &path, const ScanContext &context)
{
    if (context.options.ignoreMasks.empty())
        return false;

    std::string filename = path.filename().string();
    fs::path relativePath = path.lexically_relative(context.rootPath);
    std::string relativeString;
    if (relativePath.empty() || relativePath == fs::path("."))
        relativeString = filename;
    else
        relativeString = relativePath.generic_string();

    for (const auto &pattern : context.options.ignoreMasks)
    {
        if (pattern.empty())
            continue;
        if (platform::glob_match(pattern, filename))
            return true;
        if (!relativeString.empty() && platform::glob_match(pattern, relativeString))
            return true;
    }
    return false;
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
    platform::FileMetadata sb;
    std::error_code statError;
    if (platform::read_metadata(root, sb, statError))
        context.rootDevice = sb.device;
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

    platform::FileMetadata sb;
    std::error_code statError;
    bool haveStat = platform::read_metadata(path, sb, statError);
    if (haveStat)
        node.modifiedTime = sb.modified;
    else
        node.modifiedTime = std::chrono::system_clock::time_point{};

    if (haveStat && context.options.ignoreNodumpFlag && sb.noDump)
        return stats;

    if (haveStat && !context.options.countHardLinksMultipleTimes)
    {
        FileIdentity identity{sb.device, sb.fileId};
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
        if (entryEc)
        {
            reportError(context, entryPath, entryEc);
            continue;
        }

        platform::FileMetadata entryStat;
        std::error_code statError;
        if (!platform::read_metadata(entryPath, entryStat, statError))
        {
            reportError(context, entryPath, statError);
            continue;
        }
        bool isSymlink = entryStat.isSymlink;

        if (context.options.ignoreNodumpFlag && entryStat.noDump)
            continue;

        if (context.options.stayOnFilesystem && context.rootDevice != 0 &&
            entryStat.device != context.rootDevice)
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
            stats.logicalSize += childStats.logicalSize;
            stats.cloudOnlySize += childStats.cloudOnlySize;
            stats.fileCount += childStats.fileCount;
            stats.directoryCount += childStats.directoryCount + 1;
            stats.cloudOnlyFileCount += childStats.cloudOnlyFileCount;

            if (passesThreshold(childStats.totalSize, context.options))
                node.children.push_back(std::move(childNode));
        }
        else
        {
            bool count = true;
            if (!context.options.countHardLinksMultipleTimes)
            {
                FileIdentity identity{entryStat.device,
                                       entryStat.fileId};
                auto [it2, inserted] = context.visited.insert(identity);
                count = inserted;
            }
            if (!count)
                continue;

            SizeBreakdown breakdown = computeSizeBreakdown(entryPath, entryStat);
            stats.totalSize += breakdown.onDiskSize;
            stats.logicalSize += breakdown.logicalSize;
            stats.cloudOnlySize += breakdown.cloudOnlySize;
            if (breakdown.cloudOnlySize > 0)
                ++stats.cloudOnlyFileCount;
            ++stats.fileCount;
        }
    }

    node.stats = stats;
    return stats;
}

FileEntry makeFileEntry(const fs::path &path, const fs::path &base, const platform::FileMetadata &sb,
                        const SizeBreakdown &breakdown)
{
    FileEntry entry;
    entry.path = path;
    entry.displayPath = path.lexically_relative(base).string();
    if (entry.displayPath.empty())
        entry.displayPath = path.filename().string();

    entry.size = breakdown.onDiskSize;
    entry.logicalSize = breakdown.logicalSize;
    entry.cloudOnlySize = breakdown.cloudOnlySize;
    entry.isICloudItem = breakdown.isICloudItem;
    entry.isICloudDownloaded = breakdown.isICloudDownloaded;
    entry.isICloudDownloading = breakdown.isICloudDownloading;
    entry.iCloudStatus = breakdown.iCloudStatus;

    const auto [owner, group] = platform::owner_and_group(path, sb);
    entry.owner = owner;
    entry.group = group;
    entry.modifiedTime = sb.modified;
    entry.modified = formatTimePoint(entry.modifiedTime);
    entry.createdTime = sb.created;
    entry.created = formatTimePoint(entry.createdTime);

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

    auto cancelRequested = [&]() -> bool {
        return options.cancelRequested && options.cancelRequested();
    };

    auto reportProgress = [&](const fs::path &path) {
        if (options.progressCallback)
            options.progressCallback(path);
    };

    auto considerFile = [&](const fs::path &path, const platform::FileMetadata &sb) {
        if (cancelRequested())
            return;
        if (shouldIgnorePath(path, context))
            return;
        if (options.ignoreNodumpFlag && sb.noDump)
            return;
        if (options.stayOnFilesystem && context.rootDevice != 0 &&
            sb.device != context.rootDevice)
            return;
        if (!options.countHardLinksMultipleTimes)
        {
            FileIdentity identity{sb.device, sb.fileId};
            auto [it, inserted] = context.visited.insert(identity);
            if (!inserted)
                return;
        }
        SizeBreakdown breakdown = computeSizeBreakdown(path, sb);
        if (!passesThreshold(breakdown.onDiskSize, options))
            return;
        files.push_back(makeFileEntry(path, scanPath, sb, breakdown));
    };

    auto shouldSkipDirectory = [&](const fs::path &path, const platform::FileMetadata &sb, bool isSymlink) {
        if (shouldIgnorePath(path, context))
            return true;
        if (options.ignoreNodumpFlag && sb.noDump)
            return true;
        if (options.stayOnFilesystem && context.rootDevice != 0 &&
            sb.device != context.rootDevice)
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

            if (cancelRequested())
                return files;

            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();

            reportProgress(path);

            platform::FileMetadata sb;
            std::error_code statError;
            if (!platform::read_metadata(path, sb, statError))
            {
                reportError(context, path, statError);
                continue;
            }

            std::error_code entryEc;
            bool isSymlink = entry.is_symlink(entryEc) && !entryEc;
            bool isDirectory = sb.isDirectory;

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

            if (cancelRequested())
                return files;

            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();
            reportProgress(path);
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc)
                continue;

            platform::FileMetadata sb;
            std::error_code statError;
            if (!platform::read_metadata(path, sb, statError))
            {
                reportError(context, path, statError);
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

    auto cancelRequested = [&]() -> bool {
        return options.cancelRequested && options.cancelRequested();
    };

    auto reportProgress = [&](const fs::path &path) {
        if (options.progressCallback)
            options.progressCallback(path);
    };

    auto considerFile = [&](const fs::path &path, const platform::FileMetadata &sb) {
        if (cancelRequested())
            return;
        if (shouldIgnorePath(path, context))
            return;
        if (options.ignoreNodumpFlag && sb.noDump)
            return;
        if (options.stayOnFilesystem && context.rootDevice != 0 &&
            sb.device != context.rootDevice)
            return;
        if (!options.countHardLinksMultipleTimes)
        {
            FileIdentity identity{sb.device, sb.fileId};
            auto [it, inserted] = context.visited.insert(identity);
            if (!inserted)
                return;
        }
        SizeBreakdown breakdown = computeSizeBreakdown(path, sb);
        if (!passesThreshold(breakdown.onDiskSize, options))
            return;
        std::string type = detectFileType(path);
        FileTypeSummary &summary = summaries[type];
        if (summary.type.empty())
            summary.type = type;
        summary.totalSize += breakdown.onDiskSize;
        summary.logicalSize += breakdown.logicalSize;
        summary.cloudOnlySize += breakdown.cloudOnlySize;
        if (breakdown.cloudOnlySize > 0)
            ++summary.cloudOnlyCount;
        ++summary.count;
    };

    auto shouldSkipDirectory = [&](const fs::path &path, const platform::FileMetadata &sb, bool isSymlink) {
        if (shouldIgnorePath(path, context))
            return true;
        if (options.ignoreNodumpFlag && sb.noDump)
            return true;
        if (options.stayOnFilesystem && context.rootDevice != 0 &&
            sb.device != context.rootDevice)
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

            if (cancelRequested())
                return {};

            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();

            reportProgress(path);

            platform::FileMetadata sb;
            std::error_code statError;
            if (!platform::read_metadata(path, sb, statError))
            {
                reportError(context, path, statError);
                continue;
            }

            std::error_code entryEc;
            bool isSymlink = entry.is_symlink(entryEc) && !entryEc;
            bool isDirectory = sb.isDirectory;

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

            if (cancelRequested())
                return {};

            const fs::directory_entry &entry = *it;
            const fs::path &path = entry.path();
            reportProgress(path);
            std::error_code entryEc;
            if (!entry.is_regular_file(entryEc) || entryEc)
                continue;

            platform::FileMetadata sb;
            std::error_code statError;
            if (!platform::read_metadata(path, sb, statError))
            {
                reportError(context, path, statError);
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
