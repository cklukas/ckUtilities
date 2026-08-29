#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ck::ai
{
class SystemPromptManager;
}

namespace ck::vision
{

struct ChatSystemPrompt
{
    std::string id;
    std::string name;
    std::string message;
    bool is_default = false;
    bool is_active = false;
};

// Prompt state belongs to the chat domain. The UI gets this narrow service,
// while the production adapter can delegate to ckai_core's persistent manager.
class ChatPromptService
{
public:
    virtual ~ChatPromptService() = default;

    virtual std::vector<ChatSystemPrompt> prompts() const = 0;
    virtual std::optional<ChatSystemPrompt> active_prompt() const = 0;
    virtual bool add_or_update(ChatSystemPrompt prompt) = 0;
    virtual bool remove(std::string_view id) = 0;
    virtual bool activate(std::string_view id) = 0;
    virtual bool restore_default(std::string_view id) = 0;
    virtual bool is_default_modified(std::string_view id) const = 0;
};

class SystemPromptManagerService final : public ChatPromptService
{
public:
    explicit SystemPromptManagerService(ck::ai::SystemPromptManager &manager) noexcept;

    std::vector<ChatSystemPrompt> prompts() const override;
    std::optional<ChatSystemPrompt> active_prompt() const override;
    bool add_or_update(ChatSystemPrompt prompt) override;
    bool remove(std::string_view id) override;
    bool activate(std::string_view id) override;
    bool restore_default(std::string_view id) override;
    bool is_default_modified(std::string_view id) const override;

private:
    ck::ai::SystemPromptManager &manager_;
};

struct ChatResponseRequest
{
    std::string prompt;
    std::string system_prompt;
};

// The chat presentation depends on streaming callbacks, not a model runtime.
// A real model adapter may emit many chunks; the default adapter below turns a
// simple responder into one asynchronous response without blocking the UI.
class ChatResponseService
{
public:
    using ChunkHandler = std::function<void(std::string)>;
    using CompletionHandler = std::function<void(bool cancelled)>;

    virtual ~ChatResponseService() = default;

    virtual void start(ChatResponseRequest request, ChunkHandler on_chunk, CompletionHandler on_complete) = 0;
    virtual void cancel() noexcept = 0;
    virtual bool running() const noexcept = 0;
};

using ChatResponder = std::function<std::string(const ChatResponseRequest &request)>;

class ThreadedChatResponseService final : public ChatResponseService
{
public:
    explicit ThreadedChatResponseService(ChatResponder responder);
    ~ThreadedChatResponseService() override;

    void start(ChatResponseRequest request, ChunkHandler on_chunk, CompletionHandler on_complete) override;
    void cancel() noexcept override;
    bool running() const noexcept override;

private:
    ChatResponder responder_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::atomic_bool running_{false};
};

// Persistence policy for an exported conversation.  The presentation supplies
// a chosen path and plain transcript value; it never opens files itself.
class ChatTranscriptStore
{
public:
    virtual ~ChatTranscriptStore() = default;

    virtual bool write(const std::filesystem::path &path, const std::string &transcript) = 0;
};

class FileChatTranscriptStore final : public ChatTranscriptStore
{
public:
    bool write(const std::filesystem::path &path, const std::string &transcript) override;
};

} // namespace ck::vision
