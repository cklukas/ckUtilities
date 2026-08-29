#pragma once

#include <memory>
#include <optional>
#include <string>

#include <cvision/core/filesystem.hpp>
#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/editor_window.hpp>
#include <cvision/widgets/file_dialog.hpp>
#include <cvision/widgets/message_box.hpp>
#include <cvision/widgets/syntax_profile.hpp>

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
    ckv::ui::CommandId open_command() const noexcept { return open_command_; }
    ckv::ui::CommandId save_command() const noexcept { return save_command_; }
    ckv::ui::CommandId save_as_command() const noexcept { return save_as_command_; }
    ckv::ui::CommandId normalise_markdown_command() const noexcept { return normalise_markdown_command_; }

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
    std::optional<ckv::widgets::FileDialogPresentation> open_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> save_as_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> message_box_;
    std::optional<ckv::widgets::MessageBoxPresentation> close_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> save_conflict_confirmation_;
    bool closing_after_explicit_choice_ = false;
};

} // namespace ck::vision
