#include "disk_usage_services.hpp"

#include <utility>

namespace ck::vision
{

DiskUsageCloudCapability UnsupportedDiskUsageCloudService::capability(DiskUsageCloudAction,
                                                                       const std::filesystem::path &) const
{
    return {.available = false, .reason = "Cloud operations are not supported on this platform."};
}

void UnsupportedDiskUsageCloudService::start(DiskUsageCloudAction,
                                             std::filesystem::path,
                                             ProgressHandler,
                                             CompletionHandler on_complete)
{
    if (on_complete)
    {
        on_complete({.success = false,
                     .cancelled = false,
                     .processed_items = 0,
                     .message = "Cloud operations are not supported on this platform."});
    }
}

ThreadedDiskUsageScanService::~ThreadedDiskUsageScanService()
{
    cancel();
    std::scoped_lock lock(mutex_);
    if (worker_.joinable())
        worker_.join();
}

void ThreadedDiskUsageScanService::start(std::filesystem::path root,
                                         ck::du::BuildDirectoryTreeOptions options,
                                         ProgressHandler on_progress,
                                         CompletionHandler on_complete)
{
    cancel();

    std::jthread previous;
    {
        std::scoped_lock lock(mutex_);
        previous = std::move(worker_);
    }
    if (previous.joinable())
        previous.join();

    auto cancellation = std::make_shared<std::atomic_bool>(false);
    const auto original_cancellation = std::move(options.cancelRequested);
    options.cancelRequested = [cancellation, original_cancellation] {
        return cancellation->load(std::memory_order_acquire) ||
               (original_cancellation && original_cancellation());
    };
    const auto original_progress = std::move(options.progressCallback);
    options.progressCallback = [original_progress, on_progress = std::move(on_progress)](const std::filesystem::path &path) {
        if (original_progress)
            original_progress(path);
        if (on_progress)
            on_progress(path);
    };

    running_.store(true, std::memory_order_release);
    std::jthread worker([this, root = std::move(root), options = std::move(options), on_complete = std::move(on_complete)]() mutable {
        auto result = ck::du::buildDirectoryTree(root, options);
        running_.store(false, std::memory_order_release);
        if (on_complete)
            on_complete(std::move(result));
    });

    {
        std::scoped_lock lock(mutex_);
        cancellation_ = std::move(cancellation);
        worker_ = std::move(worker);
    }
}

void ThreadedDiskUsageScanService::cancel() noexcept
{
    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::scoped_lock lock(mutex_);
        cancellation = cancellation_;
    }
    if (cancellation)
        cancellation->store(true, std::memory_order_release);
}

bool ThreadedDiskUsageScanService::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

ThreadedDiskUsageFileListService::~ThreadedDiskUsageFileListService()
{
    cancel();
    std::scoped_lock lock(mutex_);
    if (worker_.joinable())
        worker_.join();
}

void ThreadedDiskUsageFileListService::start(std::filesystem::path directory,
                                             bool recursive,
                                             ck::du::BuildDirectoryTreeOptions options,
                                             CompletionHandler on_complete)
{
    cancel();

    std::jthread previous;
    {
        std::scoped_lock lock(mutex_);
        previous = std::move(worker_);
    }
    if (previous.joinable())
        previous.join();

    auto cancellation = std::make_shared<std::atomic_bool>(false);
    const auto original_cancellation = std::move(options.cancelRequested);
    options.cancelRequested = [cancellation, original_cancellation] {
        return cancellation->load(std::memory_order_acquire) ||
               (original_cancellation && original_cancellation());
    };
    running_.store(true, std::memory_order_release);
    std::jthread worker([this, cancellation, directory = std::move(directory), recursive, options = std::move(options),
                         on_complete = std::move(on_complete)]() mutable {
        DiskUsageFileListResult result;
        result.files = ck::du::listFiles(directory, recursive, options);
        result.cancelled = cancellation->load(std::memory_order_acquire);
        running_.store(false, std::memory_order_release);
        if (on_complete)
            on_complete(std::move(result));
    });

    {
        std::scoped_lock lock(mutex_);
        cancellation_ = std::move(cancellation);
        worker_ = std::move(worker);
    }
}

void ThreadedDiskUsageFileListService::cancel() noexcept
{
    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::scoped_lock lock(mutex_);
        cancellation = cancellation_;
    }
    if (cancellation)
        cancellation->store(true, std::memory_order_release);
}

bool ThreadedDiskUsageFileListService::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

} // namespace ck::vision
