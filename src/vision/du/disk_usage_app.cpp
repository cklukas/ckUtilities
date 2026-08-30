#include "disk_usage_app.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include <cvision/widgets/splitter.hpp>
#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>
#include <cvision/widgets/text_layout.hpp>
#include <cvision/widgets/text_view.hpp>

#include "ck/vision/keymap.hpp"

namespace ck::vision
{
namespace
{

std::string node_name(const ck::du::DirectoryNode &node)
{
    std::string name = node.path.filename().string();
    return name.empty() ? node.path.string() : name;
}

std::string format_directory_label(const ck::du::DirectoryNode &node)
{
    return node_name(node) + "  [" + ck::du::formatSize(node.stats.totalSize) + ", " +
           std::to_string(node.stats.fileCount) + " files]";
}

class DiskUsageTreeModel final : public ckv::widgets::TreeModel
{
public:
    DiskUsageTreeModel(ck::du::DirectoryNode &root,
                       const std::unordered_map<const ck::du::DirectoryNode *, std::uint64_t> &node_ids,
                       const std::unordered_map<std::uint64_t, ck::du::DirectoryNode *> &nodes_by_id,
                       const std::unordered_map<const ck::du::DirectoryNode *, std::size_t> &sibling_indexes)
        : root_(root), node_ids_(node_ids), nodes_by_id_(nodes_by_id), sibling_indexes_(sibling_indexes)
    {
    }

    std::size_t root_count() const override { return 1; }

    ckv::widgets::TreeItemId root_id_at(std::size_t index) const override
    {
        return index == 0 ? id_for(&root_) : ckv::widgets::kInvalidTreeItemId;
    }

    std::optional<std::size_t> root_index_of(ckv::widgets::TreeItemId id) const override
    {
        return id == id_for(&root_) ? std::optional<std::size_t>(0) : std::nullopt;
    }

    std::optional<ckv::widgets::TreeItemId> parent_id_of(ckv::widgets::TreeItemId id) const override
    {
        const ck::du::DirectoryNode *node = node_for(id);
        if (node == nullptr || node == &root_)
            return std::nullopt;
        return id_for(node->parent);
    }

    std::size_t child_count(ckv::widgets::TreeItemId parent) const override
    {
        const ck::du::DirectoryNode *node = node_for(parent);
        return node == nullptr ? 0 : node->children.size();
    }

    ckv::widgets::TreeItemId child_id_at(ckv::widgets::TreeItemId parent, std::size_t index) const override
    {
        const ck::du::DirectoryNode *node = node_for(parent);
        if (node == nullptr || index >= node->children.size())
            return ckv::widgets::kInvalidTreeItemId;
        return id_for(node->children[index].get());
    }

    std::optional<std::size_t> child_index_of(ckv::widgets::TreeItemId parent,
                                               ckv::widgets::TreeItemId child) const override
    {
        const ck::du::DirectoryNode *parent_node = node_for(parent);
        const ck::du::DirectoryNode *child_node = node_for(child);
        if (parent_node == nullptr || child_node == nullptr || child_node->parent != parent_node)
            return std::nullopt;
        const auto found = sibling_indexes_.find(child_node);
        return found == sibling_indexes_.end() ? std::nullopt : std::optional<std::size_t>(found->second);
    }

    std::optional<ckv::widgets::TreeItem> item(ckv::widgets::TreeItemId id) const override
    {
        ck::du::DirectoryNode *node = node_for(id);
        if (node == nullptr)
            return std::nullopt;
        return ckv::widgets::TreeItem{format_directory_label(*node), true, node};
    }

private:
    ckv::widgets::TreeItemId id_for(const ck::du::DirectoryNode *node) const
    {
        const auto found = node_ids_.find(node);
        return found == node_ids_.end() ? ckv::widgets::kInvalidTreeItemId : found->second;
    }

