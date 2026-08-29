#include <cstdio>
#include <string>
#include <string_view>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>

#include "ck/ai/model_manager.hpp"
#include "ck/ai/system_prompt_manager.hpp"
#include "ck/vision/keymap.hpp"
#include "chat_app.hpp"

int main(int argc, char **argv)
{
    if (argc > 1 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h"))
    {
        std::printf("Usage: %s\n", argc > 0 ? argv[0] : "ck-chat-ckvision");
        return 0;
    }
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::DefaultKeymapPersistence keymap_persistence;
    ck::vision::KeymapController keymap("ck-chat", application.commands(), keymap_persistence);
    ck::vision::FileChatTranscriptStore transcripts;
    ck::ai::SystemPromptManager prompt_manager;
    ck::vision::SystemPromptManagerService prompts(prompt_manager);
    ck::ai::ModelManager model_manager;
    ck::vision::ModelManagerService models(model_manager);
    ck::vision::LlmChatResponseService responses(models);
    ck::vision::ChatApp chat(application, responses, transcripts, prompts, models);
    keymap.load();
    application.run();
    return 0;
}
