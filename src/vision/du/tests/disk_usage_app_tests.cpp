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
               ProgressHandler,
               CompletionHandler on_complete) override
    {
        running_ = true;
        on_complete_ = std::move(on_complete);
    }

    void cancel() noexcept override { cancelled_ = true; }
    bool running() const noexcept override { return running_; }

    void complete(ck::du::BuildDirectoryTreeResult snapshot)
    {
        running_ = false;
        on_complete_(std::move(snapshot));
    }

    bool cancelled() const noexcept { return cancelled_; }

private:
    bool running_ = false;
    bool cancelled_ = false;
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

private:
    bool running_ = false;
    bool cancelled_ = false;
    bool recursive_ = false;
    std::filesystem::path directory_;
    CompletionHandler on_complete_;
};

} // namespace

int main()
{
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
    ck::vision::DiskUsageApp disk_usage(application, scan_service, file_list_service, "/workspace");

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
