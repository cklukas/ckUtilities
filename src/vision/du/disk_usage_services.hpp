#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "disk_usage_core.hpp"

namespace ck::vision
{

// Application-owned scanning boundary.  The presentation passes value
// options and receives immutable completed snapshots; it never walks the
// filesystem or owns a worker thread.
class DiskUsageScanService
{
public:
    using ProgressHandler = std::function<void(const std::filesystem::path &)>;
    using CompletionHandler = std::function<void(ck::du::BuildDirectoryTreeResult)>;

    virtual ~DiskUsageScanService() = default;

    virtual void start(std::filesystem::path root,
                       ck::du::BuildDirectoryTreeOptions options,
                       ProgressHandler on_progress,
                       CompletionHandler on_complete) = 0;
    virtual void cancel() noexcept = 0;
    virtual bool running() const noexcept = 0;
};

// POSIX composition-root adapter for the existing framework-independent
// scanner.  It is deliberately joinable: cancellation and destruction never
// leave a worker able to outlive its Application.
class ThreadedDiskUsageScanService final : public DiskUsageScanService
{
public:
    ~ThreadedDiskUsageScanService() override;

    void start(std::filesystem::path root,
               ck::du::BuildDirectoryTreeOptions options,
               ProgressHandler on_progress,
               CompletionHandler on_complete) override;
    void cancel() noexcept override;
    bool running() const noexcept override;

private:
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::atomic_bool running_{false};
};

struct DiskUsageFileListResult
{
    std::vector<ck::du::FileEntry> files;
    bool cancelled = false;
};

// Cloud storage is an application capability, not a widget concern.  These
// operations deliberately act on one user-selected path: recursive traversal
// and each platform's storage semantics stay in the platform adapter rather
// than becoming implicit UI behavior.
enum class DiskUsageCloudAction
{
    Download,
    EvictLocalCopies,
};

struct DiskUsageCloudCapability
{
    bool available = false;
    std::string reason;
};

struct DiskUsageCloudProgress
{
    std::string message;
    std::size_t completed_items = 0;
    std::size_t total_items = 0;
};

struct DiskUsageCloudOperationResult
{
    // `success` means the provider accepted this one request. It never means
    // that a background sync has completed. `requestAccepted` retains that
    // fact if cancellation was requested after the provider call returned.
    bool success = false;
    bool cancelled = false;
    bool requestAccepted = false;
    std::size_t processed_items = 0;
    std::string provider;
    std::string message;
};

// This is deliberately shared by the presentation and provider boundaries so
// a stale tree node cannot turn into a cloud action. It accepts one existing,
// real directory that is contained by the current scan root without any
// symbolic-link component. Provider-specific eligibility is checked by the
// corresponding service afterwards.
DiskUsageCloudCapability validateDiskUsageCloudTarget(const std::filesystem::path &scan_root,
                                                       const std::filesystem::path &target);

class DiskUsageCloudService
{
public:
    using ProgressHandler = std::function<void(DiskUsageCloudProgress)>;
    using CompletionHandler = std::function<void(DiskUsageCloudOperationResult)>;

    virtual ~DiskUsageCloudService() = default;

    virtual DiskUsageCloudCapability capability(DiskUsageCloudAction action,
                                                 const std::filesystem::path &target,
                                                 const std::filesystem::path &scan_root) const = 0;
    virtual void start(DiskUsageCloudAction action,
                       std::filesystem::path target,
                       std::filesystem::path scan_root,
                       ProgressHandler on_progress,
                       CompletionHandler on_complete) = 0;
    virtual void cancel() noexcept = 0;
    virtual bool running() const noexcept = 0;
};

// The fallback preserves the complete native workflow on platforms without a
// supported cloud provider while making the unsupported state explicit.  It
// never silently claims that a local filesystem operation changed cloud data.
class UnsupportedDiskUsageCloudService final : public DiskUsageCloudService
{
public:
    DiskUsageCloudCapability capability(DiskUsageCloudAction action,
                                         const std::filesystem::path &target,
                                         const std::filesystem::path &scan_root) const override;
    void start(DiskUsageCloudAction action,
               std::filesystem::path target,
               std::filesystem::path scan_root,
               ProgressHandler on_progress,
               CompletionHandler on_complete) override;
    void cancel() noexcept override {}
    bool running() const noexcept override { return false; }
};

#if defined(__APPLE__)
// A Foundation-backed composition-root adapter.  It invokes macOS iCloud
// download/evict requests on a worker and reports immutable progress/results;
// no Foundation API or worker lifetime reaches the presentation layer.
class MacDiskUsageCloudService final : public DiskUsageCloudService
{
public:
    ~MacDiskUsageCloudService() override;

    DiskUsageCloudCapability capability(DiskUsageCloudAction action,
                                         const std::filesystem::path &target,
                                         const std::filesystem::path &scan_root) const override;
    void start(DiskUsageCloudAction action,
               std::filesystem::path target,
               std::filesystem::path scan_root,
               ProgressHandler on_progress,
               CompletionHandler on_complete) override;
    void cancel() noexcept override;
    bool running() const noexcept override;

private:
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::atomic_bool running_{false};
};
#endif

// Separate capability because file enumeration is a different user operation
// from aggregation.  Keeping the two operations explicit prevents a table
// request from silently sharing or taking over the active tree scan.
class DiskUsageFileListService
{
public:
    using CompletionHandler = std::function<void(DiskUsageFileListResult)>;

    virtual ~DiskUsageFileListService() = default;

    virtual void start(std::filesystem::path directory,
                       bool recursive,
                       ck::du::BuildDirectoryTreeOptions options,
                       CompletionHandler on_complete) = 0;
    virtual void cancel() noexcept = 0;
    virtual bool running() const noexcept = 0;
};

class ThreadedDiskUsageFileListService final : public DiskUsageFileListService
{
public:
    ~ThreadedDiskUsageFileListService() override;

    void start(std::filesystem::path directory,
               bool recursive,
               ck::du::BuildDirectoryTreeOptions options,
               CompletionHandler on_complete) override;
    void cancel() noexcept override;
    bool running() const noexcept override;

private:
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::atomic_bool running_{false};
};

} // namespace ck::vision
