#include "ck/ai/config.hpp"
#include "ck/ai/llm.hpp"
#include "ck/app_info.hpp"
#include "ck/hotkeys.hpp"

#include "ui/chat_app.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>

namespace
{
    const ck::appinfo::ToolInfo &tool_info()
    {
        return ck::appinfo::requireTool("ck-chat");
    }

    void crash_handler(int sig, siginfo_t *, void *)
    {
        void *frames[64];
        int count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, count, STDERR_FILENO);
        _exit(128 + sig);
    }

    void install_crash_handlers()
    {
        struct sigaction action {};
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_SIGINFO | SA_RESETHAND;
        action.sa_sigaction = crash_handler;
        sigaction(SIGSEGV, &action, nullptr);
        sigaction(SIGABRT, &action, nullptr);
    }

    std::string read_prompt_from_stdin()
    {
        std::cout << "Enter prompt: " << std::flush;
        std::string prompt;
        std::getline(std::cin, prompt);
        return prompt;
    }

    bool is_help_flag(std::string_view arg)
    {
        return arg == "--help" || arg == "-h";
    }

    std::optional<std::string> parse_prompt_arg(int argc, char **argv, bool &showHelp)
    {
        std::optional<std::string> prompt;
        for (int i = 1; i < argc; ++i)
        {
            std::string_view arg(argv[i]);
            if (is_help_flag(arg))
            {
                showHelp = true;
                continue;
            }
            if (arg == "--prompt" && i + 1 < argc)
            {
                prompt = std::string(argv[++i]);
                continue;
            }
            if (arg.rfind("--prompt=", 0) == 0)
            {
                prompt = std::string(arg.substr(9));
                continue;
            }
        }
        return prompt;
    }

    ck::ai::RuntimeConfig runtime_from_config(const ck::ai::Config &config)
    {
        ck::ai::RuntimeConfig runtime = config.runtime;
        if (runtime.model_path.empty())
            runtime.model_path = "stub-model.gguf";
        return runtime;
    }

    void print_banner()
    {
        const auto &info = tool_info();
        std::cout << "=== " << info.displayName << " ===\n";
        std::cout << info.shortDescription << "\n\n";
    }

    void stream_response(ck::ai::Llm &llm, const std::string &prompt)
    {
        ck::ai::GenerationConfig config;
        std::cout << "\n[ck-chat] streaming response...\n";
        llm.generate(prompt, config, [](ck::ai::Chunk chunk)
                     {
        std::cout << chunk.text << std::flush;
        if (chunk.is_last)
            std::cout << "\n" << std::flush; });
    }

    struct CliOptions
    {
        bool showHelp = false;
        std::optional<std::string> prompt;
    };

    CliOptions parse_cli(int argc, char **argv)
    {
        CliOptions options;
        options.prompt = parse_prompt_arg(argc, argv, options.showHelp);
        return options;
    }

    int run_cli(const CliOptions &options)
    {
        print_banner();

        if (options.showHelp && !options.prompt)
        {
            std::cout << "Usage: " << tool_info().executable << " [--hotkeys SCHEME] --prompt <TEXT>\n";
            std::cout << "Launch the Turbo Vision interface without --prompt." << '\n';
            std::cout << "Available schemes: linux, mac, windows, custom." << '\n';
            std::cout << "Set CK_HOTKEY_SCHEME to select a default hotkey scheme." << std::endl;
            return 0;
        }

        std::string prompt;
        if (options.prompt)
            prompt = *options.prompt;
        else
            prompt = read_prompt_from_stdin();

        if (prompt.empty())
        {
            std::cout << "No prompt provided.\n";
            return 0;
        }

        auto cfg = ck::ai::ConfigLoader::load_or_default();
        auto runtime = runtime_from_config(cfg);
        auto llm = ck::ai::Llm::open(runtime.model_path, runtime);
        llm->set_system_prompt("You are the CL Utilities scaffolding.");

        stream_response(*llm, prompt);
        return 0;
    }

} // namespace

int main(int argc, char **argv)
{
    install_crash_handlers();

    ck::hotkeys::registerDefaultSchemes();
    ck::hotkeys::initializeFromEnvironment();
    ck::hotkeys::applyCommandLineScheme(argc, argv);

    CliOptions options = parse_cli(argc, argv);
    if (options.prompt || options.showHelp)
        return run_cli(options);

    ChatApp app(argc, argv);
    app.run();
    app.shutDown();
    return 0;
}
