#include "chat_app.hpp"

#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
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
    void start(std::string prompt, ChunkHandler on_chunk, CompletionHandler on_complete) override
    {
        running_ = true;
        prompt_ = std::move(prompt);
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

    const std::string &prompt() const noexcept { return prompt_; }
    bool cancelled() const noexcept { return cancelled_; }

private:
    bool running_ = false;
    bool cancelled_ = false;
    std::string prompt_;
    ChunkHandler on_chunk_;
    CompletionHandler on_complete_;
};
}

int main()
{
    ckv::ManualClock clock;
    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ui::Application application(terminal, clock);
    ManualResponseService responses;
    ck::vision::ChatApp chat(application, responses);
    require(chat.submit_prompt("Hello"), "The native chat app must accept a non-empty prompt.");
    require(chat.messages().size() == 2 && responses.prompt() == "Hello" && chat.response_running(),
            "The native chat app must delegate prompts to the injected streaming service.");
    responses.emit("Echo: ");
    responses.emit("Hello");
    application.step(0);
    require(chat.messages()[1].content == "Echo: Hello",
            "Streaming chunks must be marshalled into the assistant transcript.");
    responses.complete();
    application.step(0);
    require(!chat.response_running(), "Completion must clear the active response state.");
    require(application.execute_command(chat.copy_command()), "Copy must dispatch through the command registry.");
    require(application.clipboard_text().find("Echo: Hello") != std::string::npos,
            "Copy must export the native transcript through the application clipboard.");
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

    ck::vision::ThreadedChatResponseService threaded_responses([](const std::string &prompt) {
        return "Worker: " + prompt;
    });
    std::mutex completion_mutex;
    std::condition_variable completion_ready;
    std::string worker_response;
    bool completed = false;
    threaded_responses.start("Hello", [&](std::string chunk) { worker_response += chunk; }, [&](bool cancelled) {
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
    require(worker_response == "Worker: Hello", "The threaded response adapter must deliver response chunks.");
}