    ck::du::DirectoryNode *node_for(ckv::widgets::TreeItemId id) const
    {
        const auto found = nodes_by_id_.find(id);
        return found == nodes_by_id_.end() ? nullptr : found->second;
    }

    ck::du::DirectoryNode &root_;
    const std::unordered_map<const ck::du::DirectoryNode *, std::uint64_t> &node_ids_;
    const std::unordered_map<std::uint64_t, ck::du::DirectoryNode *> &nodes_by_id_;
    const std::unordered_map<const ck::du::DirectoryNode *, std::size_t> &sibling_indexes_;
};

} // namespace

DiskUsageApp::DiskUsageApp(ckv::ui::Application &application, ck::du::BuildDirectoryTreeResult snapshot)
    : application_(application), snapshot_(std::move(snapshot)), command_scope_(application.commands())
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
      scan_options_(std::move(options)),
      command_scope_(application.commands())
{
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    create_window("Disk usage: scanning");
    start_scan();
}

DiskUsageApp::DiskUsageApp(ckv::ui::Application &application,
                           DiskUsageScanService &scan_service,
                           DiskUsageFileListService &file_list_service,
                           DiskUsageCloudService &cloud_service,
                           std::filesystem::path root,
                           ck::du::BuildDirectoryTreeOptions options)
    : application_(application),
      scan_service_(&scan_service),
      file_list_service_(&file_list_service),
      cloud_service_(&cloud_service),
      scan_root_(std::move(root)),
      scan_options_(std::move(options)),
      command_scope_(application.commands())
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
    if (cloud_service_ != nullptr)
        cloud_service_->cancel();
}

