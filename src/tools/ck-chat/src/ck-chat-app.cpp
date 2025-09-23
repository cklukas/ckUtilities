#include "ck/ai/config.hpp"
#include "ck/ai/llm.hpp"
#include "ck/app_info.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace
{
const ck::appinfo::ToolInfo &tool_info()
{
    return ck::appinfo::requireTool("ck-chat");
}

void print_banner()
{
    std::cout << "=== " << tool_info().displayName << " ===\n";
    std::cout << tool_info().shortDescription << "\n\n";
}

std::string read_prompt_from_stdin()
{
    std::cout << "Enter prompt: " << std::flush;
    std::string prompt;
    std::getline(std::cin, prompt);
    return prompt;
}

std::string parse_prompt(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--prompt" && i + 1 < argc)
            return std::string(argv[++i]);
        if (arg.rfind("--prompt=", 0) == 0)
            return std::string(arg.substr(9));
    }
    return {};
}

ck::ai::RuntimeConfig runtime_from_config(const ck::ai::Config &config)
{
    ck::ai::RuntimeConfig runtime = config.runtime;
    if (runtime.model_path.empty())
        runtime.model_path = "stub-model.gguf";
    return runtime;
}

void stream_response(ck::ai::Llm &llm, const std::string &prompt)
{
    ck::ai::GenerationConfig config;
    std::cout << "\n[ck-chat] streaming response...\n";
    llm.generate(prompt, config, [](ck::ai::Chunk chunk) {
        std::cout << chunk.text << std::flush;
        if (chunk.is_last)
            std::cout << "\n" << std::flush;
    });
}
}

int main(int argc, char **argv)
{
    print_banner();

    auto cfg = ck::ai::ConfigLoader::load_or_default();
    auto runtime = runtime_from_config(cfg);
    auto llm = ck::ai::Llm::open(runtime.model_path, runtime);
    llm->set_system_prompt("You are the ck-ai scaffolding.");

    std::string prompt = parse_prompt(argc, argv);
    if (prompt.empty())
        prompt = read_prompt_from_stdin();

    if (prompt.empty())
    {
        std::cout << "No prompt provided.\n";
        return 0;
    }

    stream_response(*llm, prompt);
    return 0;
}
