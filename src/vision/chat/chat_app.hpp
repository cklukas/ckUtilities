#pragma once

#include <cstdint>
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
#include "chat_services.hpp"

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

class ChatApp
{
public:
    ChatApp(ckv::ui::Application &application, ChatResponseService &response_service);
    ~ChatApp();

    bool submit_prompt(std::string prompt);
    const std::vector<ChatMessage> &messages() const noexcept { return messages_; }
    bool response_running() const noexcept;
    ckv::widgets::FlowView *transcript() const noexcept { return transcript_; }
    ckv::ui::CommandId new_chat_command() const noexcept { return new_chat_command_; }
    ckv::ui::CommandId send_command() const noexcept { return send_command_; }
    ckv::ui::CommandId cancel_command() const noexcept { return cancel_command_; }
    ckv::ui::CommandId copy_command() const noexcept { return copy_command_; }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void create_window();
    void show_prompt_dialog();
    void new_chat();
    void cancel_response();
    void copy_transcript();
    void append_response_chunk(std::uint64_t request, std::string chunk);
    void complete_response(std::uint64_t request, bool cancelled);
    void refresh_transcript();

    ckv::ui::Application &application_;
    ChatResponseService &response_service_;
    std::vector<ChatMessage> messages_;
    std::unique_ptr<SuiteShell> shell_;
    ckv::widgets::Window *window_ = nullptr;
    ckv::widgets::FlowView *transcript_ = nullptr;
    ckv::ui::CommandId new_chat_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId send_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId cancel_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId copy_command_ = ckv::ui::kInvalidCommand;
    std::optional<ckv::widgets::DescriptorDialogPresentation> prompt_dialog_;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::uint64_t active_request_ = 0;
    bool response_pending_ = false;
};

} // namespace ck::vision
