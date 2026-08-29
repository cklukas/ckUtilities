#include <string>

#include <cvision/term/posix_clock.hpp>
#include <cvision/term/posix_terminal.hpp>

#include "ck/ai/system_prompt_manager.hpp"
#include "chat_app.hpp"

int main()
{
    ckv::term::PosixClock clock;
    ckv::term::PosixTerminal terminal(clock);
    ckv::ui::Application application(terminal, clock);
    ck::vision::ThreadedChatResponseService responses([](const ck::vision::ChatResponseRequest &request) {
        return "No model service is configured yet. Received: " + request.prompt;
    });
    ck::vision::FileChatTranscriptStore transcripts;
    ck::ai::SystemPromptManager prompt_manager;
    ck::vision::SystemPromptManagerService prompts(prompt_manager);
    ck::vision::ChatApp chat(application, responses, transcripts, prompts);
    application.run();
    return 0;
}
