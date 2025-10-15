#include "ck/ai/llm.hpp"

#include "ck/ai/runtime_config.hpp"

#include <llama.h>

#include <cctype>
#include <chrono>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <algorithm>
#include <utility>
#include <vector>

namespace ck::ai {
namespace {
std::string build_stub_response(const std::string &prompt,
                                const std::string &system) {
  std::ostringstream stream;
  stream << "[ck-ai]";
  if (!system.empty())
    stream << " {system:" << system << "}";
  stream << " Response to: " << prompt;
  return stream.str();
}
} // namespace

Llm::Llm(std::string model_path, RuntimeConfig runtime)
    : model_path_(std::move(model_path)), runtime_(std::move(runtime)),
      model_(nullptr), context_(nullptr) {
  // Initialize llama backend
  llama_backend_init();

  // Load model
  auto model_params = llama_model_default_params();

  int gpuLayers = runtime_.gpu_layers;
#if defined(__APPLE__)
  if (gpuLayers == -1)
    gpuLayers = 9999; // Offload as many layers as possible when Metal is available
#endif
  if (gpuLayers < 0)
    gpuLayers = 0; // Keep CPU fallback for negative values on other platforms

  model_params.n_gpu_layers = gpuLayers;

  model_ = llama_model_load_from_file(model_path_.c_str(), model_params);

  if (!model_) {
    throw std::runtime_error("Failed to load model: " + model_path_);
  }

  // Create context
  auto ctx_params = llama_context_default_params();
  ctx_params.n_ctx = runtime_.context_window_tokens > 0
                         ? static_cast<int>(runtime_.context_window_tokens)
                         : 4096; // Default context size
  ctx_params.n_batch = std::max<uint32_t>(512u, ctx_params.n_ctx);
  ctx_params.n_threads = runtime_.threads > 0
                             ? runtime_.threads
                             : std::thread::hardware_concurrency();

  context_ = llama_init_from_model(model_, ctx_params);

  if (!context_) {
    llama_model_free(model_);
    model_ = nullptr;
    throw std::runtime_error("Failed to create context for model: " +
                             model_path_);
  }
}

Llm::~Llm() {
  if (context_) {
    llama_free(context_);
  }
  if (model_) {
    llama_model_free(model_);
  }
  llama_backend_free();
}

std::unique_ptr<Llm> Llm::open(const std::string &model_path,
                               const RuntimeConfig &config) {
  return std::unique_ptr<Llm>(new Llm(model_path, config));
}

void Llm::set_system_prompt(std::string system_prompt) {
  std::lock_guard<std::mutex> lock(mutex_);
  system_prompt_ = std::move(system_prompt);
}

void Llm::generate(const std::string &prompt, const GenerationConfig &config,
                   const std::function<void(Chunk)> &on_token) {
  if (!on_token)
    return;

  std::lock_guard<std::mutex> lock(mutex_);

  if (!context_ || !model_)
    return;

  const llama_vocab *vocab = llama_model_get_vocab(model_);
  if (!vocab)
    return;

  llama_memory_clear(llama_get_memory(context_), true);

  llama_sampler *sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
  if (!sampler)
    return;

  auto sampler_guard = std::unique_ptr<llama_sampler, decltype(&llama_sampler_free)>(
      sampler, llama_sampler_free);

  if (config.top_k > 0)
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(config.top_k));
  if (config.top_p > 0.0f && config.top_p <= 1.0f)
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(config.top_p, 1));
  if (config.temperature > 0.0f && config.temperature != 1.0f)
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(config.temperature));

  llama_sampler_chain_add(
      sampler, llama_sampler_init_dist(config.seed != 0 ? config.seed : LLAMA_DEFAULT_SEED));

  std::string full_prompt =
      system_prompt_.empty() ? prompt : system_prompt_ + "\n\n" + prompt;

  bool add_bos = true;
  int32_t token_count = llama_tokenize(vocab, full_prompt.c_str(), full_prompt.size(), nullptr, 0,
                                       add_bos, true);
  if (token_count < 0) {
    token_count = -token_count;
  }

  std::vector<llama_token> tokens(token_count);
  if (llama_tokenize(vocab, full_prompt.c_str(), full_prompt.size(), tokens.data(), tokens.size(),
                     add_bos, true) < 0) {
    return;
  }

  llama_sampler_reset(sampler);

  llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());

  std::string buffer;
  size_t lastSent = 0;
  bool stopReached = false;
  int generated = 0;
  const int max_tokens = config.max_tokens > 0 ? config.max_tokens : 256;

  while (generated < max_tokens) {
    int ret = llama_decode(context_, batch);
    if (ret != 0)
      break;

    llama_token token_id = llama_sampler_sample(sampler, context_, -1);
    llama_sampler_accept(sampler, token_id);

    if (llama_vocab_is_eog(vocab, token_id))
      break;

    char piece_buf[512];
    int len = llama_token_to_piece(vocab, token_id, piece_buf, sizeof(piece_buf), 0, true);
    if (len < 0)
      break;

    buffer.append(piece_buf, len);

    bool stopMatched = false;
    for (const auto &stop : config.stop) {
      if (stop.empty())
        continue;
      if (buffer.size() >= stop.size() &&
          buffer.compare(buffer.size() - stop.size(), stop.size(), stop) == 0) {
        buffer.resize(buffer.size() - stop.size());
        stopReached = stopMatched = true;
        break;
      }
    }

    if (buffer.size() > lastSent) {
      Chunk chunk;
      chunk.text = buffer.substr(lastSent);
      chunk.is_last = false;
      on_token(chunk);
      lastSent = buffer.size();
    }

    generated++;
    if (stopMatched || generated >= max_tokens)
      break;

    batch = llama_batch_get_one(&token_id, 1);
  }

  if (buffer.size() > lastSent) {
    Chunk chunk;
    chunk.text = buffer.substr(lastSent);
    chunk.is_last = true;
    on_token(chunk);
    lastSent = buffer.size();
  } else {
    Chunk chunk;
    chunk.is_last = true;
    on_token(chunk);
  }
}

std::string Llm::embed(const std::string &text) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::hash<std::string> hasher;
  auto value = static_cast<unsigned long long>(hasher(text));
  std::ostringstream stream;
  stream << model_path_ << ':' << value;
  return stream.str();
}

std::size_t Llm::token_count(const std::string &text) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!context_ || !model_)
    return 0;

  const llama_vocab *vocab = llama_model_get_vocab(model_);
  if (!vocab)
    return 0;

  int32_t n = llama_tokenize(vocab, text.c_str(), text.size(), nullptr, 0, true, false);
  if (n < 0)
    n = -n;
  return static_cast<std::size_t>(n);
}

} // namespace ck::ai
