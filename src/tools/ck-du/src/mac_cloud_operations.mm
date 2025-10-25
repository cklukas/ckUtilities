#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
#define CK_HAS_ICLOUD_SYNC_PAUSE 1
#else
#define CK_HAS_ICLOUD_SYNC_PAUSE 0
#endif

#include "cloud_actions.hpp"

namespace ck::du::cloud
{
namespace
{

std::string nsStringToStd(NSString *value)
{
    if (!value)
        return {};
    const char *utf8 = [value UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

std::filesystem::path urlToPath(NSURL *url)
{
    if (!url)
        return {};
    const char *fs = [url fileSystemRepresentation];
    if (fs)
        return std::filesystem::path(fs);
    NSString *path = [url path];
    return path ? std::filesystem::path(nsStringToStd(path)) : std::filesystem::path();
}

template <typename T>
std::optional<T> optionalNumber(NSDictionary<NSURLResourceKey, id> *values, NSURLResourceKey key)
{
    NSNumber *number = values[key];
    if (!number)
        return std::nullopt;
    if constexpr (std::is_same_v<T, bool>)
        return number.boolValue;
    else
        return static_cast<T>(number.unsignedLongLongValue);
}

bool isUbiquitousItem(NSDictionary<NSURLResourceKey, id> *values)
{
    auto flag = optionalNumber<bool>(values, NSURLIsUbiquitousItemKey);
    return flag.has_value() && *flag;
}

bool isDirectory(NSDictionary<NSURLResourceKey, id> *values)
{
    auto flag = optionalNumber<bool>(values, NSURLIsDirectoryKey);
    return flag.has_value() && *flag;
}

NSString *downloadingStatus(NSDictionary<NSURLResourceKey, id> *values)
{
    NSString *status = values[NSURLUbiquitousItemDownloadingStatusKey];
    return status;
}

std::uintmax_t fileSize(NSDictionary<NSURLResourceKey, id> *values)
{
    auto size = optionalNumber<std::uintmax_t>(values, NSURLFileSizeKey);
    return size.value_or(0);
}

bool handleError(OperationResult &result,
                 const OperationCallbacks &callbacks,
                 const std::filesystem::path &path,
                 NSError *error)
{
    result.success = false;
    result.errorMessage = nsStringToStd(error ? error.localizedDescription : @"Unknown error");
    if (!path.empty())
    {
        std::string message = "Error on " + path.string() + ": " + result.errorMessage;
        if (callbacks.onStatus)
            callbacks.onStatus(message);
    }
    return false;
}

NSArray<NSURLResourceKey> *resourceKeys()
{
    static NSArray<NSURLResourceKey> *keys = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
      keys = @[
        NSURLIsDirectoryKey,
        NSURLIsRegularFileKey,
        NSURLIsUbiquitousItemKey,
        NSURLUbiquitousItemDownloadingStatusKey,
        NSURLFileSizeKey,
        NSURLUbiquitousItemDownloadRequestedKey,
        NSURLUbiquitousItemIsExcludedFromSyncKey
#if CK_HAS_ICLOUD_SYNC_PAUSE
        ,
        NSURLUbiquitousItemIsSyncPausedKey
#endif
      ];
    });
    return keys;
}

bool applyPauseState(ActionKind action,
                     NSURL *url,
                     OperationResult &result,
                     const OperationCallbacks &callbacks)
{
#if CK_HAS_ICLOUD_SYNC_PAUSE
    if (@available(macOS 15.0, *))
    {
        bool pause = action == ActionKind::PauseSync;
        NSError *error = nil;
        if (![url setResourceValue:@(pause) forKey:NSURLUbiquitousItemIsSyncPausedKey error:&error])
        {
            handleError(result, callbacks, urlToPath(url), error);
            return false;
        }
        if (callbacks.onStatus)
        {
            callbacks.onStatus(pause ? "Paused iCloud syncing." : "Resumed iCloud syncing.");
        }
        result.processedItems = 1;
        return true;
    }
    (void)url;
    (void)callbacks;
    result.success = false;
    result.errorMessage = "This version of macOS does not support pausing iCloud Drive sync.";
    return false;
#else
    (void)url;
    (void)callbacks;
    result.success = false;
    result.errorMessage = "This version of macOS does not support pausing iCloud Drive sync.";
    return false;
#endif
}

bool performOperation(ActionKind action,
                      NSURL *url,
                      NSDictionary<NSURLResourceKey, id> *values,
                      NSFileManager *fileManager,
                      OperationResult &result,
                      const OperationCallbacks &callbacks)
{
    std::filesystem::path fsPath = urlToPath(url);
    if (!isUbiquitousItem(values))
        return true;

    bool directory = isDirectory(values);
    NSString *downloadStatus = downloadingStatus(values);
    bool isDownloaded = (downloadStatus == NSURLUbiquitousItemDownloadingStatusDownloaded ||
                         downloadStatus == NSURLUbiquitousItemDownloadingStatusCurrent);
    std::uintmax_t bytes = directory ? 0 : fileSize(values);

    NSError *error = nil;
    switch (action)
    {
    case ActionKind::DownloadAll:
        if (!directory && !isDownloaded)
        {
            if (![fileManager startDownloadingUbiquitousItemAtURL:url error:&error])
                return handleError(result, callbacks, fsPath, error);
            if (callbacks.onStatus)
                callbacks.onStatus("Requested download: " + fsPath.string());
        }
        break;
    case ActionKind::EvictLocalCopies:
        if (!directory && isDownloaded)
        {
            if (![fileManager evictUbiquitousItemAtURL:url error:&error])
                return handleError(result, callbacks, fsPath, error);
            if (callbacks.onStatus)
                callbacks.onStatus("Evicted local copy: " + fsPath.string());
        }
        break;
    case ActionKind::KeepAlways:
        if (![url setResourceValue:@YES forKey:NSURLUbiquitousItemDownloadRequestedKey error:&error])
            return handleError(result, callbacks, fsPath, error);
#if defined(NSURLUbiquitousItemIsExcludedFromSyncKey)
        [url setResourceValue:@NO forKey:NSURLUbiquitousItemIsExcludedFromSyncKey error:nil];
#endif
        if (callbacks.onStatus)
            callbacks.onStatus("Pinned locally: " + fsPath.string());
        break;
    case ActionKind::OptimizeStorage:
        if (![url setResourceValue:@NO forKey:NSURLUbiquitousItemDownloadRequestedKey error:&error])
            return handleError(result, callbacks, fsPath, error);
        if (callbacks.onStatus)
            callbacks.onStatus("Allowing macOS to optimize: " + fsPath.string());
        break;
    case ActionKind::PauseSync:
    case ActionKind::ResumeSync:
        return applyPauseState(action, url, result, callbacks);
    case ActionKind::RevealInFinder:
        // handled separately
        return true;
    }

    if (callbacks.onItem)
    {
        if (!callbacks.onItem(fsPath, bytes))
        {
            result.cancelled = true;
            return false;
        }
    }
    result.processedItems += 1;
    result.processedBytes += bytes;
    return true;
}

} // namespace

OperationResult performCloudOperation(ActionKind action,
                                      const std::filesystem::path &root,
                                      const OperationCallbacks &callbacks,
                                      bool recursive)
{
    __block OperationResult result;
    @autoreleasepool
    {
        NSFileManager *fileManager = [NSFileManager defaultManager];
        std::string rootStr = root.string();
        NSString *rootPath = [[NSString alloc] initWithUTF8String:rootStr.c_str()];
        BOOL isDirectory = NO;
        if (![fileManager fileExistsAtPath:rootPath isDirectory:&isDirectory])
        {
            result.success = false;
            result.errorMessage = "Target path does not exist.";
            return result;
        }
        NSURL *rootURL = [NSURL fileURLWithPath:rootPath isDirectory:isDirectory];
        if (!rootURL)
        {
            result.success = false;
            result.errorMessage = "Unable to create file URL for target path.";
            return result;
        }

        if (action == ActionKind::PauseSync || action == ActionKind::ResumeSync)
        {
        if (!performOperation(action, rootURL, nil, fileManager, result, callbacks))
            return result;
        if (!result.cancelled && callbacks.onItem)
        {
            if (!callbacks.onItem(root, 0))
                result.cancelled = true;
        }
        return result;
    }

        if (callbacks.onStatus)
            callbacks.onStatus("Scanning for iCloud items...");

        NSDirectoryEnumerationOptions options = NSDirectoryEnumerationSkipsHiddenFiles;
        if (!recursive)
            options |= NSDirectoryEnumerationSkipsSubdirectoryDescendants;

        NSDirectoryEnumerator<NSURL *> *enumerator = [fileManager enumeratorAtURL:rootURL
                                                     includingPropertiesForKeys:resourceKeys()
                                                                        options:options
                                                                   errorHandler:^BOOL(NSURL *url, NSError *error) {
                                                                     handleError(result, callbacks, urlToPath(url), error);
                                                                     return NO;
                                                                   }];
        if (!enumerator)
        {
            result.success = false;
            result.errorMessage = "Unable to enumerate directory.";
            return result;
        }

        for (NSURL *url in enumerator)
        {
            if (callbacks.isCancelled && callbacks.isCancelled())
            {
                result.cancelled = true;
                break;
            }

            NSError *valueError = nil;
            NSDictionary<NSURLResourceKey, id> *values =
                [url resourceValuesForKeys:resourceKeys() error:&valueError];
            if (!values)
            {
                handleError(result, callbacks, urlToPath(url), valueError);
                break;
            }

            if (!performOperation(action, url, values, fileManager, result, callbacks))
            {
                if (!result.cancelled && !result.success)
                    break;
                if (result.cancelled)
                    break;
            }
        }
    }
    if (!result.cancelled && result.errorMessage.empty())
        result.success = true;
    return result;
}

OperationResult revealInFinder(const std::filesystem::path &path)
{
    OperationResult result;
    @autoreleasepool
    {
        std::string pathStr = path.string();
        NSString *nsPath = [[NSString alloc] initWithUTF8String:pathStr.c_str()];
        NSURL *url = [NSURL fileURLWithPath:nsPath];
        if (!url)
        {
            result.success = false;
            result.errorMessage = "Unable to open Finder for the selected path.";
            return result;
        }
        NSWorkspace *workspace = [NSWorkspace sharedWorkspace];
        NSFileManager *fm = [NSFileManager defaultManager];
        BOOL isDir = NO;
        [fm fileExistsAtPath:nsPath isDirectory:&isDir];

        BOOL opened = NO;
        if (isDir)
            opened = [workspace selectFile:nil inFileViewerRootedAtPath:nsPath];
        else
        {
            NSString *parent = [nsPath stringByDeletingLastPathComponent];
            opened = [workspace selectFile:nsPath inFileViewerRootedAtPath:parent];
        }

        if (!opened)
        {
            result.success = false;
            result.errorMessage = "Failed to tell Finder to reveal the path.";
            return result;
        }
    }
    result.success = true;
    result.processedItems = 1;
    return result;
}

bool supportsPauseResume(const std::filesystem::path &path)
{
    __block bool supported = false;
    @autoreleasepool
    {
        std::string pathStr = path.string();
        NSString *nsPath = [[NSString alloc] initWithUTF8String:pathStr.c_str()];
        NSURL *url = [NSURL fileURLWithPath:nsPath];
        if (!url)
            return false;
        NSError *error = nil;
        id value = nil;
        BOOL ok = [url getResourceValue:&value forKey:NSURLUbiquitousItemIsSyncPausedKey error:&error];
        supported = ok && value != nil;
    }
    return supported;
}

} // namespace ck::du::cloud
