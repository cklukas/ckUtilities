#include "chat_startup_options.hpp"

#include "chat_services.hpp"
#include "chat_options.hpp"

namespace ck::vision
{

ChatStartupSelection chatStartupSelectionFromRegistry(const ck::config::OptionRegistry &registry)
{
    return {.active_model_id = registry.getString(ck::chat::kOptionActiveModelId),
            .active_prompt_id = registry.getString(ck::chat::kOptionActivePromptId)};
}

void applyChatStartupSelection(const ChatStartupSelection &selection,
                               ChatPromptService &prompt_service,
                               ChatModelService &model_service)
{
    if (!selection.active_prompt_id.empty())
        prompt_service.activate(selection.active_prompt_id);
    if (!selection.active_model_id.empty())
        model_service.activate(selection.active_model_id);
}

} // namespace ck::vision
