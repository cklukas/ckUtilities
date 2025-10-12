#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ck::du::cloud
{

enum class ActionKind
{
    DownloadAll,
    EvictLocalCopies,
    KeepAlways,
    OptimizeStorage,
    PauseSync,
    ResumeSync,
    RevealInFinder
};

struct UsageSnapshot
{
    std::size_t totalFiles = 0;
    std::size_t localFiles = 0;
    std::size_t cloudOnlyFiles = 0;
    std::uintmax_t localBytes = 0;
    std::uintmax_t cloudBytes = 0;
    std::uintmax_t logicalBytes = 0;
};

struct OperationDefinition
{
    ActionKind kind;
    std::string label;
    std::string explanation;
    std::string impact;
    bool enabled = true;
};

struct DialogSelection
{
    bool confirmed = false;
    ActionKind action = ActionKind::DownloadAll;
    OperationDefinition definition;
};

struct OperationProgress
{
    std::size_t totalItems = 0;
    std::size_t processedItems = 0;
    std::uintmax_t totalBytes = 0;
    std::uintmax_t processedBytes = 0;
    std::string currentItem;
};

struct OperationCallbacks
{
    std::function<void(const std::string &)> onStatus;
    std::function<bool(const std::filesystem::path &, std::uintmax_t)> onItem;
    std::function<bool()> isCancelled;
};

struct OperationResult
{
    bool success = true;
    bool cancelled = false;
    std::string errorMessage;
    std::size_t processedItems = 0;
    std::uintmax_t processedBytes = 0;
};

#if defined(__APPLE__)
OperationResult performCloudOperation(ActionKind action,
                                      const std::filesystem::path &root,
                                      const OperationCallbacks &callbacks,
                                      bool recursive);

OperationResult revealInFinder(const std::filesystem::path &path);

// Returns true if NSURLUbiquitousItemIsSyncPausedKey appears readable/settable
// for the given URL. Use this to decide whether to show Pause/Resume actions.
bool supportsPauseResume(const std::filesystem::path &path);
#else
inline OperationResult performCloudOperation(ActionKind,
                                             const std::filesystem::path &,
                                             const OperationCallbacks &,
                                             bool)
{
    OperationResult result;
    result.success = false;
    result.errorMessage = "Cloud operations are only supported on macOS.";
    return result;
}

inline OperationResult revealInFinder(const std::filesystem::path &)
{
    OperationResult result;
    result.success = false;
    result.errorMessage = "Reveal in Finder is only supported on macOS.";
    return result;
}

inline bool supportsPauseResume(const std::filesystem::path &)
{
    return false;
}
#endif

} // namespace ck::du::cloud
