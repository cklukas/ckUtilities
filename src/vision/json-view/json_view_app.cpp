#include "json_view_app.hpp"

#include <algorithm>
#include <any>
#include <cctype>
#include <exception>
#include <utility>
#include <vector>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>

#include "ck/vision/keymap.hpp"

namespace ck::vision
{
namespace
{

using ckv::widgets::CommandPresentation;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

class JsonTreeModel final : public ckv::widgets::TreeModel
{
public:
    JsonTreeModel(Node &root, const std::unordered_map<const Node *, std::uint64_t> &node_ids,
                  const std::unordered_map<std::uint64_t, Node *> &nodes_by_id,
                  const std::unordered_map<const Node *, std::size_t> &sibling_indexes)
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
        const Node *node = node_for(id);
        if (node == nullptr || node == &root_)
            return std::nullopt;
        return id_for(node->parent);
    }

    std::size_t child_count(ckv::widgets::TreeItemId parent) const override
    {
        const Node *node = node_for(parent);
        return node == nullptr ? 0 : node->children.size();
    }

    ckv::widgets::TreeItemId child_id_at(ckv::widgets::TreeItemId parent, std::size_t index) const override
    {
        const Node *node = node_for(parent);
        if (node == nullptr || index >= node->children.size())
            return ckv::widgets::kInvalidTreeItemId;
        return id_for(node->children[index].get());
    }

    std::optional<std::size_t> child_index_of(ckv::widgets::TreeItemId parent,
                                               ckv::widgets::TreeItemId child) const override
    {
        const Node *parent_node = node_for(parent);
        const Node *child_node = node_for(child);
        if (parent_node == nullptr || child_node == nullptr || child_node->parent != parent_node)
            return std::nullopt;
        const auto found = sibling_indexes_.find(child_node);
        return found == sibling_indexes_.end() ? std::nullopt : std::optional<std::size_t>(found->second);
    }

    std::optional<ckv::widgets::TreeItem> item(ckv::widgets::TreeItemId id) const override
    {
        Node *node = node_for(id);
        if (node == nullptr)
            return std::nullopt;
        return ckv::widgets::TreeItem{getContentLabel(node), true, node};
    }

private:
    ckv::widgets::TreeItemId id_for(const Node *node) const
    {
        const auto found = node_ids_.find(node);
        return found == node_ids_.end() ? ckv::widgets::kInvalidTreeItemId : found->second;
    }

    Node *node_for(ckv::widgets::TreeItemId id) const
    {
        const auto found = nodes_by_id_.find(id);
        return found == nodes_by_id_.end() ? nullptr : found->second;
    }

    Node &root_;
    const std::unordered_map<const Node *, std::uint64_t> &node_ids_;
    const std::unordered_map<std::uint64_t, Node *> &nodes_by_id_;
    const std::unordered_map<const Node *, std::size_t> &sibling_indexes_;
};

} // namespace

JsonViewApp::JsonViewApp(ckv::ui::Application &application, ckv::FileSystem &files)
    : application_(application), files_(files), command_scope_(application.commands())
{
    commands_ = declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
}

JsonViewApp::CommandIds JsonViewApp::declare_commands()
{
    auto declare = [this](std::string_view key, std::function<void()> handler) {
        return command_scope_.own(
            declare_suite_command(application_.commands(), "ck-json-view", key, std::move(handler)));
    };

    CommandIds ids;
    ids.open = declare("ck.json_view.open", [this] { open_file_dialog(); });
    ids.close = declare("ck.json_view.close", [this] { close_file(); });
    ids.copy = declare("ck.json_view.copy", [this] { copy_selection(); });
    ids.find = declare("ck.json_view.find", [this] { show_find_dialog(); });
    ids.find_next = declare("ck.json_view.find_next", [this] { find_next(); });
    ids.find_previous = declare("ck.json_view.find_previous", [this] { find_previous(); });
    ids.end_search = declare("ck.json_view.end_search", [this] { end_search(); });

    for (int level = 0; level <= 9; ++level)
    {
        ids.levels[static_cast<std::size_t>(level)] = declare(
            "ck.json_view.expand_level." + std::to_string(level),
            [this, level] { set_expansion_level(level); });
    }
    return ids;
}

SuiteShellOptions JsonViewApp::make_shell_options() const
{
    std::vector<MenuItem> search_items{
        MenuItem::command(CommandPresentation{commands_.find, "&Find..."}),
        MenuItem::command(CommandPresentation{commands_.find_next, "Find &Next"}),
        MenuItem::command(CommandPresentation{commands_.find_previous, "Find &Previous"}),
        MenuItem::command(CommandPresentation{commands_.end_search, "&End search"}),
    };

    std::vector<MenuItem> view_items;
    view_items.reserve(commands_.levels.size());
    for (int level = 0; level <= 9; ++level)
    {
        view_items.push_back(MenuItem::command(
            CommandPresentation{commands_.levels[static_cast<std::size_t>(level)],
                                "Level &" + std::to_string(level)}));
    }

    return {
        .application_name = "ck JSON View",
        .about_text = "A native ckVision JSON browser. Open a document, navigate its tree, search keys or values, and copy a selected JSON value.",
        .application_menus = {
            MenuBarItem{"&Edit", {MenuItem::command(CommandPresentation{commands_.copy, "&Copy selected JSON"})}},
            MenuBarItem{"&Search", std::move(search_items)},
            MenuBarItem{"&View", std::move(view_items)},
        },
        .application_status_items = {
            StatusLineItem{CommandPresentation{commands_.open, "&Open"}, 30},
            StatusLineItem{CommandPresentation{commands_.find, "&Find"}, 25},
            StatusLineItem{CommandPresentation{commands_.copy, "&Copy"}, 20},
        },
    };
}

