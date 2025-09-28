#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ck::ai {
class Llm;
}

namespace ck::chat
{

    class ChatSession
    {
    public:
        enum class Role
        {
            User,
            Assistant,
            System,
        };

        struct Message
        {
            Role role;
            std::string content;
            bool pending = false;
        };

        ChatSession();
        ~ChatSession();

        void resetConversation();
        std::size_t addUserMessage(std::string prompt);
        std::size_t addSystemMessage(std::string text);

        void setSystemPrompt(std::string prompt);
        void startAssistantResponse(std::string prompt);
        void startAssistantResponse(std::string prompt,
                                    std::shared_ptr<ck::ai::Llm> llm);
        void cancelActiveResponse();

        bool responseInProgress() const;
        bool consumeDirtyFlag();
        std::vector<Message> snapshotMessages() const;

    private:
        struct ResponseTask
        {
            std::thread worker;
            std::atomic<bool> cancel{false};
            std::atomic<bool> finished{false};
            std::size_t messageIndex = 0;
            std::shared_ptr<ck::ai::Llm> llm;
        };

        std::size_t addMessage(Message message);
        void appendToMessage(std::size_t index, std::string_view text);
        void setMessagePending(std::size_t index, bool pending);
        void markDirty();
        void joinIfFinished();
        void runSimulatedResponse(ResponseTask &task, std::string prompt);
        void runLlmResponse(ResponseTask &task, std::string prompt);

        mutable std::mutex mutex_;
        std::vector<Message> messages_;
        std::unique_ptr<ResponseTask> activeResponse_;
        std::atomic<bool> dirty_{false};
        std::string systemPrompt_;
    };

} // namespace ck::chat
