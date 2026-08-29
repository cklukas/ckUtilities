#include "disk_usage_app.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <cvision/widgets/splitter.hpp>

namespace ck::vision
{
namespace
{

std::string node_name(const ck::du::DirectoryNode &node)
{
    std::string name = node.path.filename().string();
    return name.empty() ? node.path.string() : name;
}

} // namespace

DiskUsageApp::DiskUsageApp(ckv::ui::Application &application, ck::du::BuildDirectoryTreeResult snapshot)
    : application_(application), snapshot_(std::move(snapshot)),
      shell_(std::make_unique<SuiteShell>(application_, make_shell_options()))
{
    if (snapshot_.root != nullptr)
        create_snapshot_window();
}

SuiteShellOptions DiskUsageApp::make_shell_options() const
{
    return {
        .application_name = "ck Disk Usage",
        .about_text = "A native ckVision browser for an application-owned disk-usage snapshot.",
    };
}

void DiskUsageApp::create_snapshot_window()
{
    auto window = std::make_unique<ckv::widgets::Window>("Disk usage: " + snapshot_.root->path.string());
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{60, 16});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    window->on_closed = [this] { application_.request_quit(); };
    window_ = shell_->desktop().add_window(std::move(window));
    rebuild_snapshot_view();
}

void DiskUsageApp::rebuild_snapshot_view()
{
    if (window_ == nullptr || snapshot_.root == nullptr)
        return;

    next_node_id_ = 1;
    auto tree = std::make_unique<ckv::widgets::TreeView>();
    tree_ = tree.get();
    tree_->set_connector_style(ckv::widgets::TreeConnectorStyle::Outline);
    tree_->on_selection_changed = [this](ckv::widgets::TreeNode &selected) {
        ck::du::DirectoryNode *const *node = std::any_cast<ck::du::DirectoryNode *>(&selected.user_data);
        if (node != nullptr && *node != nullptr)
            show_directory(**node);
    };
    std::vector<ckv::widgets::TreeNode> roots;
    roots.push_back(make_tree_node(*snapshot_.root));
    tree_->set_roots(std::move(roots));

    auto table = std::make_unique<ckv::widgets::Table>();
    table_ = table.get();
    table_->set_columns({
        {"Directory", 32, 12},
        {"Files", 10, 5, ckv::widgets::TableCellType::Integer},
        {"Directories", 13, 5, ckv::widgets::TableCellType::Integer},
        {"Total size", 16, 8},
    });
    show_directory(*snapshot_.root);

    auto splitter = std::make_unique<ckv::widgets::Splitter>(window_->content_rect(), std::move(tree), std::move(table));
    window_->set_content(std::move(splitter));
    application_.set_focus(tree_);
}

ckv::widgets::TreeNode DiskUsageApp::make_tree_node(ck::du::DirectoryNode &node)
{
    ckv::widgets::TreeNode rendered;
    rendered.id = next_node_id_++;
    rendered.label = directory_label(node);
    rendered.expanded = node.parent == nullptr || node.expanded;
    rendered.children_known = true;
    rendered.user_data = &node;
    rendered.children.reserve(node.children.size());
    for (const auto &child : node.children)
        rendered.children.push_back(make_tree_node(*child));
    return rendered;
}

void DiskUsageApp::show_directory(ck::du::DirectoryNode &node)
{
    if (table_ == nullptr || window_ == nullptr)
        return;

    std::vector<ck::du::DirectoryNode *> children;
    children.reserve(node.children.size());
    for (const auto &child : node.children)
        children.push_back(child.get());
    std::sort(children.begin(), children.end(), [](const auto *left, const auto *right) {
        return left->path.filename().string() < right->path.filename().string();
    });

    std::vector<std::vector<std::string>> rows;
    rows.reserve(children.size());
    for (const ck::du::DirectoryNode *child : children)
    {
        rows.push_back({node_name(*child), std::to_string(child->stats.fileCount),
                        std::to_string(child->stats.directoryCount), ck::du::formatSize(child->stats.totalSize)});
    }
    table_->set_rows(std::move(rows));
    window_->set_footer(node.path.string() + " — " + std::to_string(node.stats.fileCount) + " files, " +
                        ck::du::formatSize(node.stats.totalSize));
}

std::string DiskUsageApp::directory_label(const ck::du::DirectoryNode &node) const
{
    return node_name(node) + "  [" + ck::du::formatSize(node.stats.totalSize) + ", " +
           std::to_string(node.stats.fileCount) + " files]";
}

const ck::du::DirectoryNode *DiskUsageApp::selected_directory() const noexcept
{
    if (tree_ == nullptr || tree_->selected() == nullptr)
        return nullptr;
    ck::du::DirectoryNode *const *node = std::any_cast<ck::du::DirectoryNode *>(&tree_->selected()->user_data);
    return node == nullptr ? nullptr : *node;
}

} // namespace ck::vision
