#pragma once

#include <any>
#include <cstdint>
#include <memory>
#include <string>

#include <cvision/ui/application.hpp>
#include <cvision/widgets/table.hpp>
#include <cvision/widgets/tree_view.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/vision/suite_shell.hpp"
#include "disk_usage_core.hpp"

namespace ck::vision
{

// A native ckVision presentation for an immutable, application-owned scan
// result. The composition root supplies the scan result; the view graph only
// maps that snapshot into retained tree/table models.
class DiskUsageApp
{
public:
    DiskUsageApp(ckv::ui::Application &application, ck::du::BuildDirectoryTreeResult snapshot);

    const ck::du::DirectoryNode *selected_directory() const noexcept;
    const ck::du::DirectoryNode *root_directory() const noexcept { return snapshot_.root.get(); }
    bool scan_cancelled() const noexcept { return snapshot_.cancelled; }
    std::size_t desktop_window_count() const noexcept { return shell_->desktop().windows().size(); }
    ckv::widgets::TreeView *tree() const noexcept { return tree_; }
    ckv::widgets::Table *table() const noexcept { return table_; }

private:
    SuiteShellOptions make_shell_options() const;
    void create_snapshot_window();
    void rebuild_snapshot_view();
    ckv::widgets::TreeNode make_tree_node(ck::du::DirectoryNode &node);
    void show_directory(ck::du::DirectoryNode &node);
    std::string directory_label(const ck::du::DirectoryNode &node) const;

    ckv::ui::Application &application_;
    ck::du::BuildDirectoryTreeResult snapshot_;
    std::unique_ptr<SuiteShell> shell_;
    std::uint64_t next_node_id_ = 1;
    ckv::widgets::Window *window_ = nullptr;
    ckv::widgets::TreeView *tree_ = nullptr;
    ckv::widgets::Table *table_ = nullptr;
};

} // namespace ck::vision
