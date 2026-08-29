#include "disk_usage_app.hpp"

#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

namespace
{

void require(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

ck::du::BuildDirectoryTreeResult make_snapshot()
{
    ck::du::BuildDirectoryTreeResult snapshot;
    snapshot.root = std::make_unique<ck::du::DirectoryNode>();
    snapshot.root->path = "/workspace";
    snapshot.root->stats = {.totalSize = 4096, .fileCount = 3, .directoryCount = 2};

    auto source = std::make_unique<ck::du::DirectoryNode>();
    source->path = "/workspace/src";
    source->parent = snapshot.root.get();
    source->stats = {.totalSize = 3072, .fileCount = 2, .directoryCount = 0};
    snapshot.root->children.push_back(std::move(source));

    auto docs = std::make_unique<ck::du::DirectoryNode>();
    docs->path = "/workspace/docs";
    docs->parent = snapshot.root.get();
    docs->stats = {.totalSize = 1024, .fileCount = 1, .directoryCount = 0};
    snapshot.root->children.push_back(std::move(docs));
    return snapshot;
}

class ManualScanService final : public ck::vision::DiskUsageScanService
{
public:
    void start(std::filesystem::path,
               ck::du::BuildDirectoryTreeOptions,
               ProgressHandler on_progress,
               CompletionHandler on_complete) override
    {
        running_ = true;
        on_progress_ = std::move(on_progress);
        on_complete_ = std::move(on_complete);
    }

    void cancel() noexcept override { cancelled_ = true; }
    bool running() const noexcept override { return running_; }

    void complete(ck::du::BuildDirectoryTreeResult snapshot)
    {
        running_ = false;
        on_complete_(std::move(snapshot));
    }

    void report_progress(std::filesystem::path path)
    {
        on_progress_(path);
    }

    bool cancelled() const noexcept { return cancelled_; }

private:
    bool running_ = false;
    bool cancelled_ = false;
    ProgressHandler on_progress_;
    CompletionHandler on_complete_;
};

class ManualFileListService final : public ck::vision::DiskUsageFileListService
{
public:
    void start(std::filesystem::path directory,
               bool recursive,
               ck::du::BuildDirectoryTreeOptions,
               CompletionHandler on_complete) override
    {
        running_ = true;
        directory_ = std::move(directory);
        recursive_ = recursive;
        on_complete_ = std::move(on_complete);
    }

    void cancel() noexcept override { cancelled_ = true; }
    bool running() const noexcept override { return running_; }

    void complete(ck::vision::DiskUsageFileListResult result)
    {
        running_ = false;
        on_complete_(std::move(result));
    }

    const std::filesystem::path &directory() const noexcept { return directory_; }
    bool recursive() const noexcept { return recursive_; }
    bool cancelled() const noexcept { return cancelled_; }

private:
    bool running_ = false;
    bool cancelled_ = false;
    bool recursive_ = false;
    std::filesystem::path directory_;
    CompletionHandler on_complete_;
};

class ManualCloudService final : public ck::vision::DiskUsageCloudService
{
public:
    ck::vision::DiskUsageCloudCapability capability(ck::vision::DiskUsageCloudAction,
                                                     const std::filesystem::path &) const override
    {
        return {.available = true, .reason = {}};
    }

    void start(ck::vision::DiskUsageCloudAction action,
               std::filesystem::path target,
               ProgressHandler on_progress,
               CompletionHandler on_complete) override
    {
        running_ = true;
        action_ = action;
        target_ = std::move(target);
        on_progress_ = std::move(on_progress);
        on_complete_ = std::move(on_complete);
    }

    void cancel() noexcept override { cancelled_ = true; }
    bool running() const noexcept override { return running_; }

    void complete(ck::vision::DiskUsageCloudOperationResult result)
    {
        running_ = false;
        on_complete_(std::move(result));
    }

    void report_progress(ck::vision::DiskUsageCloudProgress progress)
    {
        on_progress_(std::move(progress));
    }

    bool cancelled() const noexcept { return cancelled_; }
    ck::vision::DiskUsageCloudAction action() const noexcept { return action_; }
    const std::filesystem::path &target() const noexcept { return target_; }

private:
    bool running_ = false;
    bool cancelled_ = false;
    ck::vision::DiskUsageCloudAction action_ = ck::vision::DiskUsageCloudAction::Download;
    std::filesystem::path target_;
    ProgressHandler on_progress_;
    CompletionHandler on_complete_;
};

void verify_late_service_delivery_is_lifetime_safe()
{
    {
        ckv::ManualClock clock;
        ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
        ckv::ui::Application application(terminal, clock);
        ManualScanService scan_service;
        ManualFileListService file_list_service;
        ManualCloudService cloud_service;
        {
            ck::vision::DiskUsageApp disk_usage(
                application, scan_service, file_list_service, cloud_service, "/workspace");
            require(disk_usage.scan_running(),
                    "The test requires an active native disk-usage scan.");
        }
        require(scan_service.cancelled() && file_list_service.cancelled() &&
                    cloud_service.cancelled(),
                "Destroying disk usage must request cancellation from every supplied service.");
        scan_service.report_progress("/workspace/late-scan");
        scan_service.complete(make_snapshot());
        application.step(0);
        require(application.current_frame().size() == ckv::Size{100, 30},
                "Late scan delivery after destruction must be safely ignored.");
    }

    {
        ckv::ManualClock clock;
        ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
        ckv::ui::Application application(terminal, clock);
        ManualScanService scan_service;
        ManualFileListService file_list_service;
        ManualCloudService cloud_service;
        {
            ck::vision::DiskUsageApp disk_usage(
                application, scan_service, file_list_service, cloud_service, "/workspace");
            scan_service.complete(make_snapshot());
            application.step(0);
            require(application.execute_command(disk_usage.view_files_command()) &&
                        file_list_service.running(),
                    "The test requires an active native file-list request.");
        }
        require(file_list_service.cancelled(),
                "Destroying disk usage must cancel an active file-list request.");
        file_list_service.complete({{}, false});
        application.step(0);
        require(application.current_frame().size() == ckv::Size{100, 30},
                "Late file-list delivery after destruction must be safely ignored.");
    }

    {
        ckv::ManualClock clock;
        ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
        ckv::ui::Application application(terminal, clock);
        ManualScanService scan_service;
        ManualFileListService file_list_service;
        ManualCloudService cloud_service;
        {
            ck::vision::DiskUsageApp disk_usage(
                application, scan_service, file_list_service, cloud_service, "/workspace");
            scan_service.complete(make_snapshot());
            application.step(0);
            require(application.execute_command(disk_usage.download_cloud_command()) &&
                        cloud_service.running(),
                    "The test requires an active native cloud request.");
        }
        require(cloud_service.cancelled(),
                "Destroying disk usage must cancel an active cloud request.");
        cloud_service.report_progress(
            {.message = "Late cloud progress.", .completed_items = 1, .total_items = 1});
        cloud_service.complete({.success = true, .cancelled = false, .processed_items = 1,
                                .message = "Late cloud completion."});
        application.step(0);
        require(application.current_frame().size() == ckv::Size{100, 30},
                "Late cloud delivery after destruction must be safely ignored.");
    }
}

} // namespace

int main()
{
    verify_late_service_delivery_is_lifetime_safe();

    {
        ck::vision::UnsupportedDiskUsageCloudService unsupported_cloud;
        const auto capability = unsupported_cloud.capability(ck::vision::DiskUsageCloudAction::Download, "/workspace");
        require(!capability.available && !capability.reason.empty(),
                "Unsupported cloud platforms must report a concrete unavailable state.");
        std::optional<ck::vision::DiskUsageCloudOperationResult> result;
        unsupported_cloud.start(ck::vision::DiskUsageCloudAction::Download, "/workspace", {},
                                [&](ck::vision::DiskUsageCloudOperationResult completed) { result = std::move(completed); });
        require(result.has_value() && !result->success && !result->cancelled && !result->message.empty(),
                "Unsupported cloud operations must fail explicitly instead of pretending to change storage.");
    }

    {
        ckv::ManualClock clock;
        ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
        ckv::ui::Application application(terminal, clock);
        ck::vision::DiskUsageApp disk_usage(application, make_snapshot());

        require(disk_usage.root_directory() != nullptr, "The native disk-usage app must retain the scan snapshot.");
        require(disk_usage.tree() != nullptr && disk_usage.table() != nullptr,
                "The native disk-usage app must compose a tree and selected-directory table.");
        require(disk_usage.table()->row_count() == 2,
                "The selected root directory must populate its child-directory table.");
        application.step(0);
        require(application.current_frame().size() == ckv::Size{100, 30},
                "The native disk-usage snapshot must render headlessly.");
    }

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ManualScanService scan_service;
    ManualFileListService file_list_service;
    ManualCloudService cloud_service;
    ck::vision::DiskUsageApp disk_usage(application, scan_service, file_list_service, cloud_service, "/workspace");

    require(disk_usage.scan_running(), "A native disk-usage scan must be delegated to the injected service.");
    require(application.execute_command(disk_usage.cancel_scan_command()), "Cancellation must be a registry command.");
    require(scan_service.cancelled(), "Cancellation must be delegated to the scan service.");
    scan_service.complete(make_snapshot());
    application.step(0);
    require(disk_usage.root_directory() != nullptr && disk_usage.table() != nullptr,
            "A completed scan snapshot must be mapped into native tree and table views.");
    require(disk_usage.selected_directory() == disk_usage.root_directory(),
            "A completed scan must establish a stable root selection.");
    require(application.execute_command(disk_usage.view_files_command()), "File listing must be a registry command.");
    require(file_list_service.running() && file_list_service.directory() == "/workspace" && !file_list_service.recursive(),
            "The selected directory must be listed through the injected file-list service.");
    ck::du::FileEntry listed;
    listed.displayPath = "notes.md";
    listed.size = 42;
    listed.modified = "2026-08-29 12:00";
    file_list_service.complete({{listed}, false});
    application.step(0);
    require(disk_usage.desktop_window_count() == 2,
            "A completed file list must open a native Table window.");
    require(application.current_frame().size() == ckv::Size{100, 30},
            "The completed disk-usage scan must render headlessly.");

    require(application.execute_command(disk_usage.download_cloud_command()),
            "Downloading cloud content must be a native command.");
    require(disk_usage.cloud_operation_running() && cloud_service.action() == ck::vision::DiskUsageCloudAction::Download &&
                cloud_service.target() == "/workspace",
            "The selected directory must be submitted to the injected cloud service.");
    cloud_service.complete({.success = true,
                            .cancelled = false,
                            .processed_items = 1,
                            .message = "Download request accepted."});
    application.step(0);
    require(application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}}),
            "The cloud-operation result must be presented as a dismissible native message.");
    application.step(0);

    require(application.execute_command(disk_usage.evict_cloud_command()),
            "Freeing local cloud copies must be a native command.");
    require(!disk_usage.cloud_operation_running(),
            "Freeing local copies must wait for explicit confirmation before reaching the cloud service.");
    require(application.dispatch(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Enter, ckv::Modifier::None, ""}}),
            "The destructive cloud action must require an explicit Yes confirmation.");
    application.step(0);
    require(disk_usage.cloud_operation_running() && cloud_service.action() == ck::vision::DiskUsageCloudAction::EvictLocalCopies,
            "A confirmed free-local-copies action must reach the injected cloud service.");
    require(application.execute_command(disk_usage.cancel_cloud_command()),
            "Cloud cancellation must be a native command.");
    require(cloud_service.cancelled(), "Cloud cancellation must be delegated to the injected service.");
    cloud_service.complete({.success = false, .cancelled = true, .processed_items = 0, .message = "Cloud operation cancelled."});
    application.step(0);

    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "ck-vision-du-scan-service-test";
    fs::remove_all(directory);
    fs::create_directories(directory);
    {
        std::ofstream file(directory / "item.txt");
        file << "data";
    }
    ck::vision::ThreadedDiskUsageScanService threaded_scan;
    std::mutex completion_mutex;
    std::condition_variable completion_ready;
    std::optional<ck::du::BuildDirectoryTreeResult> threaded_result;
    threaded_scan.start(directory, {.reportErrors = false}, {}, [&](ck::du::BuildDirectoryTreeResult result) {
        {
            std::scoped_lock lock(completion_mutex);
            threaded_result.emplace(std::move(result));
        }
        completion_ready.notify_one();
    });
    {
        std::unique_lock lock(completion_mutex);
        require(completion_ready.wait_for(lock, std::chrono::seconds(2), [&] { return threaded_result.has_value(); }),
                "The threaded disk-usage service must complete a filesystem scan.");
    }
    require(threaded_result->root != nullptr && !threaded_result->cancelled,
            "The threaded disk-usage service must return an owned scan snapshot.");
    ck::vision::ThreadedDiskUsageFileListService threaded_file_list;
    std::optional<ck::vision::DiskUsageFileListResult> file_list_result;
    threaded_file_list.start(directory, false, {.reportErrors = false}, [&](ck::vision::DiskUsageFileListResult result) {
        {
            std::scoped_lock lock(completion_mutex);
            file_list_result.emplace(std::move(result));
        }
        completion_ready.notify_one();
    });
    {
        std::unique_lock lock(completion_mutex);
        require(completion_ready.wait_for(lock, std::chrono::seconds(2), [&] { return file_list_result.has_value(); }),
                "The threaded file-list service must complete a filesystem enumeration.");
    }
    require(!file_list_result->cancelled && file_list_result->files.size() == 1 &&
                file_list_result->files.front().displayPath == "item.txt",
            "The threaded file-list service must return typed file entries.");
    fs::remove_all(directory);
}
