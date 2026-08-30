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

RegistryChatSelectionPersistence::RegistryChatSelectionPersistence(ck::config::OptionRegistry &registry) noexcept
    : registry_(registry)
{
}

bool RegistryChatSelectionPersistence::save_active_model(std::string_view id)
{
    return save(ck::chat::kOptionActiveModelId, id);
}

bool RegistryChatSelectionPersistence::save_active_prompt(std::string_view id)
{
    return save(ck::chat::kOptionActivePromptId, id);
}

bool RegistryChatSelectionPersistence::save(std::string_view key, std::string_view value)
{
    const ck::config::OptionRegistry::Snapshot snapshot = registry_.snapshot();
    registry_.set(std::string(key), ck::config::OptionValue(std::string(value)));
    if (registry_.saveDefaults())
        return true;
    registry_.restore(snapshot);
    return false;
}

} // namespace ck::vision