void DiskUsageApp::declare_commands()
{
    rescan_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-du", "ck.du.rescan", [this] { start_scan(); }));
    cancel_scan_command_ = command_scope_.own(
        declare_suite_command(application_.commands(), "ck-du", "ck.du.cancel_scan", [this] { cancel_scan(); }));
    view_files_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-du", "ck.du.view_files", [this] { view_selected_files(); }));
    download_cloud_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-du", "ck.du.cloud.download",
        [this] { request_cloud_action(DiskUsageCloudAction::Download); }));
    evict_cloud_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-du", "ck.du.cloud.evict",
        [this] { request_cloud_action(DiskUsageCloudAction::EvictLocalCopies); }));
    cancel_cloud_command_ = command_scope_.own(declare_suite_command(
        application_.commands(), "ck-du", "ck.du.cloud.cancel", [this] { cancel_scan(); }));
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
        }}, ckv::widgets::MenuBarItem{"&Cloud", {
            ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{download_cloud_command_, "&Download selected"}),
            ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{evict_cloud_command_, "&Free local copies"}),
            ckv::widgets::MenuItem::command(ckv::widgets::CommandPresentation{cancel_cloud_command_, "Cancel &cloud operation"}),
        }}},
        .application_status_items = {
            ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{rescan_command_, "&Rescan"}, 30},
            ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{cancel_scan_command_, "&Cancel"}, 30},
            ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{view_files_command_, "&Files"}, 25},
            ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{download_cloud_command_, "&Download"}, 30},
            ckv::widgets::StatusLineItem{ckv::widgets::CommandPresentation{evict_cloud_command_, "&Free local"}, 30},
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
    tree_model_.reset();
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
    // Services may report after this presentation is gone. The outer callback
    // must not dereference `this`; the posted UI work checks the same token.
    const std::weak_ptr<void> lifetime = lifetime_;
    auto *const application = &application_;
    auto *const self = this;
    scan_service_->start(scan_root_, scan_options_,
                         [application, lifetime, self](const std::filesystem::path &path) {
                             if (lifetime.expired())
                                 return;
                             application->post([self, lifetime, path] {
                                 if (lifetime.expired() || self->window_ == nullptr)
                                     return;
                                 self->window_->set_footer("Scanning " + path.string());
                             });
                         },
                         [application, lifetime, self](
                             ck::du::BuildDirectoryTreeResult result) mutable {
                             if (lifetime.expired())
                                 return;
                             auto delivered =
                                 std::make_shared<ck::du::BuildDirectoryTreeResult>(std::move(result));
                             application->post([self, lifetime, delivered] {
                                 if (lifetime.expired())
                                     return;
                                 self->complete_scan(std::move(*delivered));
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
    if (cloud_service_ != nullptr && cloud_service_->running())
    {
        cloud_service_->cancel();
        if (window_ != nullptr)
            window_->set_footer("Cancellation requested for the cloud operation.");
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
    auto *const application = &application_;
    auto *const self = this;
    file_list_service_->start(directory, false, scan_options_,
                              [application, lifetime, self, directory](
                                  DiskUsageFileListResult result) mutable {
                                  if (lifetime.expired())
                                      return;
                                  auto delivered =
                                      std::make_shared<DiskUsageFileListResult>(std::move(result));
                                  application->post([self, lifetime, directory, delivered] {
                                      if (lifetime.expired())
                                          return;
                                      self->complete_file_list(std::move(*delivered), directory);
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

void DiskUsageApp::request_cloud_action(DiskUsageCloudAction action)
{
    const ck::du::DirectoryNode *selected = selected_directory();
    if (selected == nullptr)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Cloud storage", "Select a directory before starting a cloud operation.");
        return;
    }
    if (cloud_service_ == nullptr)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Cloud storage",
                     "No cloud storage service was provided for this disk-usage view.");
        return;
    }
    if (cloud_service_->running())
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Cloud storage",
                     "A cloud operation is already running. Cancel it before starting another.");
        return;
    }

    const std::filesystem::path target = selected->path;
    const DiskUsageCloudCapability capability = cloud_service_->capability(action, target);
    if (!capability.available)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Cloud storage",
                     capability.reason.empty() ? "The selected directory does not support this cloud operation." : capability.reason);
        return;
    }

    if (action == DiskUsageCloudAction::Download)
    {
        start_cloud_action(action, target);
        return;
    }

    cloud_confirmation_.reset();
    cloud_confirmation_.emplace(ckv::widgets::present_message_box(
        application_, shell_->desktop(), shell_->roles(),
        {ckv::widgets::MessageBoxKind::Warning,
         "Free local copies",
         "Remove local copies of cloud content at '" + target.string() +
             "'? The cloud provider keeps the remote copy, but unavailable local files may need downloading before use.",
         ckv::widgets::MessageBoxButtons::YesNo}));
    cloud_confirmation_->set_completion_handler([this, target](ckv::widgets::MessageBoxResult result) {
        if (result == ckv::widgets::MessageBoxResult::Yes)
            start_cloud_action(DiskUsageCloudAction::EvictLocalCopies, target);
    });
}

void DiskUsageApp::start_cloud_action(DiskUsageCloudAction action, std::filesystem::path target)
{
    if (cloud_service_ == nullptr)
        return;

    if (window_ != nullptr)
    {
        const std::string verb = action == DiskUsageCloudAction::Download ? "Requesting download for " : "Freeing local copies at ";
        window_->set_footer(verb + target.string());
    }
    const std::weak_ptr<void> lifetime = lifetime_;
    auto *const application = &application_;
    auto *const self = this;
    cloud_service_->start(
        action, target,
        [application, lifetime, self](DiskUsageCloudProgress progress) mutable {
            if (lifetime.expired())
                return;
            application->post([self, lifetime, progress = std::move(progress)] {
                if (lifetime.expired() || self->window_ == nullptr)
                    return;
                self->window_->set_footer(progress.message);
            });
        },
        [application, lifetime, self, target](DiskUsageCloudOperationResult result) mutable {
            if (lifetime.expired())
                return;
            auto delivered = std::make_shared<DiskUsageCloudOperationResult>(std::move(result));
            application->post([self, lifetime, target, delivered] {
                if (lifetime.expired())
                    return;
                self->complete_cloud_action(std::move(*delivered), target);
            });
        });
}

void DiskUsageApp::complete_cloud_action(DiskUsageCloudOperationResult result, std::filesystem::path target)
{
    if (result.cancelled)
    {
        if (window_ != nullptr)
            window_->set_footer("Cloud operation cancelled for " + target.string());
        return;
    }

    if (!result.success)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "Cloud storage",
                     result.message.empty() ? "The cloud operation failed. You can retry after resolving the reported issue."
                                            : result.message);
        return;
    }

    if (window_ != nullptr)
        window_->set_footer("Cloud operation completed for " + target.string() + ". Rescan to refresh usage data.");
    show_message(ckv::widgets::MessageBoxKind::Info, "Cloud storage",
                 result.message.empty() ? "The cloud operation completed. Rescan to refresh usage data." : result.message);
}

