#include "chat_services.hpp"

#include <utility>

namespace ck::vision
{

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

void ThreadedChatResponseService::start(std::string prompt, ChunkHandler on_chunk, CompletionHandler on_complete)
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
    std::jthread worker([this, cancellation, prompt = std::move(prompt), on_chunk = std::move(on_chunk),
                         on_complete = std::move(on_complete)]() mutable {
        std::string response;
        if (responder_)
            response = responder_(prompt);
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

} // namespace ck::vision
