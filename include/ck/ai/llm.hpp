#pragma once

#include "ck/ai/runtime_config.hpp"

#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for llama types
struct llama_model;
struct llama_context;

namespace ck::ai {
struct GenerationConfig {
  int max_tokens = 512;
  float temperature = 0.7f;
  float top_p = 0.9f;
  int top_k = 40;
  int seed = 0;
  std::vector<std::string> stop;
};

struct Chunk {
  std::string text;
  bool is_last = false;
};

class Llm {
public:
  static std::unique_ptr<Llm> open(const std::string &model_path,
                                   const RuntimeConfig &config);

  void set_system_prompt(std::string system_prompt);
  void generate(const std::string &prompt, const GenerationConfig &config,
                const std::function<void(Chunk)> &on_token);
  std::string embed(const std::string &text) const;
  std::size_t token_count(const std::string &text) const;

  const RuntimeConfig &runtime_config() const noexcept { return runtime_; }

  ~Llm();

private:
  explicit Llm(std::string model_path, RuntimeConfig runtime);

  std::string model_path_;
  RuntimeConfig runtime_;
  std::string system_prompt_;

  // llama.cpp objects
  llama_model *model_;
  llama_context *context_;
  mutable std::mutex mutex_;
  bool stub_mode_ = false;
};
} // namespace ck::ai
