#include "chat_services.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <utility>

#include "ck/ai/llm.hpp"
#include "ck/ai/model_manager.hpp"
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

ChatModel as_chat_model(const ck::ai::ModelInfo &model)
{
    return {.id = model.id,
            .name = model.name,
            .description = model.description,
            .hardware_requirements = model.hardware_requirements,
            .size_bytes = model.size_bytes,
            .local_path = model.local_path,
            .context_window_tokens = model.default_context_window_tokens,
            .max_output_tokens = model.default_max_output_tokens,
            .stop_sequences = model.default_stop_sequences,
            .is_downloaded = model.is_downloaded,
            .is_active = model.is_active};
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

ModelManagerService::ModelManagerService(ck::ai::ModelManager &manager)
    : manager_(manager)
{
    std::scoped_lock lock(mutex_);
    refresh_models_locked();
}

ModelManagerService::~ModelManagerService()
{
    cancel_download();
    std::jthread worker;
    {
        std::scoped_lock lock(mutex_);
        worker = std::move(worker_);
    }
    if (worker.joinable())
        worker.join();
}

std::vector<ChatModel> ModelManagerService::available_models() const
{
    std::scoped_lock lock(mutex_);
    return available_models_;
}

std::vector<ChatModel> ModelManagerService::downloaded_models() const
{
    std::scoped_lock lock(mutex_);
    return downloaded_models_;
}

std::optional<ChatModel> ModelManagerService::active_model() const
{
    std::scoped_lock lock(mutex_);
    const auto found = std::find_if(downloaded_models_.begin(), downloaded_models_.end(),
                                    [](const ChatModel &model) { return model.is_active; });
    return found == downloaded_models_.end() ? std::nullopt : std::optional<ChatModel>{*found};
}

bool ModelManagerService::activate(std::string_view id)
{
    std::scoped_lock lock(mutex_);
    if (download_running_.load(std::memory_order_acquire))
        return false;
    const bool activated = manager_.activate_model(std::string(id));
    refresh_models_locked();
    return activated;
}

bool ModelManagerService::deactivate(std::string_view id)
{
    std::scoped_lock lock(mutex_);
    if (download_running_.load(std::memory_order_acquire))
        return false;
    const bool deactivated = manager_.deactivate_model(std::string(id));
    refresh_models_locked();
    return deactivated;
}

bool ModelManagerService::remove(std::string_view id)
{
    std::scoped_lock lock(mutex_);
    if (download_running_.load(std::memory_order_acquire))
        return false;
    const bool removed = manager_.delete_model(std::string(id));
    refresh_models_locked();
    return removed;
}

bool ModelManagerService::start_download(std::string_view id, DownloadProgressHandler on_progress,
                                         DownloadCompletionHandler on_complete)
{
    std::jthread previous;
    {
        std::scoped_lock lock(mutex_);
        if (download_running_.load(std::memory_order_acquire))
            return false;
        previous = std::move(worker_);
    }
    if (previous.joinable())
        previous.join();

    const std::string model_id(id);
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    {
        std::scoped_lock lock(mutex_);
        const auto model = std::find_if(available_models_.begin(), available_models_.end(),
                                        [&model_id](const ChatModel &candidate) {
                                            return candidate.id == model_id;
                                        });
        if (model == available_models_.end() || model->is_downloaded)
            return false;
        cancellation_ = cancellation;
        download_running_.store(true, std::memory_order_release);
        worker_ = std::jthread([this, model_id, cancellation, on_progress = std::move(on_progress),
                                on_complete = std::move(on_complete)]() mutable {
            std::string error_message;
            auto next_progress_update = std::chrono::steady_clock::time_point{};
            const bool success = manager_.download_model_cancellable(
                model_id,
                [cancellation, &on_progress, &next_progress_update](const ck::ai::ModelDownloadProgress &progress) {
                    const bool keep_downloading = !cancellation->load(std::memory_order_acquire);
                    const auto now = std::chrono::steady_clock::now();
                    if (keep_downloading && on_progress && now >= next_progress_update)
                    {
                        next_progress_update = now + std::chrono::milliseconds(100);
                        on_progress({.model_id = progress.model_id,
                                     .bytes_downloaded = progress.bytes_downloaded,
                                     .total_bytes = progress.total_bytes,
                                     .progress_percentage = progress.progress_percentage});
                    }
                    return keep_downloading;
                },
                &error_message);
            const bool cancelled = cancellation->load(std::memory_order_acquire);
            {
                std::scoped_lock lock(mutex_);
                refresh_models_locked();
                download_running_.store(false, std::memory_order_release);
                if (cancellation_ == cancellation)
                    cancellation_.reset();
            }
            if (on_complete)
                on_complete({.model_id = model_id,
                             .success = success,
                             .cancelled = cancelled,
                             .error_message = std::move(error_message)});
        });
    }
    return true;
}

void ModelManagerService::cancel_download() noexcept
{
    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::scoped_lock lock(mutex_);
        cancellation = cancellation_;
    }
    if (cancellation)
        cancellation->store(true, std::memory_order_release);
}

