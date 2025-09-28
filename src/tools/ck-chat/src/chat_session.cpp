#include "chat_session.hpp"

#include "ck/ai/llm.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace ck::chat
{

    namespace
    {
        constexpr const char *kWelcomeMessage = "Welcome to ck-chat! Type a prompt below and press Alt+S or click Submit.";
    }

    ChatSession::ChatSession() = default;

    ChatSession::~ChatSession()
    {
        cancelActiveResponse();
    }

    void ChatSession::resetConversation()
    {
        cancelActiveResponse();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.clear();
            messages_.push_back(Message{Role::Assistant, std::string(kWelcomeMessage), false});
        }

        markDirty();
    }

    std::size_t ChatSession::addUserMessage(std::string prompt)
    {
        std::size_t index = addMessage(Message{Role::User, std::move(prompt), false});
        markDirty();
        return index;
    }

    std::size_t ChatSession::addSystemMessage(std::string text)
    {
        std::size_t index = addMessage(Message{Role::System, std::move(text), false});
        markDirty();
        return index;
    }

    void ChatSession::setSystemPrompt(std::string prompt)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        systemPrompt_ = std::move(prompt);
    }

    void ChatSession::startAssistantResponse(std::string prompt)
    {
        startAssistantResponse(std::move(prompt), nullptr);
    }

    void ChatSession::startAssistantResponse(std::string prompt,
                                             std::shared_ptr<ck::ai::Llm> llm)
    {
        cancelActiveResponse();

        auto task = std::make_unique<ResponseTask>();
        task->messageIndex = addMessage(Message{Role::Assistant, std::string(), true});
        markDirty();

        ResponseTask *rawTask = task.get();
        rawTask->llm = std::move(llm);
        rawTask->worker = std::thread([this, rawTask, prompt = std::move(prompt)]() mutable {
            if (rawTask->llm)
                runLlmResponse(*rawTask, std::move(prompt));
            else
                runSimulatedResponse(*rawTask, std::move(prompt));
        });

        activeResponse_ = std::move(task);
    }

    void ChatSession::cancelActiveResponse()
    {
        if (!activeResponse_)
            return;

        activeResponse_->cancel.store(true, std::memory_order_release);
        if (activeResponse_->worker.joinable())
            activeResponse_->worker.join();
        setMessagePending(activeResponse_->messageIndex, false);
        activeResponse_.reset();
        markDirty();
    }

    bool ChatSession::responseInProgress() const
    {
        return activeResponse_ && !activeResponse_->finished.load(std::memory_order_acquire);
    }

    bool ChatSession::consumeDirtyFlag()
    {
        joinIfFinished();
        return dirty_.exchange(false, std::memory_order_acq_rel);
    }

    std::vector<ChatSession::Message> ChatSession::snapshotMessages() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }

    std::size_t ChatSession::addMessage(Message message)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        messages_.push_back(std::move(message));
        return messages_.size() - 1;
    }

    void ChatSession::appendToMessage(std::size_t index, std::string_view text)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= messages_.size())
            return;
        messages_[index].content.append(text);
    }

    void ChatSession::setMessagePending(std::size_t index, bool pending)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= messages_.size())
            return;
        messages_[index].pending = pending;
    }

    void ChatSession::markDirty()
    {
        dirty_.store(true, std::memory_order_release);
    }

    void ChatSession::joinIfFinished()
    {
        if (!activeResponse_)
            return;

        if (!activeResponse_->finished.load(std::memory_order_acquire))
            return;

        if (activeResponse_->worker.joinable())
            activeResponse_->worker.join();

        activeResponse_.reset();
    }

    void ChatSession::runSimulatedResponse(ResponseTask &task, std::string prompt)
    {
        using namespace std::chrono_literals;

        for (int repeat = 0; repeat < 5; ++repeat)
        {
            for (char ch : prompt)
            {
                if (task.cancel.load(std::memory_order_acquire))
                    break;

                appendToMessage(task.messageIndex, std::string_view(&ch, 1));
                markDirty();
                std::this_thread::sleep_for(80ms);
            }

            if (task.cancel.load(std::memory_order_acquire))
                break;

            appendToMessage(task.messageIndex, "\n");
            markDirty();
            std::this_thread::sleep_for(160ms);
        }

        setMessagePending(task.messageIndex, false);
        markDirty();
        task.finished.store(true, std::memory_order_release);
    }

    void ChatSession::runLlmResponse(ResponseTask &task, std::string prompt)
    {
        auto llm = task.llm;
        if (!llm)
        {
            runSimulatedResponse(task, std::move(prompt));
            return;
        }

        try
        {
            llm->set_system_prompt(systemPrompt_);
            ck::ai::GenerationConfig config;
            llm->generate(prompt, config, [this, &task](ck::ai::Chunk chunk) {
                if (task.cancel.load(std::memory_order_acquire))
                    return;

                if (!chunk.text.empty())
                    appendToMessage(task.messageIndex, chunk.text);

                if (chunk.is_last)
                    setMessagePending(task.messageIndex, false);

                markDirty();
            });
        }
        catch (const std::exception &e)
        {
            appendToMessage(task.messageIndex,
                            std::string("\n[error] ") + e.what() + '\n');
        }

        setMessagePending(task.messageIndex, false);
        markDirty();
        task.finished.store(true, std::memory_order_release);
    }

} // namespace ck::chat
