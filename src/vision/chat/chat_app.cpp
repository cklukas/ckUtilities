#include "chat_app.hpp"
#include "chat_markdown.hpp"

#include <utility>

#include <cvision/widgets/command_presentation.hpp>
#include <cvision/widgets/menu.hpp>

#include "ck/vision/keymap.hpp"

namespace ck::vision
{
namespace
{
using ckv::widgets::CommandPresentation;
using ckv::widgets::FlowBlock;
using ckv::widgets::FlowDocument;
using ckv::widgets::FlowText;
using ckv::widgets::MenuBarItem;
using ckv::widgets::MenuItem;
using ckv::widgets::StatusLineItem;

constexpr std::size_t kRenderedMessageLimit = 160;
constexpr std::size_t kResponseRefreshBatchBytes = 96;

FlowBlock message_block(const ChatMessage &message)
{
    const std::string prefix = message.role == ChatMessage::Role::User ? "You: " : "Assistant: ";
    FlowBlock block;
    block.content.emplace_back(FlowText{prefix, ckv::Attr::Bold});
    append_markdown_flow(block, message.content);
    return block;
}
}

ChatApp::ChatApp(ckv::ui::Application &application,
                 ChatResponseService &response_service,
                 ChatTranscriptStore &transcript_store,
                 ChatPromptService &prompt_service,
                 ChatModelService &model_service)
    : application_(application), response_service_(response_service), transcript_store_(transcript_store),
      prompt_service_(prompt_service), model_service_(model_service)
{
    declare_commands();
    shell_ = std::make_unique<SuiteShell>(application_, make_shell_options());
    create_window();
}

ChatApp::~ChatApp()
{
    lifetime_.reset();
    response_service_.cancel();
    model_service_.cancel_download();
}

void ChatApp::declare_commands()
{
    new_chat_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.new_chat", [this] { new_chat(); });
    send_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.send_prompt", [this] { show_prompt_dialog(); });
    cancel_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.cancel_response", [this] { cancel_response(); });
    copy_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.copy_transcript", [this] { copy_transcript(); });
    export_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.export_transcript", [this] { show_export_dialog(); });
    select_prompt_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.select_prompt",
                                                    [this] { show_select_prompt_dialog(); });
    add_prompt_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.add_prompt",
                                                 [this] { show_add_prompt_dialog(); });
    edit_active_prompt_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.edit_active_prompt",
                                                         [this] { show_edit_active_prompt_dialog(); });
    restore_active_prompt_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.restore_active_prompt",
                                                            [this] { restore_active_prompt(); });
    delete_active_prompt_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.delete_active_prompt",
                                                           [this] { request_delete_active_prompt(); });
    select_model_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.select_model",
                                                   [this] { show_select_model_dialog(); });
    download_model_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.download_model",
                                                     [this] { show_download_model_dialog(); });
    cancel_model_download_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.cancel_model_download",
                                                            [this] { cancel_model_download(); });
    deactivate_model_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.deactivate_model",
                                                       [this] { deactivate_active_model(); });
    delete_active_model_command_ = declare_suite_command(application_.commands(), "ck-chat", "ck.chat.delete_active_model",
                                                          [this] { request_delete_active_model(); });
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
                MenuItem::command(CommandPresentation{export_command_, "&Export transcript..."}),
            }},
            MenuBarItem{"&Prompts", {
                MenuItem::command(CommandPresentation{select_prompt_command_, "Select system &prompt..."}),
                MenuItem::command(CommandPresentation{add_prompt_command_, "&Add system prompt..."}),
                MenuItem::command(CommandPresentation{edit_active_prompt_command_, "&Edit active prompt..."}),
                MenuItem::command(CommandPresentation{restore_active_prompt_command_, "&Restore active default prompt"}),
                MenuItem::command(CommandPresentation{delete_active_prompt_command_, "&Delete active prompt"}),
            }},
            MenuBarItem{"&Models", {
                MenuItem::command(CommandPresentation{select_model_command_, "Select active &model..."}),
                MenuItem::command(CommandPresentation{download_model_command_, "&Download model..."}),
                MenuItem::command(CommandPresentation{cancel_model_download_command_, "Cancel model &download"}),
                MenuItem::command(CommandPresentation{deactivate_model_command_, "&Deactivate active model"}),
                MenuItem::command(CommandPresentation{delete_active_model_command_, "&Delete active model"}),
            }}},
            .application_status_items = {
                StatusLineItem{CommandPresentation{send_command_, "&Send"}, 20},
                StatusLineItem{CommandPresentation{cancel_command_, "&Cancel"}, 20},
                StatusLineItem{CommandPresentation{copy_command_, "&Copy"}, 20},
                StatusLineItem{CommandPresentation{export_command_, "&Export"}, 20},
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
    if (prompt.empty() || response_pending_ || response_service_.running())
    {
        if (response_service_.running() && window_ != nullptr)
            window_->set_footer("Wait for the active response to finish cancelling before sending another prompt.");
        return false;
    }
    const std::optional<ChatSystemPrompt> active_prompt = prompt_service_.active_prompt();
    const std::optional<ChatModel> active_model = model_service_.active_model();
    if (!active_model)
    {
        if (window_ != nullptr)
            window_->set_footer("Activate a downloaded local model before sending a prompt.");
        return false;
    }
    messages_.push_back({ChatMessage::Role::User, std::move(prompt)});
    messages_.push_back({ChatMessage::Role::Assistant, {}});
    std::vector<ChatResponseRequest::PriorMessage> history;
    history.reserve(messages_.size() - 2);
    for (std::size_t index = 0; index + 2 < messages_.size(); ++index)
    {
        const ChatMessage &message = messages_[index];
        history.push_back({message.role == ChatMessage::Role::User
                               ? ChatResponseRequest::PriorMessage::Role::User
                               : ChatResponseRequest::PriorMessage::Role::Assistant,
                           message.content});
    }
    response_pending_ = true;
    unrendered_response_bytes_ = 0;
    render_first_response_chunk_ = true;
    const std::uint64_t request = ++active_request_;
    const std::weak_ptr<void> lifetime = lifetime_;
    response_service_.start({.prompt = messages_[messages_.size() - 2].content,
                             .system_prompt = active_prompt ? active_prompt->message : std::string{},
                             .model_id = active_model ? active_model->id : std::string{},
                             .history = std::move(history)},
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
    send_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Send prompt";
    dialog.fields.push_back({"&Prompt:", "", [](const std::string &value) { return !value.empty(); }});
    dialog.buttons.push_back({"&Send", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    send_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    send_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (result.accepted && !result.values.empty())
            submit_prompt(result.values[0]);
    });
}

std::vector<ChatSystemPrompt> ChatApp::prompts() const
{
    return prompt_service_.prompts();
}

std::optional<ChatSystemPrompt> ChatApp::active_prompt() const
{
    return prompt_service_.active_prompt();
}

bool ChatApp::activate_prompt(std::string_view id)
{
    if (id.empty() || !prompt_service_.activate(id))
    {
        if (window_ != nullptr)
            window_->set_footer("Could not activate the selected system prompt.");
        return false;
    }
    refresh_transcript();
    return true;
}

bool ChatApp::add_or_update_prompt(ChatSystemPrompt prompt)
{
    if (prompt.name.empty() || prompt.message.empty() || !prompt_service_.add_or_update(std::move(prompt)))
    {
        if (window_ != nullptr)
            window_->set_footer("System prompts need a name and an instruction.");
        return false;
    }
    refresh_transcript();
    return true;
}

bool ChatApp::remove_prompt(std::string_view id)
{
    if (id.empty() || !prompt_service_.remove(id))
    {
        if (window_ != nullptr)
            window_->set_footer("Could not delete that system prompt.");
        return false;
    }
    refresh_transcript();
    return true;
}

bool ChatApp::restore_default_prompt(std::string_view id)
{
    if (id.empty() || !prompt_service_.restore_default(id))
    {
        if (window_ != nullptr)
            window_->set_footer("Could not restore that system prompt.");
        return false;
    }
    refresh_transcript();
    return true;
}

std::vector<ChatModel> ChatApp::downloaded_models() const
{
    return model_service_.downloaded_models();
}

std::vector<ChatModel> ChatApp::available_models() const
{
    return model_service_.available_models();
}

std::optional<ChatModel> ChatApp::active_model() const
{
    return model_service_.active_model();
}

bool ChatApp::activate_model(std::string_view id)
{
    if (response_running() || id.empty() || !model_service_.activate(id))
    {
        if (window_ != nullptr)
            window_->set_footer(response_running()
                                    ? "Models cannot change while a response is running."
                                    : model_service_.download_running()
                                    ? "Models cannot change while a download is in progress."
                                    : "Could not activate the selected downloaded model.");
        return false;
    }
    refresh_transcript();
    return true;
}

bool ChatApp::deactivate_model(std::string_view id)
{
    if (response_running() || id.empty() || !model_service_.deactivate(id))
    {
        if (window_ != nullptr)
            window_->set_footer(response_running()
                                    ? "Models cannot change while a response is running."
                                    : model_service_.download_running()
                                    ? "Models cannot change while a download is in progress."
                                    : "Could not deactivate the selected model.");
        return false;
    }
    refresh_transcript();
    return true;
}

bool ChatApp::remove_model(std::string_view id)
{
    if (response_running() || id.empty() || !model_service_.remove(id))
    {
        if (window_ != nullptr)
            window_->set_footer(response_running()
                                    ? "Models cannot change while a response is running."
                                    : model_service_.download_running()
                                    ? "Models cannot change while a download is in progress."
                                    : "Could not delete that local model.");
        return false;
    }
    refresh_transcript();
    return true;
}

bool ChatApp::start_model_download(std::string_view id)
{
    if (response_running() || id.empty() || model_service_.download_running())
    {
        if (window_ != nullptr)
            window_->set_footer(response_running() ? "Wait for the active response before downloading a model."
                                                   : "A model download is already in progress.");
        return false;
    }

    const std::weak_ptr<void> lifetime = lifetime_;
    if (!model_service_.start_download(
            id,
            [this, lifetime](ChatModelDownloadProgress progress) mutable {
                application_.post([this, lifetime, progress = std::move(progress)]() mutable {
                    if (lifetime.expired())
                        return;
                    update_model_download_progress(std::move(progress));
                });
            },
            [this, lifetime](ChatModelDownloadResult result) mutable {
                application_.post([this, lifetime, result = std::move(result)]() mutable {
                    if (lifetime.expired())
                        return;
                    complete_model_download(std::move(result));
                });
            }))
    {
        if (window_ != nullptr)
            window_->set_footer("Could not start the selected model download.");
        return false;
    }

    model_download_status_ = "Starting model download: " + std::string(id);
    refresh_transcript();
    return true;
}

void ChatApp::cancel_model_download()
{
    if (!model_service_.download_running())
    {
        if (window_ != nullptr)
            window_->set_footer("There is no active model download to cancel.");
        return;
    }
    model_service_.cancel_download();
    model_download_status_ = "Cancellation requested for model download.";
    refresh_transcript();
}

bool ChatApp::model_download_running() const noexcept
{
    return model_service_.download_running();
}

void ChatApp::show_select_prompt_dialog()
{
    const std::vector<ChatSystemPrompt> available = prompts();
    if (available.empty())
    {
        if (window_ != nullptr)
            window_->set_footer("No system prompts are available.");
        return;
    }

    std::vector<std::string> labels;
    std::vector<std::string> ids;
    labels.reserve(available.size());
    ids.reserve(available.size());
    int selected = 0;
    for (std::size_t index = 0; index < available.size(); ++index)
    {
        const ChatSystemPrompt &prompt = available[index];
        labels.push_back(prompt.name + (prompt.is_active ? " [active]" : ""));
        ids.push_back(prompt.id);
        if (prompt.is_active)
            selected = static_cast<int>(index);
    }

    prompt_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Select system prompt";
    dialog.fields.push_back({"&Prompt:", "", nullptr, false, '*', ckv::widgets::FieldKind::Combo,
                             false, std::move(labels), selected});
    dialog.buttons.push_back({"&Select", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    prompt_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    prompt_dialog_->set_completion_handler([this, ids = std::move(ids)](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.selected.size() != 1 || result.selected[0] < 0 ||
            static_cast<std::size_t>(result.selected[0]) >= ids.size())
            return;
        activate_prompt(ids[static_cast<std::size_t>(result.selected[0])]);
    });
}

void ChatApp::show_add_prompt_dialog()
{
    prompt_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Add system prompt";
    dialog.resizable = true;
    dialog.minimum_window_size = ckv::Size{64, 15};
    dialog.fields.push_back({"&Name:", "", [](const std::string &value) { return !value.empty(); }});
    dialog.fields.push_back({.label = "&Instruction:",
                             .initial_text = "",
                             .validate = [](const std::string &value) { return !value.empty(); },
                             .kind = ckv::widgets::FieldKind::Memo,
                             .memo_rows = 6});
    dialog.buttons.push_back({"&Save", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    prompt_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    prompt_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (result.accepted && result.values.size() == 2)
            add_or_update_prompt({.name = result.values[0], .message = result.values[1]});
    });
}

void ChatApp::show_edit_active_prompt_dialog()
{
    const std::optional<ChatSystemPrompt> current = active_prompt();
    if (!current)
    {
        if (window_ != nullptr)
            window_->set_footer("There is no active system prompt to edit.");
        return;
    }

    prompt_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Edit system prompt";
    dialog.resizable = true;
    dialog.minimum_window_size = ckv::Size{64, 15};
    dialog.fields.push_back({"&Name:", current->name, [](const std::string &value) { return !value.empty(); }});
    dialog.fields.push_back({.label = "&Instruction:",
                             .initial_text = current->message,
                             .validate = [](const std::string &value) { return !value.empty(); },
                             .kind = ckv::widgets::FieldKind::Memo,
                             .memo_rows = 6});
    dialog.buttons.push_back({"&Save", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    prompt_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    prompt_dialog_->set_completion_handler([this, current = *current](ckv::widgets::DialogResult result) mutable {
        if (!result.accepted || result.values.size() != 2)
            return;
        current.name = result.values[0];
        current.message = result.values[1];
        add_or_update_prompt(std::move(current));
    });
}

void ChatApp::restore_active_prompt()
{
    const std::optional<ChatSystemPrompt> current = active_prompt();
    if (!current || !current->is_default)
    {
        if (window_ != nullptr)
            window_->set_footer("Only an active default prompt can be restored.");
        return;
    }
    restore_default_prompt(current->id);
}

void ChatApp::request_delete_active_prompt()
{
    const std::optional<ChatSystemPrompt> current = active_prompt();
    if (!current)
    {
        if (window_ != nullptr)
            window_->set_footer("There is no active system prompt to delete.");
        return;
    }
    if (current->is_default)
    {
        if (window_ != nullptr)
            window_->set_footer("Default prompts are restored rather than deleted.");
        return;
    }

    delete_prompt_confirmation_.reset();
    delete_prompt_confirmation_.emplace(ckv::widgets::present_message_box(
        application_, shell_->desktop(), shell_->roles(),
        {ckv::widgets::MessageBoxKind::Warning,
         "Delete system prompt",
         "Delete the active system prompt '" + current->name + "'?",
         ckv::widgets::MessageBoxButtons::YesNoCancel}));
    delete_prompt_confirmation_->set_completion_handler([this, id = current->id](ckv::widgets::MessageBoxResult result) {
        if (result == ckv::widgets::MessageBoxResult::Yes)
            remove_prompt(id);
    });
}

void ChatApp::show_select_model_dialog()
{
    const std::vector<ChatModel> available = downloaded_models();
    if (available.empty())
    {
        if (window_ != nullptr)
            window_->set_footer("No downloaded models are available. Choose Download model to add one.");
        return;
    }

    std::vector<std::string> labels;
    std::vector<std::string> ids;
    labels.reserve(available.size());
    ids.reserve(available.size());
    int selected = 0;
    for (std::size_t index = 0; index < available.size(); ++index)
    {
        const ChatModel &model = available[index];
        labels.push_back(model.name + (model.is_active ? " [active]" : ""));
        ids.push_back(model.id);
        if (model.is_active)
            selected = static_cast<int>(index);
    }

    model_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Select active model";
    dialog.fields.push_back({"&Downloaded model:", "", nullptr, false, '*', ckv::widgets::FieldKind::Combo,
                             false, std::move(labels), selected});
    dialog.buttons.push_back({"&Activate", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    model_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    model_dialog_->set_completion_handler([this, ids = std::move(ids)](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.selected.size() != 1 || result.selected[0] < 0 ||
            static_cast<std::size_t>(result.selected[0]) >= ids.size())
            return;
        activate_model(ids[static_cast<std::size_t>(result.selected[0])]);
    });
}

void ChatApp::show_download_model_dialog()
{
    if (model_service_.download_running())
    {
        if (window_ != nullptr)
            window_->set_footer("A model download is already in progress.");
        return;
    }

    const std::vector<ChatModel> all_models = available_models();
    std::vector<std::string> labels;
    std::vector<std::string> ids;
    for (const ChatModel &model : all_models)
    {
        if (model.is_downloaded)
            continue;
        labels.push_back(model.name + " (" + model.hardware_requirements + ")");
        ids.push_back(model.id);
    }
    if (ids.empty())
    {
        if (window_ != nullptr)
            window_->set_footer("All known models are already downloaded.");
        return;
    }

    model_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Download model";
    dialog.fields.push_back({"&Model:", "", nullptr, false, '*', ckv::widgets::FieldKind::Combo,
                             false, std::move(labels), 0});
    dialog.buttons.push_back({"&Download", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    model_dialog_.emplace(
        ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    model_dialog_->set_completion_handler([this, ids = std::move(ids)](ckv::widgets::DialogResult result) {
        if (!result.accepted || result.selected.size() != 1 || result.selected[0] < 0 ||
            static_cast<std::size_t>(result.selected[0]) >= ids.size())
            return;
        start_model_download(ids[static_cast<std::size_t>(result.selected[0])]);
    });
}

void ChatApp::deactivate_active_model()
{
    const std::optional<ChatModel> current = active_model();
    if (!current)
    {
        if (window_ != nullptr)
            window_->set_footer("There is no active model to deactivate.");
        return;
    }
    deactivate_model(current->id);
}

void ChatApp::request_delete_active_model()
{
    const std::optional<ChatModel> current = active_model();
    if (!current)
    {
        if (window_ != nullptr)
            window_->set_footer("There is no active local model to delete.");
        return;
    }

    delete_model_confirmation_.reset();
    delete_model_confirmation_.emplace(ckv::widgets::present_message_box(
        application_, shell_->desktop(), shell_->roles(),
        {ckv::widgets::MessageBoxKind::Warning,
         "Delete local model",
         "Delete the active local model '" + current->name + "'?",
         ckv::widgets::MessageBoxButtons::YesNoCancel}));
    delete_model_confirmation_->set_completion_handler([this, id = current->id](ckv::widgets::MessageBoxResult result) {
        if (result == ckv::widgets::MessageBoxResult::Yes)
            remove_model(id);
    });
}

void ChatApp::new_chat()
{
    response_service_.cancel();
    ++active_request_;
    response_pending_ = false;
    unrendered_response_bytes_ = 0;
    render_first_response_chunk_ = false;
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
    application_.set_clipboard_text(transcript_text());
}

std::string ChatApp::transcript_text() const
{
    std::string text;
    for (const ChatMessage &message : messages_)
    {
        text += message.role == ChatMessage::Role::User ? "You: " : "Assistant: ";
        text += message.content;
        text += "\n\n";
    }
    return text;
}

std::string ChatApp::prompt_status() const
{
    const std::optional<ChatSystemPrompt> current = active_prompt();
    if (!current)
        return "no system prompt";
    std::string status = "prompt: " + current->name;
    if (current->is_default && prompt_service_.is_default_modified(current->id))
        status += " (modified default)";
    return status;
}

std::string ChatApp::model_status() const
{
    const std::optional<ChatModel> current = active_model();
    const std::string current_status = current ? "model: " + current->name : "no active model";
    return model_download_status_.empty() ? current_status : model_download_status_ + "; " + current_status;
}

bool ChatApp::export_transcript(const std::string &path)
{
    if (path.empty() || !transcript_store_.write(path, transcript_text()))
    {
        if (window_ != nullptr)
            window_->set_footer("Could not export transcript to " + path + ".");
        return false;
    }
    if (window_ != nullptr)
        window_->set_footer("Exported transcript to " + path + ".");
    return true;
}

void ChatApp::show_export_dialog()
{
    export_dialog_.reset();
    ckv::widgets::DialogDescriptor dialog;
    dialog.title = "Export transcript";
    dialog.fields.push_back({"&Path:", "conversation.txt", [](const std::string &value) { return !value.empty(); }});
    dialog.buttons.push_back({"&Export", ckv::widgets::ButtonRole::Accept, nullptr});
    dialog.buttons.push_back({"&Cancel", ckv::widgets::ButtonRole::Dismiss, nullptr});
    export_dialog_.emplace(ckv::widgets::present_dialog(std::move(dialog), application_, shell_->desktop(), shell_->roles()));
    export_dialog_->set_completion_handler([this](ckv::widgets::DialogResult result) {
        if (result.accepted && result.values.size() == 1)
            export_transcript(result.values.front());
    });
}

void ChatApp::append_response_chunk(std::uint64_t request, std::string chunk)
{
    if (!response_pending_ || request != active_request_ || messages_.empty())
        return;
    ChatMessage &message = messages_.back();
    if (message.role != ChatMessage::Role::Assistant)
        return;
    message.content += chunk;
    unrendered_response_bytes_ += chunk.size();
    if (render_first_response_chunk_ || unrendered_response_bytes_ >= kResponseRefreshBatchBytes)
    {
        unrendered_response_bytes_ = 0;
        render_first_response_chunk_ = false;
        refresh_active_response();
    }
}

void ChatApp::complete_response(std::uint64_t request, bool cancelled)
{
    if (!response_pending_ || request != active_request_ || messages_.empty())
        return;
    response_pending_ = false;
    unrendered_response_bytes_ = 0;
    render_first_response_chunk_ = false;
    ChatMessage &message = messages_.back();
    if (cancelled && message.role == ChatMessage::Role::Assistant && message.content.empty())
        message.content = "[Response cancelled.]";
    refresh_transcript();
    if (window_ != nullptr && cancelled)
        window_->set_footer("Response cancelled; " + std::to_string(messages_.size()) + " messages");
}

void ChatApp::update_model_download_progress(ChatModelDownloadProgress progress)
{
    model_download_status_ = "Downloading " + progress.model_id + ": " +
                             std::to_string(static_cast<int>(progress.progress_percentage)) + "%";
    refresh_transcript();
}

void ChatApp::complete_model_download(ChatModelDownloadResult result)
{
    if (result.success)
        model_download_status_ = "Downloaded model: " + result.model_id;
    else if (result.cancelled)
        model_download_status_ = "Model download cancelled: " + result.model_id;
    else
        model_download_status_ = "Model download failed: " + result.model_id +
                                 (result.error_message.empty() ? std::string{} : " (" + result.error_message + ")");
    refresh_transcript();
}

bool ChatApp::response_running() const noexcept
{
    return response_pending_ || response_service_.running();
}

void ChatApp::refresh_active_response()
{
    if (transcript_ == nullptr || messages_.empty() || messages_.back().role != ChatMessage::Role::Assistant ||
        transcript_->document().blocks.empty())
    {
        refresh_transcript();
        return;
    }
    const std::size_t active_block = transcript_->document().blocks.size() - 1;
    if (!transcript_->replace_block(active_block, message_block(messages_.back())))
        refresh_transcript();
}

void ChatApp::refresh_transcript()
{
    if (transcript_ == nullptr)
        return;
    FlowDocument document;
    const std::size_t first_rendered_message = messages_.size() > kRenderedMessageLimit
                                                   ? messages_.size() - kRenderedMessageLimit
                                                   : 0;
    document.blocks.reserve(messages_.size() - first_rendered_message + 1);
    if (messages_.empty())
        document.blocks.push_back({{FlowText{"Send a prompt to begin a new conversation."}}});
    else if (first_rendered_message != 0)
    {
        document.blocks.push_back({{FlowText{"Earlier " + std::to_string(first_rendered_message) +
                                                " messages are retained for export and model context."}}});
    }
    for (std::size_t index = first_rendered_message; index < messages_.size(); ++index)
        document.blocks.push_back(message_block(messages_[index]));
    transcript_->set_document(std::move(document));
    if (window_ != nullptr)
    {
        std::string status = std::to_string(messages_.size()) +
                             (response_pending_ ? " messages; generating; " : " messages; ");
        if (first_rendered_message != 0)
            status += "viewing latest " + std::to_string(kRenderedMessageLimit) + "; ";
        window_->set_footer(std::move(status) + prompt_status() + "; " + model_status());
    }
}

} // namespace ck::vision
