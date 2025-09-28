#include "ck/ai/llm.hpp"

#include "ck/ai/runtime_config.hpp"

#include <llama.h>

#include <cctype>
#include <chrono>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
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
  model_params.n_gpu_layers = 0; // CPU only for now
  model_ = llama_model_load_from_file(model_path_.c_str(), model_params);

  if (!model_) {
    throw std::runtime_error("Failed to load model: " + model_path_);
  }

  // Create context
  auto ctx_params = llama_context_default_params();
  ctx_params.n_ctx = 4096; // Default context size
  ctx_params.n_batch = 512;
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
  system_prompt_ = std::move(system_prompt);
}

void Llm::generate(const std::string &prompt, const GenerationConfig &config,
                   const std::function<void(Chunk)> &on_token) {
  if (!on_token || !context_ || !model_)
    return;

  // Prepare the full prompt with system message
  std::string full_prompt =
      system_prompt_.empty() ? prompt : system_prompt_ + "\n\n" + prompt;

  // Get vocabulary for tokenization
  const llama_vocab *vocab = llama_model_get_vocab(model_);
  if (!vocab) {
    return;
  }

  // Tokenize the prompt
  std::vector<llama_token> tokens;
  tokens.resize(full_prompt.length() + 1);
  int n_tokens =
      llama_tokenize(vocab, full_prompt.c_str(), full_prompt.length(),
                     tokens.data(), tokens.size(), true, false);
  if (n_tokens < 0) {
    tokens.resize(-n_tokens);
    n_tokens = llama_tokenize(vocab, full_prompt.c_str(), full_prompt.length(),
                              tokens.data(), tokens.size(), true, false);
  }
  if (n_tokens <= 0) {
    return; // No tokens to process
  }
  tokens.resize(n_tokens);

  // For now, use a simple approach that doesn't crash
  // TODO: Implement proper llama.cpp generation once we understand the API
  // better
  const std::string response = build_stub_response(prompt, system_prompt_);
  Chunk chunk;
  chunk.is_last = false;
  for (std::size_t i = 0; i < response.size(); ++i) {
    chunk.text.assign(1, response[i]);
    chunk.is_last = (i + 1 == response.size());
    on_token(chunk);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

std::string Llm::embed(const std::string &text) const {
  std::hash<std::string> hasher;
  auto value = static_cast<unsigned long long>(hasher(text));
  std::ostringstream stream;
  stream << model_path_ << ':' << value;
  return stream.str();
}

std::size_t Llm::token_count(const std::string &text) const {
  if (!context_ || !model_)
    return 0;

  const llama_vocab *vocab = llama_model_get_vocab(model_);
  if (!vocab) {
    return 0;
  }

  std::vector<llama_token> tokens;
  tokens.resize(text.length() + 1);
  int n_tokens = llama_tokenize(vocab, text.c_str(), text.length(),
                                tokens.data(), tokens.size(), true, false);
  if (n_tokens < 0) {
    tokens.resize(-n_tokens);
    n_tokens = llama_tokenize(vocab, text.c_str(), text.length(), tokens.data(),
                              tokens.size(), true, false);
  }

  return n_tokens > 0 ? static_cast<std::size_t>(n_tokens) : 0;
}

} // namespace ck::ai