bool ModelManagerService::download_running() const noexcept
{
    return download_running_.load(std::memory_order_acquire);
}

void ModelManagerService::refresh_models_locked()
{
    const std::vector<ck::ai::ModelInfo> available = manager_.get_available_models();
    const std::vector<ck::ai::ModelInfo> downloaded = manager_.get_downloaded_models();
    const std::optional<ck::ai::ModelInfo> active = manager_.get_active_model();
    const std::string active_id = active ? active->id : std::string{};

    available_models_.clear();
    available_models_.reserve(available.size());
    for (const ck::ai::ModelInfo &model : available)
    {
        ChatModel chat_model = as_chat_model(model);
        const auto downloaded_model = std::find_if(downloaded.begin(), downloaded.end(), [&model](const auto &candidate) {
            return candidate.id == model.id;
        });
        chat_model.is_downloaded = downloaded_model != downloaded.end();
        chat_model.is_active = model.id == active_id;
        available_models_.push_back(std::move(chat_model));
    }

    downloaded_models_.clear();
    downloaded_models_.reserve(downloaded.size());
    for (const ck::ai::ModelInfo &model : downloaded)
    {
        ChatModel chat_model = as_chat_model(model);
        chat_model.is_downloaded = true;
        chat_model.is_active = model.id == active_id;
        downloaded_models_.push_back(std::move(chat_model));
    }
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

LlmChatResponseService::LlmChatResponseService(ChatModelService &model_service)
    : model_service_(model_service)
{
}

LlmChatResponseService::~LlmChatResponseService()
{
    cancel();
    std::scoped_lock lock(mutex_);
    if (worker_.joinable())
        worker_.join();
}

void LlmChatResponseService::start(ChatResponseRequest request, ChunkHandler on_chunk, CompletionHandler on_complete)
{
    cancel();

    std::jthread previous;
    {
        std::scoped_lock lock(mutex_);
        previous = std::move(worker_);
    }
    if (previous.joinable())
        previous.join();

    const std::optional<ChatModel> model = model_service_.active_model();
    if (!model || model->id != request.model_id || model->local_path.empty())
    {
        if (on_chunk)
            on_chunk("[Activate a downloaded local model before sending a prompt.]");
        if (on_complete)
            on_complete(false);
        return;
    }

    auto cancellation = std::make_shared<std::atomic_bool>(false);
    running_.store(true, std::memory_order_release);
    std::jthread worker([this, model = *model, cancellation, request = std::move(request),
                         on_chunk = std::move(on_chunk), on_complete = std::move(on_complete)]() mutable {
        load_model(model);
        if (!cancellation->load(std::memory_order_acquire))
        {
            llm_->set_system_prompt(request.system_prompt);
            ck::ai::GenerationConfig config;
            config.max_tokens = model.max_output_tokens == 0 ? 512 : static_cast<int>(model.max_output_tokens);
            config.stop = model.stop_sequences;
            llm_->generate_cancellable(request.prompt, config, [cancellation, &on_chunk](ck::ai::Chunk chunk) {
                const bool keep_generating = !cancellation->load(std::memory_order_acquire);
                if (keep_generating && on_chunk && !chunk.text.empty())
                    on_chunk(std::move(chunk.text));
                return keep_generating;
            });
        }
        const bool cancelled = cancellation->load(std::memory_order_acquire);
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

void LlmChatResponseService::cancel() noexcept
{
    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::scoped_lock lock(mutex_);
        cancellation = cancellation_;
    }
    if (cancellation)
        cancellation->store(true, std::memory_order_release);
}

bool LlmChatResponseService::running() const noexcept
{
    return running_.load(std::memory_order_acquire);
}

void LlmChatResponseService::load_model(const ChatModel &model)
{
    if (llm_ && loaded_model_id_ == model.id)
        return;

    ck::ai::RuntimeConfig runtime;
    runtime.model_path = model.local_path.string();
    runtime.context_window_tokens = model.context_window_tokens == 0 ? 4096 : model.context_window_tokens;
    runtime.max_output_tokens = model.max_output_tokens == 0 ? 512 : model.max_output_tokens;
    llm_ = ck::ai::Llm::open(runtime.model_path, runtime);
    loaded_model_id_ = model.id;
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
