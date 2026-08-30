#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <cvision/core/filesystem.hpp>
#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/editor_window.hpp>
#include <cvision/widgets/file_dialog.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/syntax_profile.hpp>

#include "ck/edit/markdown_transformations.hpp"
#include "ck/vision/suite_shell.hpp"
#include "markdown_profile.hpp"

namespace ck::vision
{

class EditApp
{
public:
    EditApp(ckv::ui::Application &application, ckv::FileSystem &files);

    bool open_file(const std::string &path);
    bool request_close(ckv::widgets::EditorCloseChoice choice);
    ckv::widgets::EditorDocument &document() noexcept;
    const ckv::widgets::EditorDocument &document() const noexcept;
    std::string path() const;
    std::string syntax_profile() const;
    bool normalise_markdown();
    ckv::widgets::TextEditor *editor_view() const noexcept
    {
        return window_ == nullptr ? nullptr : &window_->editor();
    }
    ckv::ui::CommandId open_command() const noexcept { return open_command_; }
    ckv::ui::CommandId save_command() const noexcept { return save_command_; }
    ckv::ui::CommandId save_as_command() const noexcept { return save_as_command_; }
    ckv::ui::CommandId normalise_markdown_command() const noexcept { return normalise_markdown_command_; }
    ckv::ui::CommandId bold_command() const noexcept { return bold_command_; }
    ckv::ui::CommandId italic_command() const noexcept { return italic_command_; }
    ckv::ui::CommandId strikethrough_command() const noexcept { return strikethrough_command_; }
    ckv::ui::CommandId inline_code_command() const noexcept { return inline_code_command_; }
    ckv::ui::CommandId heading_command(int level) const noexcept;
    ckv::ui::CommandId toggle_task_command() const noexcept { return toggle_task_command_; }
    ckv::ui::CommandId toggle_quote_command() const noexcept { return toggle_quote_command_; }
    ckv::ui::CommandId toggle_bullet_list_command() const noexcept { return toggle_bullet_list_command_; }
    ckv::ui::CommandId toggle_ordered_list_command() const noexcept { return toggle_ordered_list_command_; }
    ckv::ui::CommandId toggle_link_command() const noexcept { return toggle_link_command_; }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void create_editor_window();
    void close_editor_window();
    void show_close_confirmation();
    void open_file_dialog();
    void save();
    void show_save_as_dialog();
    void show_save_conflict_resolution();
    void reload_after_save_conflict();
    void show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message);
    void toggle_inline_markdown(ck::edit::MarkdownInlineStyle style, std::string_view label);
    void toggle_heading_markdown(int level);
    void toggle_task_markdown();
    void toggle_quote_markdown();
    void toggle_list_markdown(ck::edit::MarkdownListStyle style, std::string_view label);
    void toggle_link_markdown();
    void show_link_destination_dialog();
    bool commit_markdown_transform(const ck::edit::MarkdownTransformEdit &transform,
                                   std::string_view success_message);
    bool markdown_document() const noexcept;
    static std::string status_message(ckv::widgets::EditorFileStatus status);

    ckv::ui::Application &application_;
    ckv::FileSystem &files_;
    std::shared_ptr<ckv::widgets::EditorDocument> document_;
    ckv::widgets::SyntaxProfileRegistry profiles_;
    std::unique_ptr<SuiteShell> shell_;
    ckv::widgets::EditorWindow *window_ = nullptr;
    ckv::ui::CommandId open_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId save_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId save_as_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId normalise_markdown_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId bold_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId italic_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId strikethrough_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId inline_code_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId toggle_task_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId toggle_quote_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId toggle_bullet_list_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId toggle_ordered_list_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId toggle_link_command_ = ckv::ui::kInvalidCommand;
    std::array<ckv::ui::CommandId, 6> heading_commands_{};
    std::optional<ckv::widgets::FileDialogPresentation> open_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> save_as_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> link_destination_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> message_box_;
    std::optional<ckv::widgets::MessageBoxPresentation> close_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> save_conflict_confirmation_;
    bool closing_after_explicit_choice_ = false;
    SuiteCommandScope command_scope_;
};

} // namespace ck::vision
