#include <cstdio>
#include <string>
#include <string_view>
#include <vector>


#include "ck/ai/model_manager.hpp"
#include "ck/ai/system_prompt_manager.hpp"
#include "ck/vision/keymap.hpp"
#include "native_host.hpp"
#include "chat_app.hpp"
#include "chat_options.hpp"
#include "chat_startup_options.hpp"

namespace
{

int run(const std::vector<std::string> &args)
{
    if (args.size() > 1 && (args[1] == "--help" || args[1] == "-h"))
    {
        std::printf("Usage: %s\n", args.empty() ? "ck-chat" : args[0].c_str());
        return 0;
    }
    ck::config::OptionRegistry optionsRegistry("ck-chat");
    ck::chat::registerChatOptions(optionsRegistry);
    optionsRegistry.loadDefaults();
    const ck::vision::ChatStartupSelection startupSelection =
        ck::vision::chatStartupSelectionFromRegistry(optionsRegistry);
    const ck::vision::ChatMarkdownOptions markdownOptions =
        ck::vision::chatMarkdownOptionsFromRegistry(optionsRegistry);
    ck::host::NativeClock clock;
    ck::host::NativeTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-chat", application.commands(), keymap_persistence);
    ck::vision::FileChatTranscriptStore transcripts;
    ck::ai::SystemPromptManager prompt_manager;
    ck::vision::SystemPromptManagerService prompts(prompt_manager);
    ck::ai::ModelManager model_manager;
    ck::vision::ModelManagerService models(model_manager);
    ck::vision::applyChatStartupSelection(startupSelection, prompts, models);
    ck::vision::RegistryChatSelectionPersistence selection_persistence(optionsRegistry);
    ck::vision::LlmChatResponseService responses(models);
    ck::vision::ChatApp chat(application, responses, transcripts, prompts, models, selection_persistence, markdownOptions);
    keymap.load();
    application.run();
    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    return run(ck::host::native_arguments(argc, argv));
}
