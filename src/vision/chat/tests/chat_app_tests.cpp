#include "chat_app.hpp"
#include "chat_startup_options.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

#include "chat_options.hpp"

namespace
{
void require(bool value, const char *message)
{
    if (value)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

std::string block_text(const ckv::widgets::FlowBlock &block)
{
    std::string text;
    for (const auto &inline_content : block.content)
        if (const auto *flow_text = std::get_if<ckv::widgets::FlowText>(&inline_content))
            text += flow_text->text;
    return text;
}

class ManualResponseService final : public ck::vision::ChatResponseService
{
public:
    void start(ck::vision::ChatResponseRequest request, ChunkHandler on_chunk, CompletionHandler on_complete) override
    {
        running_ = true;
        cancelled_ = false;
        request_ = std::move(request);
        on_chunk_ = std::move(on_chunk);
        on_complete_ = std::move(on_complete);
    }

    void cancel() noexcept override { cancelled_ = true; }
    bool running() const noexcept override { return running_; }

    void emit(std::string chunk) { on_chunk_(std::move(chunk)); }
    void complete(bool cancelled = false)
    {
        running_ = false;
        on_complete_(cancelled);
    }

    const ck::vision::ChatResponseRequest &request() const noexcept { return request_; }
    bool cancelled() const noexcept { return cancelled_; }

private:
    bool running_ = false;
    bool cancelled_ = false;
    ck::vision::ChatResponseRequest request_;
    ChunkHandler on_chunk_;
    CompletionHandler on_complete_;
};

class MemoryTranscriptStore final : public ck::vision::ChatTranscriptStore
{
public:
    bool write(const std::filesystem::path &path, const std::string &transcript) override
    {
        path_ = path;
        transcript_ = transcript;
        return true;
    }

    const std::filesystem::path &path() const noexcept { return path_; }
    const std::string &transcript() const noexcept { return transcript_; }

private:
    std::filesystem::path path_;
    std::string transcript_;
};

class MemoryPromptService final : public ck::vision::ChatPromptService
{
public:
    MemoryPromptService()
        : prompts_{{"default", "Helpful", "Keep responses clear.", true, true},
                   {"review", "Code review", "Review code for correctness.", false, false}}
    {
    }

    std::vector<ck::vision::ChatSystemPrompt> prompts() const override { return prompts_; }

    std::optional<ck::vision::ChatSystemPrompt> active_prompt() const override
    {
        for (const auto &prompt : prompts_)
            if (prompt.is_active)
                return prompt;
        return std::nullopt;
    }

    bool add_or_update(ck::vision::ChatSystemPrompt prompt) override
    {
        if (prompt.name.empty() || prompt.message.empty())
            return false;
        if (prompt.id.empty())
            prompt.id = "custom-" + std::to_string(prompts_.size());
        for (auto &existing : prompts_)
            if (existing.id == prompt.id)
            {
                const bool active = existing.is_active;
                existing = std::move(prompt);
                existing.is_active = active;
                return true;
            }
        prompts_.push_back(std::move(prompt));
        return true;
    }

    bool remove(std::string_view id) override
    {
        const auto found = std::find_if(prompts_.begin(), prompts_.end(), [id](const auto &prompt) {
            return prompt.id == id && !prompt.is_default;
        });
        if (found == prompts_.end())
            return false;
        const bool active = found->is_active;
        prompts_.erase(found);
        if (active && !prompts_.empty())
        {
            for (auto &prompt : prompts_)
                prompt.is_active = false;
            prompts_.front().is_active = true;
        }
        return true;
    }

    bool activate(std::string_view id) override
    {
        const auto found = std::find_if(prompts_.begin(), prompts_.end(), [id](const auto &prompt) {
            return prompt.id == id;
        });
        if (found == prompts_.end())
            return false;
        for (auto &prompt : prompts_)
            prompt.is_active = prompt.id == id;
        return true;
    }

    bool restore_default(std::string_view id) override
    {
        for (auto &prompt : prompts_)
            if (prompt.id == id && prompt.is_default)
            {
                prompt.name = "Helpful";
                prompt.message = "Keep responses clear.";
                return true;
            }
        return false;
    }

    bool is_default_modified(std::string_view id) const override
    {
        for (const auto &prompt : prompts_)
            if (prompt.id == id && prompt.is_default)
                return prompt.name != "Helpful" || prompt.message != "Keep responses clear.";
        return false;
    }

private:
    std::vector<ck::vision::ChatSystemPrompt> prompts_;
};

class MemoryModelService final : public ck::vision::ChatModelService
{
public:
    MemoryModelService()
        : models_{{.id = "local",
                   .name = "Local assistant",
                   .description = "A locally available assistant.",
                   .hardware_requirements = "CPU",
                   .size_bytes = 512,
                   .local_path = "model",
                   .is_downloaded = true,
                   .is_active = true},
                  {.id = "writer",
                   .name = "Technical writer",
                   .description = "A local writing model.",
                   .hardware_requirements = "CPU",
                   .size_bytes = 768,
                   .local_path = "writer-model",
                   .is_downloaded = true,
                   .is_active = false},
                  {.id = "download",
                   .name = "Downloadable assistant",
                   .description = "A model ready to download.",
                   .hardware_requirements = "CPU",
                   .size_bytes = 1024,
                   .is_downloaded = false,
                   .is_active = false}}
    {
    }

    std::vector<ck::vision::ChatModel> available_models() const override { return models_; }

    std::vector<ck::vision::ChatModel> downloaded_models() const override
    {
        std::vector<ck::vision::ChatModel> downloaded;
        for (const auto &model : models_)
            if (model.is_downloaded)
                downloaded.push_back(model);
        return downloaded;
    }

    std::optional<ck::vision::ChatModel> active_model() const override
    {
        for (const auto &model : models_)
            if (model.is_active)
                return model;
        return std::nullopt;
    }

    bool activate(std::string_view id) override
    {
        const auto found = std::find_if(models_.begin(), models_.end(), [id](const auto &model) {
            return model.id == id && model.is_downloaded;
        });
        if (found == models_.end())
            return false;
        for (auto &model : models_)
            model.is_active = model.id == id;
        return true;
    }

    bool deactivate(std::string_view id) override
    {
        for (auto &model : models_)
            if (model.id == id && model.is_downloaded && model.is_active)
            {
                model.is_active = false;
                return true;
            }
        return false;
    }

    bool remove(std::string_view id) override
    {
        const auto found = std::find_if(models_.begin(), models_.end(), [id](const auto &model) {
            return model.id == id && model.is_downloaded;
        });
        if (found == models_.end())
            return false;
        models_.erase(found);
        return true;
    }

    bool start_download(std::string_view id, DownloadProgressHandler on_progress,
                        DownloadCompletionHandler on_complete) override
    {
        if (running_)
            return false;
        const auto found = std::find_if(models_.begin(), models_.end(), [id](const auto &model) {
            return model.id == id && !model.is_downloaded;
        });
        if (found == models_.end())
            return false;
        running_ = true;
        cancelled_ = false;
        downloading_id_ = std::string(id);
        on_progress_ = std::move(on_progress);
        on_complete_ = std::move(on_complete);
        return true;
    }

    void cancel_download() noexcept override { cancelled_ = true; }
    bool download_running() const noexcept override { return running_; }

    void emit_download_progress(std::size_t downloaded, std::size_t total)
    {
        on_progress_({.model_id = downloading_id_,
                      .bytes_downloaded = downloaded,
                      .total_bytes = total,
                      .progress_percentage = total == 0 ? 0.0 : (100.0 * downloaded) / total});
    }

    void complete_download(bool success, bool cancelled = false)
    {
        running_ = false;
        cancelled_ = cancelled_ || cancelled;
        if (success)
            for (auto &model : models_)
                if (model.id == downloading_id_)
                    model.is_downloaded = true;
        on_complete_({.model_id = downloading_id_, .success = success, .cancelled = cancelled_});
    }

    bool download_cancelled() const noexcept { return cancelled_; }

private:
    std::vector<ck::vision::ChatModel> models_;
    bool running_ = false;
    bool cancelled_ = false;
    std::string downloading_id_;
    DownloadProgressHandler on_progress_;
    DownloadCompletionHandler on_complete_;
};

void verify_late_chat_delivery_is_lifetime_safe()
{
    {
        ckv::ManualClock clock;
        ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
        ckv::ui::Application application(terminal, clock);
        ManualResponseService responses;
        MemoryTranscriptStore transcripts;
        MemoryPromptService prompts;
        MemoryModelService models;
        {
            ck::vision::ChatApp chat(application, responses, transcripts, prompts, models);
            require(chat.submit_prompt("Late response"),
                    "The test requires an active native chat response.");
        }
        require(responses.cancelled(), "Destroying chat must request response cancellation.");
        responses.emit("Late response chunk.");
        responses.complete();
        application.step(0);
        require(application.current_frame().size() == ckv::Size{100, 30},
                "Late chat response delivery after destruction must be safely ignored.");
    }

    {
        ckv::ManualClock clock;
        ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
        ckv::ui::Application application(terminal, clock);
        ManualResponseService responses;
        MemoryTranscriptStore transcripts;
        MemoryPromptService prompts;
        MemoryModelService models;
        {
            ck::vision::ChatApp chat(application, responses, transcripts, prompts, models);
            require(chat.start_model_download("download") && chat.model_download_running(),
                    "The test requires an active native model download.");
        }
        require(models.download_cancelled(), "Destroying chat must request model-download cancellation.");
        models.emit_download_progress(1024, 1024);
        models.complete_download(true);
        application.step(0);
        require(application.current_frame().size() == ckv::Size{100, 30},
                "Late model-download delivery after destruction must be safely ignored.");
    }
}
}

int main()
{
    verify_late_chat_delivery_is_lifetime_safe();

    ck::config::OptionRegistry optionsRegistry("ck-chat");
    ck::chat::registerChatOptions(optionsRegistry);
    optionsRegistry.set(ck::chat::kOptionActiveModelId, ck::config::OptionValue(std::string("writer")));
    optionsRegistry.set(ck::chat::kOptionActivePromptId, ck::config::OptionValue(std::string("review")));
    const ck::vision::ChatStartupSelection startupSelection =
        ck::vision::chatStartupSelectionFromRegistry(optionsRegistry);
    require(startupSelection.active_model_id == "writer" && startupSelection.active_prompt_id == "review",
            "The chat startup boundary must derive persistent model and prompt selections from its profile.");

    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ManualResponseService responses;
    MemoryTranscriptStore transcripts;
    MemoryPromptService prompts;
    MemoryModelService models;
    ck::vision::applyChatStartupSelection(startupSelection, prompts, models);
    require(prompts.active_prompt() && prompts.active_prompt()->id == "review" &&
                models.active_model() && models.active_model()->id == "writer",
            "The chat startup boundary must apply valid profile selections through the owned services.");
    require(prompts.activate("default") && models.activate("local"),
            "The profile-selection test must restore the fixture's initial chat state.");
    ck::vision::applyChatStartupSelection({"missing-model", "missing-prompt"}, prompts, models);
    require(prompts.active_prompt() && prompts.active_prompt()->id == "default" &&
                models.active_model() && models.active_model()->id == "local",
            "Unavailable profile selections must leave the service-owned chat state unchanged.");
    ck::vision::ChatApp chat(application, responses, transcripts, prompts, models);
    require(chat.submit_prompt("Hello"), "The native chat app must accept a non-empty prompt.");
    require(chat.messages().size() == 2 && responses.request().prompt == "Hello" &&
                responses.request().system_prompt == "Keep responses clear." && responses.request().model_id == "local" &&
                chat.response_running(),
            "The native chat app must delegate prompts to the injected streaming service.");
    require(!chat.activate_model("writer"),
            "Model lifecycle changes must be rejected while a response is running on the active model.");
    responses.emit("**Echo** ");
    responses.emit("[Hello](https://example.com)");
    application.step(0);
    require(chat.messages()[1].content == "**Echo** [Hello](https://example.com)",
            "Streaming chunks must be marshalled into the assistant transcript.");
    responses.complete();
    application.step(0);
    require(!chat.response_running(), "Completion must clear the active response state.");
    require(chat.transcript()->link_count() == 1,
            "Completion must flush Markdown links in batched assistant output into FlowView navigation.");
    require(chat.submit_prompt("Follow up"), "A completed conversation must accept a follow-up prompt.");
    require(responses.request().history.size() == 2 &&
                responses.request().history[0].role == ck::vision::ChatResponseRequest::PriorMessage::Role::User &&
                responses.request().history[0].content == "Hello" &&
                responses.request().history[1].role == ck::vision::ChatResponseRequest::PriorMessage::Role::Assistant,
            "Follow-up requests must retain completed conversation turns at the service boundary.");
    responses.complete();
    application.step(0);
    require(application.execute_command(chat.copy_command()), "Copy must dispatch through the command registry.");
    require(application.clipboard_text().find("**Echo** [Hello](https://example.com)") != std::string::npos,
            "Copy must export the native transcript through the application clipboard.");
    require(chat.export_transcript("/exports/conversation.txt"),
            "Transcript export must delegate to the injected storage policy.");
    require(transcripts.path() == "/exports/conversation.txt" &&
                transcripts.transcript().find("Assistant: **Echo**") != std::string::npos,
            "Transcript export must preserve the selected path and native conversation content.");
    require(application.execute_command(chat.export_command()), "Export must dispatch through the command registry.");
    require(chat.activate_prompt("review"), "The chat app must activate a chosen system prompt through the service.");
    require(chat.active_prompt() && chat.active_prompt()->id == "review",
            "The selected system prompt must become the active prompt.");
    require(chat.add_or_update_prompt({"release", "Release notes", "Write concise release notes.", false, false}),
            "The chat app must persist a named custom system prompt through the service.");
    require(chat.remove_prompt("release"), "The chat app must remove a custom system prompt through the service.");
    require(!chat.remove_prompt("default"), "The chat app must leave a default system prompt intact.");
    require(application.execute_command(chat.select_prompt_command()), "Prompt selection must dispatch through the command registry.");
    require(application.execute_command(chat.add_prompt_command()), "Prompt creation must dispatch through the command registry.");
    require(application.execute_command(chat.edit_active_prompt_command()),
            "Active-prompt editing must dispatch through the command registry.");
    require(application.execute_command(chat.restore_active_prompt_command()),
            "Default-prompt restoration must dispatch through the command registry.");
    require(application.execute_command(chat.delete_active_prompt_command()),
            "Active-prompt deletion must dispatch through the command registry.");
    require(chat.activate_model("writer"), "The chat app must activate a selected downloaded model through the service.");
    require(chat.active_model() && chat.active_model()->id == "writer",
            "The selected downloaded model must become the active model.");
    require(application.execute_command(chat.select_model_command()), "Model selection must dispatch through the command registry.");
    require(application.execute_command(chat.download_model_command()), "Model download must dispatch through the command registry.");
    require(chat.start_model_download("download"), "The chat app must start a requested model download through the service.");
    require(chat.model_download_running(), "A started model download must be observable through the chat app.");
    models.emit_download_progress(256, 1024);
    application.step(0);
    chat.cancel_model_download();
    require(models.download_cancelled(), "Model download cancellation must be delegated to the model service.");
    models.complete_download(false, true);
    application.step(0);
    require(!chat.model_download_running(), "Completed model downloads must clear the active download state.");
    require(application.execute_command(chat.cancel_model_download_command()),
            "Model download cancellation must dispatch through the command registry.");
    require(application.execute_command(chat.deactivate_model_command()), "Model deactivation must dispatch through the command registry.");
    require(chat.activate_model("writer"), "A deactivated model must be activatable again.");
    require(application.execute_command(chat.delete_active_model_command()),
            "Active-model deletion must dispatch through the command registry.");
    require(chat.activate_model("local"), "The remaining local model must remain selectable.");
    require(chat.remove_model("writer"), "The chat app must remove a downloaded model through the service.");
    require(application.execute_command(chat.new_chat_command()), "New conversation must dispatch through the command registry.");
    require(chat.messages().empty(), "New conversation must clear the application-owned conversation state.");
    require(chat.submit_prompt("Retire me"), "A prompt must start before a new conversation can cancel it.");
    require(application.execute_command(chat.new_chat_command()),
            "Starting a new conversation must cancel the active response through the command registry.");
    require(responses.cancelled(), "Starting a new conversation must delegate cancellation to the response service.");
    require(!chat.submit_prompt("Too early"),
            "The chat app must wait for a cancelled worker to finish before starting another response.");
    responses.complete(true);
    application.step(0);
    require(chat.submit_prompt("Cancel me"), "A new prompt must start after completion.");
    require(responses.request().model_id == "local", "The active model identifier must travel with every response request.");
    require(application.execute_command(chat.cancel_command()), "Cancellation must dispatch through the command registry.");
    require(responses.cancelled(), "Cancellation must be delegated to the response service.");
    responses.complete(true);
    application.step(0);
    require(chat.messages().size() == 2 && chat.messages()[1].content == "[Response cancelled.]",
            "A cancelled response must retain an explicit transcript outcome.");
    require(application.execute_command(chat.send_command()), "Send must dispatch through the command registry.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "The native chat app must render headlessly.");

    require(application.execute_command(chat.new_chat_command()) && chat.submit_prompt("Batch render"),
            "A fresh chat must start a response for transcript batching coverage.");
    responses.emit("first");
    application.step(0);
    require(block_text(chat.transcript()->document().blocks.back()).find("first") != std::string::npos,
            "The first streamed response chunk must remain promptly visible.");
    responses.emit(" deferred");
    application.step(0);
    require(block_text(chat.transcript()->document().blocks.back()).find("deferred") == std::string::npos,
            "Small subsequent chunks must not rebuild the rich transcript individually.");
    responses.complete();
    application.step(0);
    require(block_text(chat.transcript()->document().blocks.back()).find("deferred") != std::string::npos,
            "Response completion must flush every deferred transcript chunk.");

    for (int index = 0; index < 81; ++index)
    {
        require(chat.submit_prompt("Long session prompt " + std::to_string(index)),
                "A completed long-session turn must start the next deterministic response.");
        responses.emit("Reply");
        application.step(0);
        responses.complete();
        application.step(0);
    }
    require(chat.messages().size() == 164 && chat.transcript()->document().blocks.size() == 161,
            "The live transcript must bound rich rendering to its newest 160 messages plus one retention notice.");
    require(chat.export_transcript("/exports/long-session.txt") &&
                transcripts.transcript().find("Batch render") != std::string::npos &&
                transcripts.transcript().find("Long session prompt 80") != std::string::npos,
            "Transcript export must retain messages outside the bounded live rendering window.");

    ck::vision::ThreadedChatResponseService threaded_responses([](const ck::vision::ChatResponseRequest &request) {
        return "Worker: " + request.system_prompt + " / " + request.prompt;
    });
    std::mutex completion_mutex;
    std::condition_variable completion_ready;
    std::string worker_response;
    bool completed = false;
    threaded_responses.start({"Hello", "System", "model"}, [&](std::string chunk) { worker_response += chunk; }, [&](bool cancelled) {
        {
            std::scoped_lock lock(completion_mutex);
            completed = !cancelled;
        }
        completion_ready.notify_one();
    });
    {
        std::unique_lock lock(completion_mutex);
        require(completion_ready.wait_for(lock, std::chrono::seconds(2), [&] { return completed; }),
                "The threaded response adapter must complete an injected responder.");
    }
    require(worker_response == "Worker: System / Hello", "The threaded response adapter must deliver response chunks.");

    ck::vision::LlmChatResponseService runtime_responses(models);
    std::mutex runtime_mutex;
    std::condition_variable runtime_ready;
    std::string runtime_response;
    bool runtime_completed = false;
    runtime_responses.start({.prompt = "Hello runtime",
                             .system_prompt = "Runtime system",
                             .model_id = "local",
                             .history = {{ck::vision::ChatResponseRequest::PriorMessage::Role::User, "Earlier turn"}}},
                            [&](std::string chunk) { runtime_response += chunk; },
                            [&](bool cancelled) {
                                {
                                    std::scoped_lock lock(runtime_mutex);
                                    runtime_completed = !cancelled;
                                }
                                runtime_ready.notify_one();
                            });
    {
        std::unique_lock lock(runtime_mutex);
        require(runtime_ready.wait_for(lock, std::chrono::seconds(2), [&] { return runtime_completed; }),
                "The native LLM response service must load the selected active model on a worker.");
    }
    require(runtime_response.find("Runtime system") != std::string::npos &&
                runtime_response.find("Earlier turn") != std::string::npos &&
                runtime_response.find("Hello runtime") != std::string::npos,
            "The native LLM response service must stream ckai_core output with the selected system prompt and prior turns.");
}
