#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/flow_view.hpp>
#include <cvision/widgets/window.hpp>

#include "ck/vision/suite_shell.hpp"

namespace ck::vision
{

struct ChatMessage
{
    enum class Role
    {
        User,
        Assistant,
    };

    Role role = Role::User;
    std::string content;
};

// Application service seam. A production adapter can enqueue a model request;
// this initial native surface is intentionally agnostic about threading and
// transport lifetime.
using ChatResponder = std::function<std::string(const std::string &prompt)>;

class ChatApp
{
public:
    ChatApp(ckv::ui::Application &application, ChatResponder responder);

    bool submit_prompt(std::string prompt);
    const std::vector<ChatMessage> &messages() const noexcept { return messages_; }
    ckv::widgets::FlowView *transcript() const noexcept { return transcript_; }
    ckv::ui::CommandId new_chat_command() const noexcept { return new_chat_command_; }
    ckv::ui::CommandId send_command() const noexcept { return send_command_; }
    ckv::ui::CommandId copy_command() const noexcept { return copy_command_; }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void create_window();
    void show_prompt_dialog();
    void new_chat();
    void copy_transcript();
    void refresh_transcript();

    ckv::ui::Application &application_;
    ChatResponder responder_;
    std::vector<ChatMessage> messages_;
    std::unique_ptr<SuiteShell> shell_;
    ckv::widgets::Window *window_ = nullptr;
    ckv::widgets::FlowView *transcript_ = nullptr;
    ckv::ui::CommandId new_chat_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId send_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId copy_command_ = ckv::ui::kInvalidCommand;
    std::optional<ckv::widgets::DescriptorDialogPresentation> prompt_dialog_;
};

} // namespace ck::vision
