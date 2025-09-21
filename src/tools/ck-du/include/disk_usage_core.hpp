#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <system_error>
#include <string>
#include <vector>

namespace ck::du
{

enum class SizeUnit
{
    Auto,
    Bytes,
    Kilobytes,
    Megabytes,
    Gigabytes,
    Terabytes,
    Blocks
};

enum class SortKey
{
    Unsorted,
    NameAscending,
    NameDescending,
    SizeDescending,
    SizeAscending,
    ModifiedDescending,
    ModifiedAscending
};

struct DirectoryStats
{
    std::uintmax_t totalSize = 0;
    std::size_t fileCount = 0;
    std::size_t directoryCount = 0;
};

struct DirectoryNode
{
    std::filesystem::path path;
    DirectoryStats stats;
    DirectoryNode *parent = nullptr;
    std::vector<std::unique_ptr<DirectoryNode>> children;
    std::chrono::system_clock::time_point modifiedTime{};
    bool expanded = false;
};

struct FileEntry
{
    std::filesystem::path path;
    std::string displayPath;
    std::uintmax_t size = 0;
    std::string owner;
    std::string group;
    std::string created;
    std::string modified;
    std::chrono::system_clock::time_point createdTime{};
    std::chrono::system_clock::time_point modifiedTime{};
};

struct BuildDirectoryTreeOptions
{
    std::function<void(const std::filesystem::path &)> progressCallback;
    std::function<bool()> cancelRequested;
    enum class SymlinkPolicy
    {
        Never,
        CommandLineOnly,
        Always
    } symlinkPolicy = SymlinkPolicy::Never;
    bool followCommandLineSymlinks = false;
    bool countHardLinksMultipleTimes = false;
    bool ignoreNodumpFlag = false;
    bool reportErrors = true;
    std::int64_t threshold = 0;
    bool stayOnFilesystem = false;
    std::vector<std::string> ignoreMasks;
    std::function<void(const std::filesystem::path &, const std::error_code &)> errorCallback;
};

struct BuildDirectoryTreeResult
{
    std::unique_ptr<DirectoryNode> root;
    bool cancelled = false;
};

BuildDirectoryTreeResult buildDirectoryTree(const std::filesystem::path &rootPath,
                                            const BuildDirectoryTreeOptions &options = {});
std::vector<FileEntry> listFiles(const std::filesystem::path &directory, bool recursive,
                                const BuildDirectoryTreeOptions &options = {});

SizeUnit getCurrentUnit() noexcept;
void setCurrentUnit(SizeUnit unit) noexcept;
const char *unitName(SizeUnit unit) noexcept;
std::string formatSize(std::uintmax_t bytes, SizeUnit unit = SizeUnit::Auto);

SortKey getCurrentSortKey() noexcept;
void setCurrentSortKey(SortKey key) noexcept;
const char *sortKeyName(SortKey key) noexcept;

} // namespace ck::du

