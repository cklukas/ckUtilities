#include "chat_app.hpp"
#include "chat_markdown.hpp"

#include <utility>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>

namespace ck::vision
{
namespace
{
using ckv::ui::CommandDescriptor;
using ckv::ui::CommandVisibility;
using ckv::widgets::CommandPresentation;
using ckv::widgets::FlowBlock;
using ckv::widgets::FlowDocument;
using ckv::widgets::FlowText;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;
}

ChatApp::ChatApp(ckv::ui::Application &application, ChatResponseService &response_service)
    : application_(application), response_service_(response_service)
{
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    create_window();
}

ChatApp::~ChatApp()
{
    lifetime_.reset();
    response_service_.cancel();
}

void ChatApp::declare_commands()
{
    new_chat_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.chat.new_chat", .title = "&New conversation", .category = "Chat", .chord = "Ctrl+N",
        .visibility = CommandVisibility::Palette, .handler = [this] { new_chat(); }});
    send_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.chat.send_prompt", .title = "&Send prompt...", .category = "Chat", .chord = "Ctrl+Enter",
        .visibility = CommandVisibility::Palette, .handler = [this] { show_prompt_dialog(); }});
    cancel_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.chat.cancel_response", .title = "&Cancel response", .category = "Chat", .chord = "Ctrl+C",
        .visibility = CommandVisibility::Palette, .handler = [this] { cancel_response(); }});
    copy_command_ = application_.commands().declare(CommandDescriptor{
        .key = "ck.chat.copy_transcript", .title = "&Copy transcript", .category = "Chat",
        .visibility = CommandVisibility::Palette, .handler = [this] { copy_transcript(); }});
}

SuiteShellOptions ChatApp::make_shell_options() const
{
    return {.application_name = "ck Chat",
            .about_text = "A native ckVision chat transcript and prompt surface with injected streaming and cancellation.",
            .application_menus = {MenuBarItem{"&Chat", {
                MenuItem::command(CommandPresentation{new_chat_command_, "&New conversation"}),
                MenuItem::command(CommandPresentation{send_command_, "&Send prompt..."}),
                MenuItem::command(CommandPresentation{cancel_command_, "&Cancel response"}),
                MenuItem::command(CommandPresentation{copy_command_, "&Copy transcript"}),
            }}},
            .application_status_items = {
                StatusLineItem{CommandPresentation{send_command_, "&Send"}, 20},
                StatusLineItem{CommandPresentation{cancel_command_, "&Cancel"}, 20},
                StatusLineItem{CommandPresentation{copy_command_, "&Copy"}, 20},
            }};
}

void ChatApp::create_window()
{
    auto window = std::make_unique<ckv::widgets::Window>("Chat");
    window->set_bounds(shell_->desktop().content_area());
    window->set_min_size(ckv::Size{50, 14});
    window->set_grow_policy(ckv::widgets::DesktopGrowPolicy::KeepFilling);
    window->on_closed = [this] { application_.request_quit(); };
    auto transcript = std::make_unique<ckv::widgets::FlowView>();
    transcript_ = transcript.get();
    window->set_content(std::move(transcript));
    window_ = shell_->desktop().add_window(std::move(window));
    refresh_transcript();
    application_.set_focus(transcript_);
}

bool ChatApp::submit_prompt(std::string prompt)
{
    if (prompt.empty() || response_pending_)
        return false;
    messages_.push_back({ChatMessage::Role::User, std::move(prompt)});
    messages_.push_back({ChatMessage::Role::Assistant, {}});
    response_pending_ = true;
    const std::uint64_t request = ++active_request_;
    const std::weak_ptr<void> lifetime = lifetime_;
    response_service_.start(messages_[messages_.size() - 2].content,
                            [this, lifetime, request](std::string chunk) mutable {
                                application_.post([this, lifetime, request, chunk = std::move(chunk)]() mutable {
                                    if (lifetime.expired())
                                        return;
                                    append_response_chunk(request, std::move(chunk));
                                });
                            },
                            [this, lifetime, request](bool cancelled) {
                                application_.post([this, lifetime, request, cancelled] {
                                    if (lifetime.expired())
                                        return;
                                    complete_response(request, cancelled);
                                });
                            });
    refresh_transcript();
    return true;
}

void ChatApp::show_prompt_dialog()
{
    prompt_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Send prompt";
    dialog.fields.push_back({"&Prompt:", "", [](const std::string &value) { return !value.empty(); }});
    dialog.buttons.push_back({"&Send", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    prompt_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    prompt_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (result.accepted && !result.values.empty())
            submit_prompt(result.values[0]);
    });
}

void ChatApp::new_chat()
{
    response_service_.cancel();
    ++active_request_;
    response_pending_ = false;
    messages_.clear();
    refresh_transcript();
}

void ChatApp::cancel_response()
{
    if (!response_pending_)
    {
        if (window_ != nullptr)
            window_->set_footer("There is no active response to cancel.");
        return;
    }
    response_service_.cancel();
    if (window_ != nullptr)
        window_->set_footer("Cancellation requested for the active response.");
}

void ChatApp::copy_transcript()
{
    std::string text;
    for (const ChatMessage &message : messages_)
    {
        text += message.role == ChatMessage::Role::User ? "You: " : "Assistant: ";
        text += message.content;
        text += "\n\n";
    }
    application_.set_clipboard_text(std::move(text));
}

void ChatApp::append_response_chunk(std::uint64_t request, std::string chunk)
{
    if (!response_pending_ || request != active_request_ || messages_.empty())
        return;
    ChatMessage &message = messages_.back();
    if (message.role != ChatMessage::Role::Assistant)
        return;
    message.content += chunk;
    refresh_transcript();
}

void ChatApp::complete_response(std::uint64_t request, bool cancelled)
{
    if (!response_pending_ || request != active_request_ || messages_.empty())
        return;
    response_pending_ = false;
    ChatMessage &message = messages_.back();
    if (cancelled && message.role == ChatMessage::Role::Assistant && message.content.empty())
        message.content = "[Response cancelled.]";
    refresh_transcript();
    if (window_ != nullptr && cancelled)
        window_->set_footer("Response cancelled; " + std::to_string(messages_.size()) + " messages");
}

bool ChatApp::response_running() const noexcept
{
    return response_pending_ || response_service_.running();
}

void ChatApp::refresh_transcript()
{
    if (transcript_ == nullptr)
        return;
    FlowDocument document;
    document.blocks.reserve(messages_.size() + 1);
    if (messages_.empty())
        document.blocks.push_back({{FlowText{"Send a prompt to begin a new conversation."}}});
    for (const ChatMessage &message : messages_)
    {
        const std::string prefix = message.role == ChatMessage::Role::User ? "You: " : "Assistant: ";
        FlowBlock block;
        block.content.emplace_back(FlowText{prefix, ckv::Attr::Bold});
        append_markdown_flow(block, message.content);
        document.blocks.push_back(std::move(block));
    }
    transcript_->set_document(std::move(document));
    if (window_ != nullptr)
        window_->set_footer(std::to_string(messages_.size()) + (response_pending_ ? " messages; generating..." : " messages"));
}

} // namespace ck::vision
