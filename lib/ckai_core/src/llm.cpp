#include "ck/ai/llm.hpp"

#include "ck/ai/runtime_config.hpp"

#include <chrono>
#include <cctype>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace ck::ai
{
namespace
{
std::string build_stub_response(const std::string &prompt, const std::string &system)
{
    std::ostringstream stream;
    stream << "[ck-ai]";
    if (!system.empty())
        stream << " {system:" << system << "}";
    stream << " Response to: " << prompt;
    return stream.str();
}
}

Llm::Llm(std::string model_path, RuntimeConfig runtime)
    : model_path_(std::move(model_path)), runtime_(std::move(runtime))
{
}

std::unique_ptr<Llm> Llm::open(const std::string &model_path, const RuntimeConfig &config)
{
    return std::unique_ptr<Llm>(new Llm(model_path, config));
}

void Llm::set_system_prompt(std::string system_prompt)
{
    system_prompt_ = std::move(system_prompt);
}

void Llm::generate(const std::string &prompt, const GenerationConfig &,
                   const std::function<void(Chunk)> &on_token)
{
    if (!on_token)
        return;

    const std::string response = build_stub_response(prompt, system_prompt_);
    Chunk chunk;
    chunk.is_last = false;
    for (std::size_t i = 0; i < response.size(); ++i)
    {
        chunk.text.assign(1, response[i]);
        chunk.is_last = (i + 1 == response.size());
        on_token(chunk);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

std::string Llm::embed(const std::string &text) const
{
    std::hash<std::string> hasher;
    auto value = static_cast<unsigned long long>(hasher(text));
    std::ostringstream stream;
    stream << model_path_ << ':' << value;
    return stream.str();
}

std::size_t Llm::token_count(const std::string &text) const
{
    if (text.empty())
        return 0;

    std::size_t count = 0;
    bool in_token = false;
    for (char ch : text)
    {
        if (std::isspace(static_cast<unsigned char>(ch)))
        {
            if (in_token)
            {
                ++count;
                in_token = false;
            }
        }
        else
        {
            in_token = true;
        }
    }
    if (in_token)
        ++count;
    return count;
}

} // namespace ck::ai
