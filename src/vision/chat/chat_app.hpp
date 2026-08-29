#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cvision/ui/application.hpp>
#include <cvision/ui/command.hpp>
#include <cvision/widgets/dialog.hpp>
#include <cvision/widgets/flow_view.hpp>
#include <cvision/widgets/message_box.hpp>
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
    ChatApp(ckv::ui::Application &application,
            ChatResponseService &response_service,
            ChatTranscriptStore &transcript_store,
            ChatPromptService &prompt_service,
            ChatModelService &model_service);
    ~ChatApp();

    bool submit_prompt(std::string prompt);
    bool export_transcript(const std::string &path);
    std::vector<ChatSystemPrompt> prompts() const;
    std::optional<ChatSystemPrompt> active_prompt() const;
    bool activate_prompt(std::string_view id);
    bool add_or_update_prompt(ChatSystemPrompt prompt);
    bool remove_prompt(std::string_view id);
    bool restore_default_prompt(std::string_view id);
    std::vector<ChatModel> available_models() const;
    std::vector<ChatModel> downloaded_models() const;
    std::optional<ChatModel> active_model() const;
    bool activate_model(std::string_view id);
    bool deactivate_model(std::string_view id);
    bool remove_model(std::string_view id);
    bool start_model_download(std::string_view id);
    void cancel_model_download();
    bool model_download_running() const noexcept;
    const std::vector<ChatMessage> &messages() const noexcept { return messages_; }
    bool response_running() const noexcept;
    ckv::widgets::FlowView *transcript() const noexcept { return transcript_; }
    ckv::ui::CommandId new_chat_command() const noexcept { return new_chat_command_; }
    ckv::ui::CommandId send_command() const noexcept { return send_command_; }
    ckv::ui::CommandId cancel_command() const noexcept { return cancel_command_; }
    ckv::ui::CommandId copy_command() const noexcept { return copy_command_; }
    ckv::ui::CommandId export_command() const noexcept { return export_command_; }
    ckv::ui::CommandId select_prompt_command() const noexcept { return select_prompt_command_; }
    ckv::ui::CommandId add_prompt_command() const noexcept { return add_prompt_command_; }
    ckv::ui::CommandId edit_active_prompt_command() const noexcept { return edit_active_prompt_command_; }
    ckv::ui::CommandId restore_active_prompt_command() const noexcept { return restore_active_prompt_command_; }
    ckv::ui::CommandId delete_active_prompt_command() const noexcept { return delete_active_prompt_command_; }
    ckv::ui::CommandId select_model_command() const noexcept { return select_model_command_; }
    ckv::ui::CommandId download_model_command() const noexcept { return download_model_command_; }
    ckv::ui::CommandId cancel_model_download_command() const noexcept { return cancel_model_download_command_; }
    ckv::ui::CommandId deactivate_model_command() const noexcept { return deactivate_model_command_; }
    ckv::ui::CommandId delete_active_model_command() const noexcept { return delete_active_model_command_; }

private:
    void declare_commands();
    SuiteShellOptions make_shell_options() const;
    void create_window();
    void show_prompt_dialog();
    void show_select_prompt_dialog();
    void show_add_prompt_dialog();
    void show_edit_active_prompt_dialog();
    void restore_active_prompt();
    void request_delete_active_prompt();
    void show_select_model_dialog();
    void show_download_model_dialog();
    void deactivate_active_model();
    void request_delete_active_model();
    void new_chat();
    void cancel_response();
    void copy_transcript();
    void show_export_dialog();
    std::string transcript_text() const;
    std::string prompt_status() const;
    std::string model_status() const;
    void append_response_chunk(std::uint64_t request, std::string chunk);
    void complete_response(std::uint64_t request, bool cancelled);
    void update_model_download_progress(ChatModelDownloadProgress progress);
    void complete_model_download(ChatModelDownloadResult result);
    void refresh_active_response();
    void refresh_transcript();

    ckv::ui::Application &application_;
    ChatResponseService &response_service_;
    ChatTranscriptStore &transcript_store_;
    ChatPromptService &prompt_service_;
    ChatModelService &model_service_;
    std::vector<ChatMessage> messages_;
    std::unique_ptr<SuiteShell> shell_;
    ckv::widgets::Window *window_ = nullptr;
    ckv::widgets::FlowView *transcript_ = nullptr;
    ckv::ui::CommandId new_chat_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId send_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId cancel_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId copy_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId export_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId select_prompt_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId add_prompt_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId edit_active_prompt_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId restore_active_prompt_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId delete_active_prompt_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId select_model_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId download_model_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId cancel_model_download_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId deactivate_model_command_ = ckv::ui::kInvalidCommand;
    ckv::ui::CommandId delete_active_model_command_ = ckv::ui::kInvalidCommand;
    std::optional<ckv::widgets::DescriptorDialogPresentation> send_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> prompt_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> model_dialog_;
    std::optional<ckv::widgets::DescriptorDialogPresentation> export_dialog_;
    std::optional<ckv::widgets::MessageBoxPresentation> delete_prompt_confirmation_;
    std::optional<ckv::widgets::MessageBoxPresentation> delete_model_confirmation_;
    std::shared_ptr<void> lifetime_ = std::make_shared<int>(0);
    std::uint64_t active_request_ = 0;
    std::size_t unrendered_response_bytes_ = 0;
    bool render_first_response_chunk_ = false;
    bool response_pending_ = false;
    std::string model_download_status_;
};

} // namespace ck::vision
