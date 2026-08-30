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
    source->stats = {.totalSize = 3072, .fileCount = 2, .directoryCount = 1};
    auto generated = std::make_unique<ck::du::DirectoryNode>();
    generated->path = "/workspace/src/generated";
    generated->parent = source.get();
    generated->stats = {.totalSize = 1024, .fileCount = 1, .directoryCount = 0};
    source->children.push_back(std::move(generated));
    snapshot.root->children.push_back(std::move(source));

    auto docs = std::make_unique<ck::du::DirectoryNode>();
    docs->path = "/workspace/docs";
    docs->parent = snapshot.root.get();
    docs->stats = {.totalSize = 1024, .fileCount = 1, .directoryCount = 0};
    snapshot.root->children.push_back(std::move(docs));
    return snapshot;
}

ck::du::BuildDirectoryTreeResult make_refreshed_snapshot()
{
    ck::du::BuildDirectoryTreeResult snapshot;
    snapshot.root = std::make_unique<ck::du::DirectoryNode>();
    snapshot.root->path = "/workspace";
    snapshot.root->stats = {.totalSize = 8192, .fileCount = 6, .directoryCount = 2};

    auto source = std::make_unique<ck::du::DirectoryNode>();
    source->path = "/workspace/src";
    source->parent = snapshot.root.get();
    source->stats = {.totalSize = 7168, .fileCount = 5, .directoryCount = 1};
    auto generated = std::make_unique<ck::du::DirectoryNode>();
    generated->path = "/workspace/src/generated";
    generated->parent = source.get();
    generated->stats = {.totalSize = 2048, .fileCount = 2, .directoryCount = 0};
    source->children.push_back(std::move(generated));
    snapshot.root->children.push_back(std::move(source));

    auto tests = std::make_unique<ck::du::DirectoryNode>();
    tests->path = "/workspace/tests";
    tests->parent = snapshot.root.get();
    tests->stats = {.totalSize = 1024, .fileCount = 1, .directoryCount = 0};
    snapshot.root->children.push_back(std::move(tests));
    return snapshot;
}

