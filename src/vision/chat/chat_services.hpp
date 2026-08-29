#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ck::vision
{

// The chat presentation depends on streaming callbacks, not a model runtime.
// A real model adapter may emit many chunks; the default adapter below turns a
// simple responder into one asynchronous response without blocking the UI.
class ChatResponseService
{
public:
    using ChunkHandler = std::function<void(std::string)>;
    using CompletionHandler = std::function<void(bool cancelled)>;

    virtual ~ChatResponseService() = default;

    virtual void start(std::string prompt, ChunkHandler on_chunk, CompletionHandler on_complete) = 0;
    virtual void cancel() noexcept = 0;
    virtual bool running() const noexcept = 0;
};

using ChatResponder = std::function<std::string(const std::string &prompt)>;

class ThreadedChatResponseService final : public ChatResponseService
{
public:
    explicit ThreadedChatResponseService(ChatResponder responder);
    ~ThreadedChatResponseService() override;

    void start(std::string prompt, ChunkHandler on_chunk, CompletionHandler on_complete) override;
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
