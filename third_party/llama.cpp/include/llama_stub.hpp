#pragma once

#include <string>

namespace llama
{
struct RuntimeOptions
{
    int threads = 1;
    int max_tokens = 256;
};

class LlamaContext
{
public:
    LlamaContext(std::string model_path, RuntimeOptions options);

    std::string describe() const;
    RuntimeOptions runtime_options() const;

private:
    std::string model_path_;
    RuntimeOptions options_;
};
} // namespace llama