bool JsonViewApp::load_file(const std::string &path)
{
    const auto contents = files_.read_file(path);
    if (!contents)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "Open JSON",
                     "Could not read '" + path + "'.");
        return false;
    }
    return load_document(path, contents->contents);
}

bool JsonViewApp::load_document(std::string source_name, const std::string &contents)
{
    std::unique_ptr<json> parsed;
    std::unique_ptr<Node> parsed_root;
    try
    {
        parsed = std::make_unique<json>(parseJsonWithSpecialNumbers(contents));
        parsed_root = buildTree(parsed.get(), source_name, nullptr, true);
    }
    catch (const std::exception &error)
    {
        show_message(ckv::widgets::MessageBoxKind::Error, "Open JSON",
                     "Could not parse '" + source_name + "': " + error.what());
        return false;
    }

    close_file();
    document_ = std::move(parsed);
    root_ = std::move(parsed_root);
    document_path_ = std::move(source_name);
    root_->sourceSize = contents.size();
    search_ = SearchState{};
    create_document_window();
    return true;
}

void JsonViewApp::close_file()
{
    if (window_ != nullptr)
        shell_->desktop().remove_window(window_);
    window_ = nullptr;
    tree_ = nullptr;
    tree_model_.reset();
    document_.reset();
    root_.reset();
    document_path_.clear();
    node_ids_.clear();
    nodes_by_id_.clear();
    sibling_indexes_.clear();
    search_ = SearchState{};
}

bool JsonViewApp::find(std::string term, bool search_keys, bool search_values)
{
    if (!root_)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Find", "Open a JSON document before searching.");
        return false;
    }

    term = lowercase(std::move(term));
    if (term.empty() || (!search_keys && !search_values))
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Find",
                     "Enter a search term and choose keys, values, or both.");
        return false;
    }

    SearchState next;
    next.term = std::move(term);
    next.searchKeys = search_keys;
    next.searchValues = search_values;
    searchTree(root_.get(), next.term, next.searchKeys, next.searchValues, next.matches);
    if (next.matches.empty())
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Find", "No matching JSON nodes were found.");
        return false;
    }

    search_ = std::move(next);
    return reveal_current_match();
}

void JsonViewApp::open_file_dialog()
{
    open_dialog_.reset();
    ckv::widgets::FileDialogOptions options;
    options.filters.push_back({"JSON files", {".json"}});
    open_dialog_.emplace(ckv::widgets::present_file_dialog(
        ckv::widgets::FileDialogMode::Open, initial_directory(), files_, std::move(options),
        application_, shell_->desktop(), shell_->roles()));
    open_dialog_->set_completion_handler([this](ckv::widgets::FileDialogResult result) {
        if (result.accepted)
            load_file(result.path);
    });
}

void JsonViewApp::show_find_dialog()
{
    if (!root_)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Find", "Open a JSON document before searching.");
        return;
    }

    find_dialog_.reset();
    ckv::widgets::DialogDescriptor descriptor;
    descriptor.title = "Find JSON";
    descriptor.fields.push_back({"&Term:", search_.term,
                                 [](const std::string &value) { return !value.empty(); }});
    ckv::widgets::FieldDescriptor keys;
    keys.label = "Search &keys";
    keys.kind = ckv::widgets::FieldKind::Check;
    keys.initial_checked = search_.term.empty() || search_.searchKeys;
    descriptor.fields.push_back(std::move(keys));
    ckv::widgets::FieldDescriptor values;
    values.label = "Search &values";
    values.kind = ckv::widgets::FieldKind::Check;
    values.initial_checked = search_.searchValues;
    descriptor.fields.push_back(std::move(values));
    descriptor.buttons.push_back({"&Find", ckv::widgets::ButtonRole::Accept, nullptr});
    descriptor.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});

    find_dialog_.emplace(ckv::widgets::present_dialog(std::move(descriptor), application_,
                                                        shell_->desktop(), shell_->roles()));
    find_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (result.accepted && result.values.size() >= 3 && result.checked.size() >= 3)
            find(result.values[0], result.checked[1], result.checked[2]);
    });
}

void JsonViewApp::find_next()
{
    if (search_.matches.empty())
    {
        show_find_dialog();
        return;
    }
    search_.currentIndex = (search_.currentIndex + 1) % static_cast<int>(search_.matches.size());
    reveal_current_match();
}

