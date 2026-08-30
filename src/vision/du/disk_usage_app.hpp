#pragma once

#include <any>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <cvision/ui/application.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/table.hpp>
#include <cvision/widgets/tree_view.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/vision/suite_shell.hpp"
#include "disk_usage_core.hpp"
#include "disk_usage_services.hpp"

namespace ck::vision
{

// A native ckVision presentation for application-owned disk-usage snapshots.
// It exposes their stable node identities through a borrowed TreeModel rather
// than duplicating the snapshot into a presentation tree. It can render a
// supplied snapshot or request one from an injected service; view code never
// traverses the filesystem or owns a worker thread.
class DiskUsageApp
{
public:
    DiskUsageApp(ckv::ui::Application &application, ck::du::BuildDirectoryTreeResult snapshot);
    DiskUsageApp(ckv::ui::Application &application,
                 DiskUsageScanService &scan_service,
                 DiskUsageFileListService &file_list_service,
                 std::filesystem::path root,
                 ck::du::BuildDirectoryTreeOptions options = {});
    DiskUsageApp(ckv::ui::Application &application,
                 DiskUsageScanService &scan_service,
                 DiskUsageFileListService &file_list_service,
                 DiskUsageCloudService &cloud_service,
                 std::filesystem::path root,
                 ck::du::BuildDirectoryTreeOptions options = {});
    ~DiskUsageApp();

    const ck::du::DirectoryNode *selected_directory() const noexcept;
    const ck::du::DirectoryNode *root_directory() const noexcept { return snapshot_.root.get(); }
    bool scan_cancelled() const noexcept { return snapshot_.cancelled; }
    bool scan_running() const noexcept;
    ckv::ui::CommandId rescan_command() const noexcept { return rescan_command_; }
    ckv::ui::CommandId cancel_scan_command() const noexcept { return cancel_scan_command_; }
    ckv::ui::CommandId view_files_command() const noexcept { return view_files_command_; }
    ckv::ui::CommandId download_cloud_command() const noexcept { return download_cloud_command_; }
    ckv::ui::CommandId evict_cloud_command() const noexcept { return evict_cloud_command_; }
    ckv::ui::CommandId cancel_cloud_command() const noexcept { return cancel_cloud_command_; }
    std::size_t desktop_window_count() const noexcept { return shell_->desktop().windows().size(); }
    bool cloud_operation_running() const noexcept;
    ckv::widgets::TreeView *tree() const noexcept { return tree_; }
    ckv::widgets::Table *table() const noexcept { return table_; }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void create_window(std::string title);
    void show_scan_state(std::string text);
    void start_scan();
    void cancel_scan();
    void complete_scan(ck::du::BuildDirectoryTreeResult snapshot);
    void view_selected_files();
    void complete_file_list(DiskUsageFileListResult result, std::filesystem::path directory);
    void request_cloud_action(DiskUsageCloudAction action);
    void start_cloud_action(DiskUsageCloudAction action, std::filesystem::path target);
    void complete_cloud_action(DiskUsageCloudOperationResult result, std::filesystem::path target);
    void show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message);
    void rebuild_snapshot_view();
    void index_tree_nodes(ck::du::DirectoryNode &node, std::size_t sibling_index);
    void show_directory(ck::du::DirectoryNode &node);
    std::string directory_label(const ck::du::DirectoryNode &node) const;

    ckv::ui::Application &application_;
    ck::du::BuildDirectoryTreeResult snapshot_;
    DiskUsageScanService *scan_service_ = nullptr;
    DiskUsageFileListService *file_list_service_ = nullptr;
    DiskUsageCloudService *cloud_service_ = nullptr;
    std::filesystem::path scan_root_;
    ck::du::BuildDirectoryTreeOptions scan_options_;
    std::unordered_map<const ck::du::DirectoryNode *, std::uint64_t> node_ids_;
    std::unordered_map<std::uint64_t, ck::du::DirectoryNode *> nodes_by_id_;
    std::unordered_map<const ck::du::DirectoryNode *, std::size_t> sibling_indexes_;
    std::uint64_t next_node_id_ = 1;
    // TreeView borrows this provider and its domain-node indices. They must
    // therefore outlive the shell that owns the TreeView.
    std::unique_ptr<ckv::widgets::TreeModel> tree_model_;

    std::unique_ptr<SuiteShell> shell_;
    ckv::ui::CommandId rescan_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId cancel_scan_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId view_files_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId download_cloud_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId evict_cloud_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId cancel_cloud_command_ = ckv::ui::kInvalidCommand;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::optional<ckv::widgets::MessageBoxPresentation> cloud_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> message_box_;
    ckv::widgets::Window *window_ = nullptr;
    ckv::widgets::TreeView *tree_ = nullptr;
    ckv::widgets::Table *table_ = nullptr;
};

} // namespace ck::vision
