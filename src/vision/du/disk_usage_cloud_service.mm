#import <Foundation/Foundation.h>

#include "disk_usage_services.hpp"

#include <utility>

namespace ck::vision
{
namespace
{

NSURL *file_url(const std::filesystem::path &path)
{
    const std::string native_path = path.string();
    NSString *string_path = [NSString stringWithUTF8String:native_path.c_str()];
    return string_path == nil ? nil : [NSURL fileURLWithPath:string_path];
}

std::string error_message(NSError *error, std::string fallback)
{
    if (error == nil)
        return fallback;
    const char *description = [[error localizedDescription] UTF8String];
    return description == nullptr ? fallback : std::string(description);
}

DiskUsageCloudCapability cloud_capability(const std::filesystem::path &target)
{
    @autoreleasepool
    {
        NSURL *url = file_url(target);
        if (url == nil)
            return {.available = false, .reason = "The selected path cannot be represented by the macOS cloud provider."};

        NSError *error = nil;
        id ubiquitous = nil;
        if (![url getResourceValue:&ubiquitous forKey:NSURLIsUbiquitousItemKey error:&error])
        {
            return {.available = false,
                    .reason = "Could not inspect cloud availability: " + error_message(error, "unknown macOS error")};
        }
        if (ubiquitous == nil || ![ubiquitous boolValue])
            return {.available = false, .reason = "The selected directory is not managed by iCloud Drive."};
        return {.available = true, .reason = {}};
    }
}

DiskUsageCloudOperationResult perform_cloud_action(DiskUsageCloudAction action,
                                                    const std::filesystem::path &target,
                                                    const std::atomic_bool &cancellation,
                                                    const DiskUsageCloudService::ProgressHandler &on_progress)
{
    if (cancellation.load(std::memory_order_acquire))
        return {.success = false, .cancelled = true, .processed_items = 0, .message = "Cloud operation cancelled."};

    const DiskUsageCloudCapability capability = cloud_capability(target);
    if (!capability.available)
        return {.success = false, .cancelled = false, .processed_items = 0, .message = capability.reason};

    if (on_progress)
    {
        on_progress({.message = action == DiskUsageCloudAction::Download ? "Requesting iCloud download…"
                                                                           : "Requesting removal of local iCloud copies…",
                     .completed_items = 0,
                     .total_items = 1});
    }

    @autoreleasepool
    {
        NSURL *url = file_url(target);
        NSError *error = nil;
        NSFileManager *manager = [[NSFileManager alloc] init];
        const bool accepted = action == DiskUsageCloudAction::Download
                                  ? [manager startDownloadingUbiquitousItemAtURL:url error:&error]
                                  : [manager evictUbiquitousItemAtURL:url error:&error];
        [manager release];

        if (cancellation.load(std::memory_order_acquire))
            return {.success = false, .cancelled = true, .processed_items = 0, .message = "Cloud operation cancelled."};
        if (!accepted)
        {
            return {.success = false,
                    .cancelled = false,
                    .processed_items = 0,
                    .message = "macOS did not accept the cloud request: " + error_message(error, "unknown macOS error")};
        }

        const std::string message = action == DiskUsageCloudAction::Download
                                        ? "macOS accepted the download request. The provider may continue synchronizing in the background."
                                        : "macOS accepted the request to free local cloud copies. Rescan to refresh usage data.";
        if (on_progress)
            on_progress({.message = message, .completed_items = 1, .total_items = 1});
        return {.success = true, .cancelled = false, .processed_items = 1, .message = message};
    }
}

} // namespace

MacDiskUsageCloudService::~MacDiskUsageCloudService()
{
    cancel();
    std::scoped_lock lock(mutex_);
    if (worker_.joinable())
        worker_.join();
}

DiskUsageCloudCapability MacDiskUsageCloudService::capability(DiskUsageCloudAction,
                                                               const std::filesystem::path &target) const
{
    return cloud_capability(target);
}

void MacDiskUsageCloudService::start(DiskUsageCloudAction action,
                                     std::filesystem::path target,
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
    running_.store(true, std::memory_order_release);
    std::jthread worker([this,
                         action,
                         target = std::move(target),
                         cancellation,
                         on_progress = std::move(on_progress),
                         on_complete = std::move(on_complete)]() mutable {
        DiskUsageCloudOperationResult result = perform_cloud_action(action, target, *cancellation, on_progress);
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

void MacDiskUsageCloudService::cancel() noexcept
{
    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::scoped_lock lock(mutex_);
        cancellation = cancellation_;
    }
    if (cancellation)
        cancellation->store(true, std::memory_order_release);
}

bool MacDiskUsageCloudService::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

} // namespace ck::vision
