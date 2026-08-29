#pragma once

#include <atomic>
#include <cstddef>
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
class Llm;
class ModelManager;
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

struct ChatModel
{
    std::string id;
    std::string name;
    std::string description;
    std::string hardware_requirements;
    std::size_t size_bytes = 0;
    std::filesystem::path local_path;
    std::size_t context_window_tokens = 0;
    std::size_t max_output_tokens = 0;
    std::vector<std::string> stop_sequences;
    bool is_downloaded = false;
    bool is_active = false;
};

struct ChatModelDownloadProgress
{
    std::string model_id;
    std::size_t bytes_downloaded = 0;
    std::size_t total_bytes = 0;
    double progress_percentage = 0.0;
};

struct ChatModelDownloadResult
{
    std::string model_id;
    bool success = false;
    bool cancelled = false;
    std::string error_message;
};

// Model selection, local deletion, and downloads are application workflows.
// The presentation talks to this service only; the production adapter owns the
// worker and keeps the ModelManager isolated from the UI thread while a
// download is in flight. Its progress callback is rate-limited before it can
// enqueue UI-thread work.
class ChatModelService
{
public:
    using DownloadProgressHandler = std::function<void(ChatModelDownloadProgress)>;
    using DownloadCompletionHandler = std::function<void(ChatModelDownloadResult)>;

    virtual ~ChatModelService() = default;

    virtual std::vector<ChatModel> available_models() const = 0;
    virtual std::vector<ChatModel> downloaded_models() const = 0;
    virtual std::optional<ChatModel> active_model() const = 0;
    virtual bool activate(std::string_view id) = 0;
    virtual bool deactivate(std::string_view id) = 0;
    virtual bool remove(std::string_view id) = 0;
    virtual bool start_download(std::string_view id, DownloadProgressHandler on_progress,
                                DownloadCompletionHandler on_complete) = 0;
    virtual void cancel_download() noexcept = 0;
    virtual bool download_running() const noexcept = 0;
};

class ModelManagerService final : public ChatModelService
{
public:
    explicit ModelManagerService(ck::ai::ModelManager &manager);
    ~ModelManagerService() override;

    std::vector<ChatModel> available_models() const override;
    std::vector<ChatModel> downloaded_models() const override;
    std::optional<ChatModel> active_model() const override;
    bool activate(std::string_view id) override;
    bool deactivate(std::string_view id) override;
    bool remove(std::string_view id) override;
    bool start_download(std::string_view id, DownloadProgressHandler on_progress,
                        DownloadCompletionHandler on_complete) override;
    void cancel_download() noexcept override;
    bool download_running() const noexcept override;

private:
    void refresh_models_locked();

    ck::ai::ModelManager &manager_;
    mutable std::mutex mutex_;
    std::vector<ChatModel> available_models_;
    std::vector<ChatModel> downloaded_models_;
    std::jthread worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::atomic_bool download_running_{false};
};

struct ChatResponseRequest
{
    std::string prompt;
    std::string system_prompt;
    std::string model_id;
    struct PriorMessage
    {
        enum class Role
        {
            User,
            Assistant,
        };

        Role role = Role::User;
        std::string content;
    };
    std::vector<PriorMessage> history;
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

// Loads the selected local model only on its worker, then streams ckai_core
// chunks through the response service boundary. Cancellation stops generation
// at the next emitted chunk and never calls into a ckVision view directly.
class LlmChatResponseService final : public ChatResponseService
{
public:
    explicit LlmChatResponseService(ChatModelService &model_service);
    ~LlmChatResponseService() override;

    void start(ChatResponseRequest request, ChunkHandler on_chunk, CompletionHandler on_complete) override;
    void cancel() noexcept override;
    bool running() const noexcept override;

private:
    void load_model(const ChatModel &model);

    ChatModelService &model_service_;
    mutable std::mutex mutex_;
    std::jthread worker_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::unique_ptr<ck::ai::Llm> llm_;
    std::string loaded_model_id_;
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
