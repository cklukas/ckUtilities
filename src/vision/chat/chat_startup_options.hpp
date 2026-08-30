#pragma once

#include <string>
#include <string_view>

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

// ChatApp uses this narrow capability when the user changes a selection. The
// production adapter keeps profile I/O in composition code, while tests can
// supply an in-memory policy.
class ChatSelectionPersistence
{
public:
    virtual ~ChatSelectionPersistence() = default;

    virtual bool save_active_model(std::string_view id) = 0;
    virtual bool save_active_prompt(std::string_view id) = 0;
};

class RegistryChatSelectionPersistence final : public ChatSelectionPersistence
{
public:
    explicit RegistryChatSelectionPersistence(ck::config::OptionRegistry &registry) noexcept;

    bool save_active_model(std::string_view id) override;
    bool save_active_prompt(std::string_view id) override;

private:
    bool save(std::string_view key, std::string_view value);

    ck::config::OptionRegistry &registry_;
};

} // namespace ck::vision