void JsonViewApp::find_previous()
{
    if (search_.matches.empty())
    {
        show_find_dialog();
        return;
    }
    search_.currentIndex = (search_.currentIndex - 1 + static_cast<int>(search_.matches.size())) %
                           static_cast<int>(search_.matches.size());
    reveal_current_match();
}

void JsonViewApp::end_search()
{
    search_ = SearchState{};
    update_footer();
}

void JsonViewApp::copy_selection()
{
    const Node *selected = selected_json_node();
    if (selected == nullptr)
    {
        show_message(ckv::widgets::MessageBoxKind::Info, "Copy JSON", "Select a JSON node to copy.");
        return;
    }

    application_.set_clipboard_text(reconstructJson(selected).dump(2));
    show_message(ckv::widgets::MessageBoxKind::Info, "Copy JSON", "The selected JSON value was copied.");
}

void JsonViewApp::set_expansion_level(int level)
{
    if (!root_)
        return;
    expandToLevel(root_.get(), level, 0);
    rebuild_tree();
}

void JsonViewApp::create_document_window()
{
    auto window = std::make_unique<ckv::widgets::Window>("JSON: " + document_path_);
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{40, 10});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    // Window::close explicitly permits this callback to detach and destroy the
    // current Window. Removing it here also makes the document ownership
    // transition immediate, rather than leaving a queued callback that could
    // outlive this presentation object during application teardown.
    window->on_closed = [this] { close_file(); };
    window_ = shell_->desktop().add_window(std::move(window));
    rebuild_tree();
}

void JsonViewApp::rebuild_tree()
{
    if (window_ == nullptr || !root_)
        return;

    node_ids_.clear();
    nodes_by_id_.clear();
    sibling_indexes_.clear();
    next_node_id_ = 1;
    index_tree_nodes(*root_, 0);

    auto model = std::make_unique<JsonTreeModel>(*root_, node_ids_, nodes_by_id_, sibling_indexes_);
    auto tree = std::make_unique<ckv::widgets::TreeView>();
    tree->set_connector_style(ckv::widgets::TreeConnectorStyle::Outline);
    tree_ = tree.get();
    tree_->on_selection_changed = [this](ckv::widgets::TreeNode &) { update_footer(); };
    tree_->set_model(*model);
    std::function<void(const Node &)> apply_expansion;
    apply_expansion = [this, &apply_expansion](const Node &node) {
        if (node.expanded)
            tree_->set_item_expanded(node_ids_.at(&node), true);
        for (const auto &child : node.children)
            apply_expansion(*child);
    };
    apply_expansion(*root_);
    window_->set_content(std::move(tree));
    tree_model_ = std::move(model);
    application_.set_focus(tree_);
    update_footer();
}

void JsonViewApp::index_tree_nodes(Node &node, std::size_t sibling_index)
{
    const std::uint64_t id = next_node_id_++;
    node_ids_.emplace(&node, id);
    nodes_by_id_.emplace(id, &node);
    sibling_indexes_.emplace(&node, sibling_index);
    for (std::size_t index = 0; index < node.children.size(); ++index)
        index_tree_nodes(*node.children[index], index);
}

bool JsonViewApp::reveal_current_match()
{
    if (search_.matches.empty() || search_.currentIndex < 0 ||
        search_.currentIndex >= static_cast<int>(search_.matches.size()))
        return false;

    Node *match = const_cast<Node *>(search_.matches[static_cast<std::size_t>(search_.currentIndex)]);
    expandPath(match);
    rebuild_tree();
    const auto id = node_ids_.find(match);
    if (id == node_ids_.end() || !tree_->reveal_and_select(id->second))
        return false;
    update_footer();
    return true;
}

void JsonViewApp::update_footer()
{
    if (window_ == nullptr)
        return;
    if (search_.matches.empty())
    {
        window_->set_footer(document_path_);
        return;
    }
    window_->set_footer("Find '" + search_.term + "' " +
                        std::to_string(search_.currentIndex + 1) + "/" +
                        std::to_string(search_.matches.size()));
}

void JsonViewApp::show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message)
{
    message_box_.reset();
    message_box_.emplace(ckv::widgets::present_message_box(
        application_, shell_->desktop(), shell_->roles(),
        {kind, std::move(title), std::move(message), ckv::widgets::MessageBoxButtons::Ok}));
    message_box_->set_completion_handler([](ckv::widgets::MessageBoxResult) {});
}

std::string JsonViewApp::initial_directory() const
{
    if (!document_path_.empty())
    {
        const std::string parent = files_.parent(document_path_);
        if (files_.is_directory(parent))
            return parent;
    }
    if (files_.is_directory("."))
        return ".";
    return "/";
}

const Node *JsonViewApp::selected_json_node() const noexcept
{
    if (tree_ == nullptr || tree_->selected() == nullptr)
        return nullptr;
    Node *const *node = std::any_cast<Node *>(&tree_->selected()->user_data);
    return node != nullptr ? *node : nullptr;
}

ckv::ui::CommandId JsonViewApp::level_command(int level) const noexcept
{
    if (level < 0 || level >= static_cast<int>(commands_.levels.size()))
        return ckv::ui::kInvalidCommand;
    return commands_.levels[static_cast<std::size_t>(level)];
}

} // namespace ck::vision
