#include "chat_app.hpp"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <utility>

#include <cvision/core/clock.hpp>
#include <cvision/term/headless_terminal.hpp>

namespace
{
void require(bool value, const char *message)
{
    if (value)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

class ManualResponseService final : public ck::vision::ChatResponseService
{
public:
    void start(ck::vision::ChatResponseRequest request, ChunkHandler on_chunk, CompletionHandler on_complete) override
    {
        running_ = true;
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
}

int main()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ManualResponseService responses;
    MemoryTranscriptStore transcripts;
    MemoryPromptService prompts;
    ck::vision::ChatApp chat(application, responses, transcripts, prompts);
    require(chat.submit_prompt("Hello"), "The native chat app must accept a non-empty prompt.");
    require(chat.messages().size() == 2 && responses.request().prompt == "Hello" &&
                responses.request().system_prompt == "Keep responses clear." && chat.response_running(),
            "The native chat app must delegate prompts to the injected streaming service.");
    responses.emit("**Echo** ");
    responses.emit("[Hello](https://example.com)");
    application.step(0);
    require(chat.messages()[1].content == "**Echo** [Hello](https://example.com)",
            "Streaming chunks must be marshalled into the assistant transcript.");
    require(chat.transcript()->link_count() == 1,
            "Markdown links in assistant output must be exposed through FlowView navigation.");
    responses.complete();
    application.step(0);
    require(!chat.response_running(), "Completion must clear the active response state.");
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
    require(application.execute_command(chat.new_chat_command()), "New conversation must dispatch through the command registry.");
    require(chat.messages().empty(), "New conversation must clear the application-owned conversation state.");
    require(chat.submit_prompt("Cancel me"), "A new prompt must start after completion.");
    require(application.execute_command(chat.cancel_command()), "Cancellation must dispatch through the command registry.");
    require(responses.cancelled(), "Cancellation must be delegated to the response service.");
    responses.complete(true);
    application.step(0);
    require(chat.messages().size() == 2 && chat.messages()[1].content == "[Response cancelled.]",
            "A cancelled response must retain an explicit transcript outcome.");
    require(application.execute_command(chat.send_command()), "Send must dispatch through the command registry.");
    application.step(0);
    require(application.current_frame().size() == ckv::Size{100, 30}, "The native chat app must render headlessly.");

    ck::vision::ThreadedChatResponseService threaded_responses([](const ck::vision::ChatResponseRequest &request) {
        return "Worker: " + request.system_prompt + " / " + request.prompt;
    });
    std::mutex completion_mutex;
    std::condition_variable completion_ready;
    std::string worker_response;
    bool completed = false;
    threaded_responses.start({"Hello", "System"}, [&](std::string chunk) { worker_response += chunk; }, [&](bool cancelled) {
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
}
