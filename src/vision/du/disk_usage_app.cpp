#include "disk_usage_app.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <cvision/widgets/splitter.hpp>
#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/text_layout.hpp>
#include <cvision/widgets/text_view.hpp>

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
    : application_(application), snapshot_(std::move(snapshot))
{
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    if (snapshot_.root != nullptr)
    {
        create_window("Disk usage: " + snapshot_.root->path.string());
        rebuild_snapshot_view();
    }
}

DiskUsageApp::DiskUsageApp(ckv::ui::Application &application,
                           DiskUsageScanService &scan_service,
                           DiskUsageFileListService &file_list_service,
                           std::filesystem::path root,
                           ck::du::BuildDirectoryTreeOptions options)
    : application_(application),
      scan_service_(&scan_service),
      file_list_service_(&file_list_service),
      scan_root_(std::move(root)),
      scan_options_(std::move(options))
{
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    create_window("Disk usage: scanning");
    start_scan();
}

DiskUsageApp::~DiskUsageApp()
{
    lifetime_.reset();
    if (scan_service_ != nullptr)
        scan_service_->cancel();
    if (file_list_service_ != nullptr)
        file_list_service_->cancel();
}

void DiskUsageApp::declare_commands()
{
    rescan_command_ = application_.commands().declare({
        .key = "ck.du.rescan", .title = "&Rescan", .category = "Disk usage", .chord = "F5",
        .visibility = ckv::ui::CommandVisibility::Palette,
        .handler = [this] { start_scan(); }});
    cancel_scan_command_ = application_.commands().declare({
        .key = "ck.du.cancel_scan", .title = "&Cancel scan", .category = "Disk usage", .chord = "Ctrl+C",
        .visibility = ckv::ui::CommandVisibility::Palette,
        .handler = [this] { cancel_scan(); }});
    view_files_command_ = application_.commands().declare({
        .key = "ck.du.view_files", .title = "&View files", .category = "Disk usage", .chord = "Enter",
        .visibility = ckv::ui::CommandVisibility::Palette,
        .handler = [this] { view_selected_files(); }});
}

SuiteShellOptions DiskUsageApp::make_shell_options() const
{
    return {
        .application_name = "ck Disk Usage",
        .about_text = "A native ckVision browser for cancellable, application-owned disk-usage snapshots.",
        .application_menus = {ckv::widgets::MenuBarItem{"&Scan", {
            ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{rescan_command_, "&Rescan"}),
            ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{cancel_scan_command_, "&Cancel scan"}),
            ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{view_files_command_, "&View files"}),
        }}},
        .application_status_items = {
            ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{rescan_command_, "&Rescan"}, 30},
            ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{cancel_scan_command_, "&Cancel"}, 30},
            ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{view_files_command_, "&Files"}, 25},
        },
    };
}

void DiskUsageApp::create_window(std::string title)
{
    auto window = std::make_unique<ckv::widgets::Window>(std::move(title));
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{60, 16});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    window->on_closed = [this] { application_.request_quit(); };
    window_ = shell_->desktop().add_window(std::move(window));
}

void DiskUsageApp::show_scan_state(std::string text)
{
    if (window_ == nullptr)
        create_window("Disk usage: scanning");
    window_->set_title("Disk usage: scanning");
    window_->set_footer("Scanning " + scan_root_.string());
    auto content = std::make_unique<ckv::widgets::TextView>();
    content->set_wrap_mode(ckv::widgets::WrapMode::Word);
    content->set_text(std::move(text));
    window_->set_content(std::move(content));
    tree_ = nullptr;
    table_ = nullptr;
}

void DiskUsageApp::start_scan()
{
    if (scan_service_ == nullptr)
    {
        if (window_ != nullptr)
            window_->set_footer("No scan service was provided for this supplied snapshot.");
        return;
    }
    if (scan_service_->running())
    {
        show_scan_state("A disk-usage scan is already running. Use Cancel scan before starting another.");
        return;
    }

    snapshot_ = {};
    show_scan_state("Scanning disk usage in the background. The current directory is shown in the window footer.");
    const std::weak_ptr<void> lifetime = lifetime_;
    scan_service_->start(scan_root_, scan_options_,
                         [this, lifetime](const std::filesystem::path &path) {
                             application_.post([this, lifetime, path] {
                                 if (lifetime.expired() || window_ == nullptr)
                                     return;
                                 window_->set_footer("Scanning " + path.string());
                             });
                         },
                         [this, lifetime](ck::du::BuildDirectoryTreeResult result) mutable {
                             auto delivered = std::make_shared<ck::du::BuildDirectoryTreeResult>(std::move(result));
                             application_.post([this, lifetime, delivered] {
                                 if (lifetime.expired())
                                     return;
                                 complete_scan(std::move(*delivered));
                             });
                         });
}

