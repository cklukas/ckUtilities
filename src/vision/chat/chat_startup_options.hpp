#pragma once

#include <string>

#include "ck/options.hpp"

namespace ck::vision
{

class ChatModelService;
class ChatPromptService;

// The persisted chat profile intentionally controls only the selections the
// native composition root can apply before the UI starts. Validation of IDs
// remains with the prompt and model services that own those resources.
struct ChatStartupSelection
{
    std::string active_model_id;
    std::string active_prompt_id;
};

ChatStartupSelection chatStartupSelectionFromRegistry(const ck::config::OptionRegistry &registry);
void applyChatStartupSelection(const ChatStartupSelection &selection,
                               ChatPromptService &prompt_service,
                               ChatModelService &model_service);

} // namespace ck::vision