void DiskUsageApp::show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message)
{
    message_box_.reset();
    message_box_.emplace(ckv::widgets::present_message_box(
        application_, shell_->desktop(), shell_->roles(), {kind, std::move(title), std::move(message), ckv::widgets::MessageBoxButtons::Ok}));
    message_box_->set_completion_handler([](ckv::widgets::MessageBoxResult) {});
}

void DiskUsageApp::rebuild_snapshot_view()
{
    if (window_ == nullptr || snapshot_.root == nullptr)
        return;

    node_ids_.clear();
    nodes_by_id_.clear();
    sibling_indexes_.clear();
    next_node_id_ = 1;
    index_tree_nodes(*snapshot_.root, 0);
    auto model = std::make_unique<DiskUsageTreeModel>(*snapshot_.root, node_ids_, nodes_by_id_, sibling_indexes_);
    auto tree = std::make_unique<ckv::widgets::TreeView>();
    tree_ = tree.get();
    tree_->set_connector_style(ckv::widgets::TreeConnectorStyle::Outline);
    tree_->on_selection_changed = [this](ckv::widgets::TreeNode &selected) {
        ck::du::DirectoryNode *const *node = std::any_cast<ck::du::DirectoryNode *>(&selected.user_data);
        if (node != nullptr && *node != nullptr)
            show_directory(**node);
    };
    tree_->set_model(*model);
    std::function<void(const ck::du::DirectoryNode &)> apply_expansion;
    apply_expansion = [this, &apply_expansion](const ck::du::DirectoryNode &node) {
        if (node.parent == nullptr || node.expanded)
            tree_->set_item_expanded(node_ids_.at(&node), true);
        for (const auto &child : node.children)
            apply_expansion(*child);
    };
    apply_expansion(*snapshot_.root);

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
    tree_model_ = std::move(model);
    application_.set_focus(tree_);
}

bool DiskUsageApp::scan_running() const noexcept
{
    return scan_service_ != nullptr && scan_service_->running();
}

bool DiskUsageApp::cloud_operation_running() const noexcept
{
    return cloud_service_ != nullptr && cloud_service_->running();
}

void DiskUsageApp::index_tree_nodes(ck::du::DirectoryNode &node, std::size_t sibling_index)
{
    const std::uint64_t id = next_node_id_++;
    node_ids_.emplace(&node, id);
    nodes_by_id_.emplace(id, &node);
    sibling_indexes_.emplace(&node, sibling_index);
    for (std::size_t index = 0; index < node.children.size(); ++index)
        index_tree_nodes(*node.children[index], index);
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
    return format_directory_label(node);
}

const ck::du::DirectoryNode *DiskUsageApp::selected_directory() const noexcept
{
    if (tree_ == nullptr || tree_->selected() == nullptr)
        return nullptr;
    ck::du::DirectoryNode *const *node = std::any_cast<ck::du::DirectoryNode *>(&tree_->selected()->user_data);
    return node == nullptr ? nullptr : *node;
}

} // namespace ck::vision
