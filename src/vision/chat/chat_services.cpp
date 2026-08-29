#include "chat_services.hpp"

#include <fstream>
#include <utility>

#include "ck/ai/system_prompt_manager.hpp"

namespace ck::vision
{
namespace
{
ChatSystemPrompt as_chat_prompt(const ck::ai::SystemPrompt &prompt)
{
    return {.id = prompt.id,
            .name = prompt.name,
            .message = prompt.message,
            .is_default = prompt.is_default,
            .is_active = prompt.is_active};
}

ck::ai::SystemPrompt as_system_prompt(ChatSystemPrompt prompt)
{
    return {.id = std::move(prompt.id),
            .name = std::move(prompt.name),
            .message = std::move(prompt.message),
            .is_default = prompt.is_default,
            .is_active = prompt.is_active};
}
} // namespace

SystemPromptManagerService::SystemPromptManagerService(ck::ai::SystemPromptManager &manager) noexcept
    : manager_(manager)
{
}

std::vector<ChatSystemPrompt> SystemPromptManagerService::prompts() const
{
    std::vector<ChatSystemPrompt> prompts;
    for (const ck::ai::SystemPrompt &prompt : manager_.get_prompts())
        prompts.push_back(as_chat_prompt(prompt));
    return prompts;
}

std::optional<ChatSystemPrompt> SystemPromptManagerService::active_prompt() const
{
    const std::optional<ck::ai::SystemPrompt> prompt = manager_.get_active_prompt();
    return prompt ? std::optional<ChatSystemPrompt>{as_chat_prompt(*prompt)} : std::nullopt;
}

bool SystemPromptManagerService::add_or_update(ChatSystemPrompt prompt)
{
    return manager_.add_or_update_prompt(as_system_prompt(std::move(prompt)));
}

bool SystemPromptManagerService::remove(std::string_view id)
{
    return manager_.delete_prompt(std::string(id));
}

bool SystemPromptManagerService::activate(std::string_view id)
{
    return manager_.set_active_prompt(std::string(id));
}

bool SystemPromptManagerService::restore_default(std::string_view id)
{
    return manager_.restore_default_prompt(std::string(id));
}

bool SystemPromptManagerService::is_default_modified(std::string_view id) const
{
    return manager_.is_default_prompt_modified(std::string(id));
}

ThreadedChatResponseService::ThreadedChatResponseService(ChatResponder responder)
    : responder_(std::move(responder))
{
}

ThreadedChatResponseService::~ThreadedChatResponseService()
{
    cancel();
    std::scoped_lock lock(mutex_);
    if (worker_.joinable())
        worker_.join();
}

void ThreadedChatResponseService::start(ChatResponseRequest request, ChunkHandler on_chunk,
                                        CompletionHandler on_complete)
{
    cancel();

    std::jthread previous;
    {
        std::scoped_lock lock(mutex_);
        previous = std::move(worker_);
    }
    if (previous.joinable())
        previous.join();

    auto cancellation = std::make_shared<std::atomic_bool>(false);
    running_.store(true, std::memory_order_release);
    std::jthread worker([this, cancellation, request = std::move(request), on_chunk = std::move(on_chunk),
                         on_complete = std::move(on_complete)]() mutable {
        std::string response;
        if (responder_)
            response = responder_(request);
        const bool cancelled = cancellation->load(std::memory_order_acquire);
        if (!cancelled && on_chunk)
            on_chunk(std::move(response));
        running_.store(false, std::memory_order_release);
        if (on_complete)
            on_complete(cancelled);
    });

    {
        std::scoped_lock lock(mutex_);
        cancellation_ = std::move(cancellation);
        worker_ = std::move(worker);
    }
}

void ThreadedChatResponseService::cancel() noexcept
{
    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::scoped_lock lock(mutex_);
        cancellation = cancellation_;
    }
    if (cancellation)
        cancellation->store(true, std::memory_order_release);
}

bool ThreadedChatResponseService::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

bool FileChatTranscriptStore::write(const std::filesystem::path &path, const std::string &transcript)
{
    if (path.empty())
        return false;
    std::error_code error;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output << transcript;
    return static_cast<bool>(output);
}

} // namespace ck::vision