void DiskUsageApp::cancel_scan()
{
    if (scan_service_ == nullptr)
    {
        if (window_ != nullptr)
            window_->set_footer("No scan service was provided for this supplied snapshot.");
        return;
    }
    if (scan_service_->running())
    {
        scan_service_->cancel();
        show_scan_state("Cancellation requested. The scan will stop at its next filesystem boundary.");
        return;
    }
    if (file_list_service_ != nullptr && file_list_service_->running())
    {
        file_list_service_->cancel();
        if (window_ != nullptr)
            window_->set_footer("Cancellation requested for the file list.");
        return;
    }
    if (window_ != nullptr)
        window_->set_footer("There is no running disk-usage operation to cancel.");
}

void DiskUsageApp::complete_scan(ck::du::BuildDirectoryTreeResult snapshot)
{
    snapshot_ = std::move(snapshot);
    if (snapshot_.cancelled)
    {
        show_scan_state("Disk-usage scan cancelled. Run Rescan to build a new snapshot.");
        return;
    }
    if (snapshot_.root == nullptr)
    {
        show_scan_state("Disk-usage scan did not produce a directory snapshot.");
        return;
    }
    window_->set_title("Disk usage: " + snapshot_.root->path.string());
    rebuild_snapshot_view();
}

void DiskUsageApp::view_selected_files()
{
    const ck::du::DirectoryNode *selected = selected_directory();
    if (selected == nullptr)
    {
        if (window_ != nullptr)
            window_->set_footer("Select a directory before viewing its files.");
        return;
    }
    if (file_list_service_ == nullptr)
    {
        if (window_ != nullptr)
            window_->set_footer("No file-list service was provided for this snapshot.");
        return;
    }
    if (file_list_service_->running())
    {
        if (window_ != nullptr)
            window_->set_footer("A file list is already being built.");
        return;
    }

    const std::filesystem::path directory = selected->path;
    if (window_ != nullptr)
        window_->set_footer("Listing files in " + directory.string());
    const std::weak_ptr<void> lifetime = lifetime_;
    file_list_service_->start(directory, false, scan_options_,
                              [this, lifetime, directory](DiskUsageFileListResult result) mutable {
                                  auto delivered = std::make_shared<DiskUsageFileListResult>(std::move(result));
                                  application_.post([this, lifetime, directory, delivered] {
                                      if (lifetime.expired())
                                          return;
                                      complete_file_list(std::move(*delivered), directory);
                                  });
                              });
}

void DiskUsageApp::complete_file_list(DiskUsageFileListResult result, std::filesystem::path directory)
{
    if (result.cancelled)
    {
        if (window_ != nullptr)
            window_->set_footer("File list cancelled for " + directory.string());
        return;
    }

    auto window = std::make_unique<ckv::widgets::Window>("Files: " + directory.string());
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{68, 16});
    auto table = std::make_unique<ckv::widgets::Table>();
    table->set_columns({{"Path", 42, 12}, {"Size", 14, 8}, {"Modified", 18, 10}, {"Cloud", 16, 8}});
    std::vector<std::vector<std::string>> rows;
    rows.reserve(result.files.size());
    for (const auto &file : result.files)
    {
        rows.push_back({file.displayPath, ck::du::formatSize(file.size), file.modified,
                        file.iCloudStatus.empty() ? "Local" : file.iCloudStatus});
    }
    table->set_rows(std::move(rows));
    window->set_content(std::move(table));
    window->set_footer(std::to_string(result.files.size()) + " files");
    shell_->desktop().add_window(std::move(window));
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
    const std::uint64_t root_id = roots.front().id;
    tree_->set_roots(std::move(roots));
    tree_->reveal_and_select(root_id);

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

bool DiskUsageApp::scan_running() const noexcept
{
    return scan_service_ != nullptr && scan_service_->running();
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
