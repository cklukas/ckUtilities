#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
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
            bool include_in_context = true;
            bool is_summary = false;
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

        struct ConversationSettings
        {
            std::size_t max_context_tokens = 4096;
            std::size_t summary_trigger_tokens = 2048;
            std::size_t max_response_tokens = 512;
        };

        struct ContextStats
        {
            std::size_t prompt_tokens = 0;
            std::size_t max_context_tokens = 0;
            std::size_t summary_trigger_tokens = 0;
            std::size_t max_response_tokens = 0;
            bool summarization_enabled = false;
            bool summary_present = false;
        };

        void setConversationSettings(const ConversationSettings &settings);
        ConversationSettings conversationSettings() const;
        ContextStats contextStats() const;

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
        void ensureContextWithinLimits(ck::ai::Llm &llm);
        bool summarizeOldMessages(ck::ai::Llm &llm);
        std::string buildModelPrompt() const;
        std::string formatMessagesForSummary(const std::vector<Message> &msgs) const;
        std::string existingSummaryText() const;
        bool pruneOldMessages();
        void trimStopSequences(std::size_t index);

        struct SummaryPlan
        {
            std::vector<std::size_t> message_indices;
            std::vector<Message> messages;
        };
        std::optional<SummaryPlan> prepareSummaryPlan() const;
        void applySummaryUpdate(const std::vector<std::size_t> &indices,
                                const std::string &summary);
        static std::string role_prefix(Role role);

        mutable std::mutex mutex_;
        std::vector<Message> messages_;
        std::unique_ptr<ResponseTask> activeResponse_;
        std::atomic<bool> dirty_{false};
        std::string systemPrompt_;
        std::optional<std::size_t> summaryMessageIndex_;
        ConversationSettings settings_{};
        std::atomic<std::size_t> lastPromptTokens_{0};
    };

} // namespace ck::chat
