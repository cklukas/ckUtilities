#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include <cvision/core/filesystem.hpp>
#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/file_dialog.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/tree_view.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/vision/suite_shell.hpp"
#include "json_view_core.hpp"

namespace ck::vision
{

// A native ckVision presentation for the existing JSON-domain model. The
// document and Node tree are application-owned so every TreeNode payload
// remains valid for the complete lifetime of the rendered view.
class JsonViewApp
{
public:
    JsonViewApp(ckv::ui::Application &application, ckv::FileSystem &files);

    bool load_file(const std::string &path);
    bool load_document(std::string source_name, const std::string &contents);
    void close_file();

    // The non-dialog entry point also supports headless tests and external
    // command automation. It selects the first match on success.
    bool find(std::string term, bool search_keys, bool search_values);

    const Node *selected_json_node() const noexcept;
    const SearchState &search_state() const noexcept { return search_; }
    ckv::widgets::TreeView *tree() const noexcept { return tree_; }

    ckv::ui::CommandId open_command() const noexcept { return commands_.open; }
    ckv::ui::CommandId close_command() const noexcept { return commands_.close; }
    ckv::ui::CommandId copy_command() const noexcept { return commands_.copy; }
    ckv::ui::CommandId find_command() const noexcept { return commands_.find; }
    ckv::ui::CommandId find_next_command() const noexcept { return commands_.find_next; }
    ckv::ui::CommandId find_previous_command() const noexcept { return commands_.find_previous; }
    ckv::ui::CommandId end_search_command() const noexcept { return commands_.end_search; }
    ckv::ui::CommandId level_command(int level) const noexcept;

private:
    struct CommandIds
    {
        ckv::ui::CommandId open = ckv::ui::kInvalidCommand;
        ckv::ui::CommandId close = ckv::ui::kInvalidCommand;
        ckv::ui::CommandId copy = ckv::ui::kInvalidCommand;
        ckv::ui::CommandId find = ckv::ui::kInvalidCommand;
        ckv::ui::CommandId find_next = ckv::ui::kInvalidCommand;
        ckv::ui::CommandId find_previous = ckv::ui::kInvalidCommand;
        ckv::ui::CommandId end_search = ckv::ui::kInvalidCommand;
        std::array<ckv::ui::CommandId, 10> levels{};
    };

    CommandIds declare_commands();
    SuiteShellOptions make_shell_options() const;

    void open_file_dialog();
    void show_find_dialog();
    void find_next();
    void find_previous();
    void end_search();
    void copy_selection();
    void set_expansion_level(int level);

    void create_document_window();
    void rebuild_tree();
    ckv::widgets::TreeNode make_tree_node(Node &node);
    bool reveal_current_match();
    void update_footer();
    void show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message);
    std::string initial_directory() const;

    ckv::ui::Application &application_;
    ckv::FileSystem &files_;
    CommandIds commands_;
    std::unique_ptr<SuiteShell> shell_;

    std::unique_ptr<json> document_;
    std::unique_ptr<Node> root_;
    std::string document_path_;
    SearchState search_;
    std::unordered_map<const Node *, std::uint64_t> node_ids_;
    std::uint64_t next_node_id_ = 1;

    ckv::widgets::Window *window_ = nullptr;
    ckv::widgets::TreeView *tree_ = nullptr;
    std::optional<ckv::widgets::FileDialogPresentation> open_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> find_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> message_box_;
};

} // namespace ck::vision
