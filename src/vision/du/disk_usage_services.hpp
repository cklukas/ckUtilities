#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

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

} // namespace ck::vision
