#include "chat_session.hpp"

#include "ck/ai/llm.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <string_view>
#include <utility>

namespace ck::chat
{

    namespace
    {
        constexpr const char *kWelcomeMessage = "Welcome! Ask me anything.";
        constexpr std::size_t kRecentContextReserve = 6;
        constexpr const char *kArchivedPrefix = "[Archived from context] ";
        constexpr const char *kTrimmedPrefix = "[Trimmed from context] ";
        constexpr const char *kSummaryHeader = "[Conversation Summary]\n";
    }

    ChatSession::ChatSession()
    {
        settings_.max_context_tokens = 4096;
        settings_.summary_trigger_tokens = 2048;
        settings_.max_response_tokens = 512;
    }

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
            summaryMessageIndex_.reset();
        }

        lastPromptTokens_.store(0, std::memory_order_release);
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
        Message assistantPlaceholder{Role::Assistant, std::string(), true};
        assistantPlaceholder.include_in_context = false;
        task->messageIndex = addMessage(std::move(assistantPlaceholder));
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

    void ChatSession::setConversationSettings(const ConversationSettings &settings)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_ = settings;
        }
        markDirty();
    }

    ChatSession::ConversationSettings ChatSession::conversationSettings() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return settings_;
    }

    ChatSession::ContextStats ChatSession::contextStats() const
    {
        ContextStats stats;
        stats.prompt_tokens = lastPromptTokens_.load(std::memory_order_acquire);
        std::lock_guard<std::mutex> lock(mutex_);
        stats.max_context_tokens = settings_.max_context_tokens;
        stats.summary_trigger_tokens = settings_.summary_trigger_tokens;
        stats.max_response_tokens = settings_.max_response_tokens;
        stats.summarization_enabled = settings_.summary_trigger_tokens > 0;
        stats.summary_present = summaryMessageIndex_.has_value() &&
                                *summaryMessageIndex_ < messages_.size() &&
                                messages_[*summaryMessageIndex_].include_in_context &&
                                messages_[*summaryMessageIndex_].is_summary;
        return stats;
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
        auto &message = messages_[index];
        message.pending = pending;
        if (!pending && message.role == Role::Assistant)
            message.include_in_context = true;
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

    std::string ChatSession::role_prefix(Role role)
    {
        switch (role)
        {
        case Role::User:
            return "User";
        case Role::Assistant:
            return "Assistant";
        case Role::System:
            return "System";
        }
        return std::string{};
    }

    std::string ChatSession::buildModelPrompt() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::ostringstream stream;
        for (const auto &message : messages_)
        {
            if (!message.include_in_context)
                continue;
            if (message.pending && message.role == Role::Assistant)
                continue;

            stream << role_prefix(message.role) << ": " << message.content << '\n';
        }
        stream << "Assistant:";
        return stream.str();
    }

    std::optional<ChatSession::SummaryPlan> ChatSession::prepareSummaryPlan() const
    {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::size_t> candidates;
        candidates.reserve(messages_.size());
        for (std::size_t index = 0; index < messages_.size(); ++index)
        {
            const auto &message = messages_[index];
            if (!message.include_in_context)
                continue;
            if (message.pending && message.role == Role::Assistant)
                continue;
            if (summaryMessageIndex_ && index == *summaryMessageIndex_)
                continue;
            candidates.push_back(index);
        }

        if (candidates.size() <= kRecentContextReserve)
            return std::nullopt;

        std::size_t summariseCount = candidates.size() - kRecentContextReserve;
        if (summariseCount == 0)
            return std::nullopt;

        SummaryPlan plan;
        plan.message_indices.reserve(summariseCount);
        plan.messages.reserve(summariseCount);
        for (std::size_t i = 0; i < summariseCount; ++i)
        {
            std::size_t idx = candidates[i];
            plan.message_indices.push_back(idx);
            plan.messages.push_back(messages_[idx]);
        }

        return plan;
    }

    std::string ChatSession::formatMessagesForSummary(const std::vector<Message> &msgs) const
    {
        std::ostringstream stream;
        for (const auto &msg : msgs)
        {
            stream << role_prefix(msg.role) << ": " << msg.content << '\n';
        }
        return stream.str();
    }

    std::string ChatSession::existingSummaryText() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!summaryMessageIndex_ || *summaryMessageIndex_ >= messages_.size())
            return std::string{};

        const auto &msg = messages_[*summaryMessageIndex_];
        std::string_view content(msg.content);
        std::string_view header(kSummaryHeader);
        if (content.substr(0, header.size()) == header)
            content.remove_prefix(header.size());
        return std::string(content);
    }

    bool ChatSession::pruneOldMessages()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t index = 0; index < messages_.size(); ++index)
        {
            if (summaryMessageIndex_ && index == *summaryMessageIndex_)
                continue;

            auto &msg = messages_[index];
            if (!msg.include_in_context)
                continue;
            if (msg.pending && msg.role == Role::Assistant)
                continue;

            msg.include_in_context = false;
            if (msg.content.rfind(kTrimmedPrefix, 0) != 0)
                msg.content = std::string(kTrimmedPrefix) + msg.content;
            return true;
        }
        return false;
    }

    void ChatSession::ensureContextWithinLimits(ck::ai::Llm &llm)
    {
        auto settings = conversationSettings();
        const std::size_t maxContext = settings.max_context_tokens;
        const std::size_t summaryTrigger = settings.summary_trigger_tokens;
        const bool summarizationEnabled = summaryTrigger > 0;

        for (int iteration = 0; iteration < 5; ++iteration)
        {
            std::string prompt = buildModelPrompt();
            std::size_t tokens = llm.token_count(prompt);
            lastPromptTokens_.store(tokens, std::memory_order_release);
            markDirty();

            bool withinContext = (maxContext == 0) || (tokens <= maxContext);
            bool withinSummary = (!summarizationEnabled) || (tokens <= summaryTrigger);

            if (withinContext && withinSummary)
                break;

            bool modified = false;
            if (summarizationEnabled && tokens > summaryTrigger)
                modified = summarizeOldMessages(llm);

            if (!modified)
            {
                modified = pruneOldMessages();
                if (modified)
                    markDirty();
            }

            if (!modified)
                break;
        }
    }

    bool ChatSession::summarizeOldMessages(ck::ai::Llm &llm)
    {
        auto planOpt = prepareSummaryPlan();
        if (!planOpt)
            return false;

        auto plan = *planOpt;
        if (plan.message_indices.empty())
            return false;

        std::string conversation = formatMessagesForSummary(plan.messages);
        if (conversation.empty())
            return false;

        std::string priorSummary = existingSummaryText();

        std::ostringstream prompt;
        prompt << "You maintain a running summary of a conversation between a user and an assistant."
               << " Provide an updated concise summary that preserves key facts, decisions, and open"
               << " questions. Limit the result to a short paragraph or up to six bullet points.\n\n";

        if (!priorSummary.empty())
        {
            prompt << "Existing summary:\n" << priorSummary << "\n\n";
        }

        prompt << "New conversation excerpts:\n" << conversation << "\n\nUpdated summary:";

        std::string summaryPrompt = prompt.str();
        std::string summary;

        ck::ai::GenerationConfig config;
        config.max_tokens = 256;
        config.stop = {"\n\n", "\nUser:", "\nAssistant:", "\nSystem:", "\nYou:"};

        llm.generate(summaryPrompt, config, [&summary](ck::ai::Chunk chunk) {
            if (!chunk.text.empty())
                summary += chunk.text;
        });

    auto trim = [](std::string &text) {
        auto notSpace = [](int ch) { return !std::isspace(ch); };
        text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
        text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    };
    trim(summary);
        if (summary.empty())
            return false;

        applySummaryUpdate(plan.message_indices, summary);
        markDirty();
        return true;
    }

    void ChatSession::applySummaryUpdate(const std::vector<std::size_t> &indices,
                                         const std::string &summary)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        for (auto idx : indices)
        {
            if (idx >= messages_.size())
                continue;
            auto &msg = messages_[idx];
            msg.include_in_context = false;
            if (msg.content.rfind(kArchivedPrefix, 0) != 0)
                msg.content = std::string(kArchivedPrefix) + msg.content;
        }

        std::string summaryContent = std::string(kSummaryHeader) + summary;

        if (summaryMessageIndex_ && *summaryMessageIndex_ < messages_.size())
        {
            auto &summaryMsg = messages_[*summaryMessageIndex_];
            summaryMsg.content = summaryContent;
            summaryMsg.include_in_context = true;
            summaryMsg.is_summary = true;
        }
        else
        {
            Message summaryMsg;
            summaryMsg.role = Role::System;
            summaryMsg.content = summaryContent;
            summaryMsg.pending = false;
            summaryMsg.include_in_context = true;
            summaryMsg.is_summary = true;
            summaryMessageIndex_ = messages_.size();
            messages_.push_back(std::move(summaryMsg));
        }
    }

    void ChatSession::trimStopSequences(std::size_t index)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= messages_.size())
            return;

        auto &message = messages_[index];
        if (message.content.empty())
            return;

        static const std::string stops[] = {"\nUser:", "\nAssistant:",
                                            "\nSystem:", "\nYou:"};

        bool modified = false;
        bool changed;
        do
        {
            changed = false;
            for (const auto &stop : stops)
            {
                if (message.content.size() >= stop.size() &&
                    message.content.compare(message.content.size() - stop.size(),
                                             stop.size(), stop) == 0)
                {
                    message.content.resize(message.content.size() - stop.size());
                    changed = modified = true;
                    break;
                }
            }
        } while (changed && !message.content.empty());

        if (modified)
        {
            auto last = message.content.find_last_not_of(" \t\r\n");
            if (last == std::string::npos)
                message.content.clear();
            else
                message.content.resize(last + 1);
        }
    }
    void ChatSession::runLlmResponse(ResponseTask &task, std::string prompt)
    {
        auto llm = task.llm;
        if (!llm)
        {
            runSimulatedResponse(task, std::move(prompt));
            return;
        }

        (void)prompt;

        try
        {
            llm->set_system_prompt(systemPrompt_);

            auto settings = conversationSettings();

            ensureContextWithinLimits(*llm);
            std::string modelPrompt = buildModelPrompt();

            ck::ai::GenerationConfig config;
            config.stop = {"\n\n", "\nUser:", "\nAssistant:", "\nSystem:",
                           "\nYou:"};
            std::size_t runtimeLimit = llm->runtime_config().max_output_tokens;
            std::size_t desiredMax = settings.max_response_tokens;
            if (desiredMax == 0)
                desiredMax = runtimeLimit;
            if (runtimeLimit > 0 && desiredMax > runtimeLimit)
                desiredMax = runtimeLimit;
            if (desiredMax == 0)
                desiredMax = 512;
            config.max_tokens = static_cast<int>(desiredMax);
            llm->generate(modelPrompt, config, [this, &task](ck::ai::Chunk chunk) {
                if (task.cancel.load(std::memory_order_acquire))
                    return;

                if (!chunk.text.empty())
                {
                    appendToMessage(task.messageIndex, chunk.text);
                    trimStopSequences(task.messageIndex);
                }

                if (chunk.is_last)
                {
                    trimStopSequences(task.messageIndex);
                    setMessagePending(task.messageIndex, false);
                }

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
