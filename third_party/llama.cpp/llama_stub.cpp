#include <llama_stub.hpp>

namespace llama
{
LlamaContext::LlamaContext(std::string model_path, RuntimeOptions options)
    : model_path_(std::move(model_path)), options_(options)
{
}

std::string LlamaContext::describe() const
{
    return "llama.cpp stub(" + model_path_ + ")";
}

RuntimeOptions LlamaContext::runtime_options() const
{
    return options_;
}
}
