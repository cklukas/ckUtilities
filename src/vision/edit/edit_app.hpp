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

#include "ck/vision/suite_shell.hpp"

namespace ck::vision
{

class EditApp
{
public:
    EditApp(ckv::ui::Application &application, ckv::FileSystem &files);

    bool open_file(const std::string &path);
    const ckv::widgets::EditorDocument &document() const noexcept;
    std::string path() const;
    ckv::ui::CommandId open_command() const noexcept { return open_command_; }
    ckv::ui::CommandId save_command() const noexcept { return save_command_; }
    ckv::ui::CommandId save_as_command() const noexcept { return save_as_command_; }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void create_editor_window();
    void close_editor_window();
    void open_file_dialog();
    void save();
    void show_save_as_dialog();
    void show_message(ckv::widgets::MessageBoxKind kind, std::string title, std::string message);
    static std::string status_message(ckv::widgets::EditorFileStatus status);

    ckv::ui::Application &application_;
    ckv::FileSystem &files_;
    std::shared_ptr<ckv::widgets::EditorDocument> document_;
    std::unique_ptr<SuiteShell> shell_;
    ckv::widgets::EditorWindow *window_ = nullptr;
    ckv::ui::CommandId open_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId save_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId save_as_command_ = ckv::ui::kInvalidCommand;
    std::optional<ckv::widgets::FileDialogPresentation> open_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> save_as_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> message_box_;
};

} // namespace ck::vision