ck::du::BuildDirectoryTreeResult make_wide_snapshot(std::size_t entries)
{
    ck::du::BuildDirectoryTreeResult snapshot;
    snapshot.root = std::make_unique<ck::du::DirectoryNode>();
    snapshot.root->path = "/workspace";
    snapshot.root->stats = {.totalSize = entries, .fileCount = entries, .directoryCount = entries};
    snapshot.root->children.reserve(entries);
    for (std::size_t index = 0; index < entries; ++index)
    {
        auto child = std::make_unique<ck::du::DirectoryNode>();
        child->path = "/workspace/entry-" + std::to_string(index);
        child->parent = snapshot.root.get();
        child->stats = {.totalSize = index + 1, .fileCount = 1, .directoryCount = 0};
        snapshot.root->children.push_back(std::move(child));
    }
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

#if defined(__APPLE__)
    {
        // A normal temporary directory is not an iCloud item. This exercises
        // the real Foundation adapter without requesting a provider mutation.
        ck::vision::MacDiskUsageCloudService mac_cloud;
        const std::filesystem::path non_cloud_path = std::filesystem::temp_directory_path();
        const auto capability = mac_cloud.capability(ck::vision::DiskUsageCloudAction::Download, non_cloud_path);
        require(!capability.available && !capability.reason.empty(),
                "The macOS cloud adapter must reject a path that is not managed by iCloud Drive.");

        std::mutex completion_mutex;
        std::condition_variable completion_ready;
        std::optional<ck::vision::DiskUsageCloudOperationResult> completion;
        mac_cloud.start(ck::vision::DiskUsageCloudAction::Download, non_cloud_path, {},
                        [&](ck::vision::DiskUsageCloudOperationResult result) {
                            {
                                std::scoped_lock lock(completion_mutex);
                                completion = std::move(result);
                            }
                            completion_ready.notify_one();
                        });
        std::unique_lock lock(completion_mutex);
        require(completion_ready.wait_for(lock, std::chrono::seconds(5), [&] { return completion.has_value(); }),
                "The macOS cloud adapter must complete a rejected non-cloud request.");
        require(!completion->success && !completion->cancelled && !completion->message.empty() && !mac_cloud.running(),
                "The macOS cloud adapter must fail a non-cloud request without claiming a provider change.");
    }
#endif

    {
        ckv::ManualClock clock;
        ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
        ckv::ui::Application application(terminal, clock);
        ck::vision::DiskUsageApp disk_usage(application, make_snapshot());

        require(disk_usage.root_directory() != nullptr, "The native disk-usage app must retain the scan snapshot.");
        require(disk_usage.tree() != nullptr && disk_usage.tree()->model() != nullptr && disk_usage.table() != nullptr,
                "The native disk-usage app must compose a provider-backed tree and selected-directory table.");
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
    require(disk_usage.root_directory() != nullptr && disk_usage.tree() != nullptr &&
                disk_usage.tree()->model() != nullptr && disk_usage.table() != nullptr,
            "A completed scan snapshot must be mapped into provider-backed tree and table views.");
    require(disk_usage.selected_directory() == disk_usage.root_directory(),
            "A completed scan must establish a stable root selection.");
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Down, ckv::Modifier::None, ""}});
    application.step(0);
    require(disk_usage.selected_directory() != nullptr && disk_usage.selected_directory()->path == "/workspace/src",
            "The disk-usage tree must navigate to a child before its refresh scenario.");
    const auto retained_selection_id = disk_usage.tree()->selected_id();
    auto *const retained_model = disk_usage.tree()->model();
    require(retained_selection_id.has_value() && retained_model != nullptr,
            "The disk-usage refresh fixture must expose a stable provider selection.");
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Right, ckv::Modifier::None, ""}});
    application.step(0);
    require(disk_usage.tree()->item_expanded(*retained_selection_id),
            "The disk-usage refresh fixture must expand its selected directory.");
    require(application.execute_command(disk_usage.rescan_command()) && disk_usage.scan_running() &&
                disk_usage.tree()->model() == retained_model &&
                disk_usage.selected_directory()->path == "/workspace/src",
            "A rescan must retain the last valid disk-usage snapshot until replacement succeeds.");
    scan_service.complete(make_refreshed_snapshot());
    application.step(0);
    require(disk_usage.tree()->model() == retained_model &&
                disk_usage.tree()->selected_id() == retained_selection_id &&
                disk_usage.tree()->item_expanded(*retained_selection_id) &&
                disk_usage.selected_directory() != nullptr &&
                disk_usage.selected_directory()->path == "/workspace/src" &&
                disk_usage.selected_directory()->stats.totalSize == 7168,
            "A refreshed disk snapshot must retain its provider and surviving directory selection.");
    terminal.inject_event(ckv::KeyEvent{ckv::KeyChord{ckv::Key::Up, ckv::Modifier::None, ""}});
    application.step(0);
    require(disk_usage.selected_directory() == disk_usage.root_directory(),
            "Disk-usage navigation must return to the root after refresh acceptance.");
    const ck::du::DirectoryNode *const last_completed_root = disk_usage.root_directory();
    require(application.execute_command(disk_usage.rescan_command()),
            "A cancelled disk-usage refresh must start through the native rescan command.");
    auto cancelled_snapshot = make_snapshot();
    cancelled_snapshot.cancelled = true;
    scan_service.complete(std::move(cancelled_snapshot));
    application.step(0);
    require(disk_usage.root_directory() == last_completed_root &&
                disk_usage.tree()->model() == retained_model,
            "A cancelled disk-usage refresh must retain the last completed provider snapshot.");
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

    constexpr std::size_t scale_entries = 2048;
    require(application.execute_command(disk_usage.rescan_command()),
            "The application-scale disk snapshot must start through the native rescan command.");
    scan_service.complete(make_wide_snapshot(scale_entries));
    application.step(0);
    const auto terminal_cells = static_cast<std::size_t>(terminal.size().width) *
                                static_cast<std::size_t>(terminal.size().height);
    require(disk_usage.provider_node_count() == scale_entries + 1 &&
                application.last_compose_cells_touched() <= terminal_cells,
            "The application-scale disk snapshot exceeded its visible-frame provider budget.");
    require(application.execute_command(disk_usage.rescan_command()),
            "The small disk snapshot must start through the native rescan command.");
    scan_service.complete(make_snapshot());
    application.step(0);
    require(disk_usage.provider_node_count() == 4 &&
                application.last_compose_cells_touched() <= terminal_cells,
            "A small disk refresh retained indexes from the obsolete large snapshot.");

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
