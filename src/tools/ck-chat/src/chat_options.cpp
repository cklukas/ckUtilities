#include "chat_options.hpp"

#include <string>

namespace ck::chat
{

void registerChatOptions(config::OptionRegistry &registry)
{
  registry.registerOption({kOptionShowThinking, config::OptionKind::Boolean,
                           config::OptionValue(false), "Show Thinking",
                           "Display hidden thinking traces from the model."});
  registry.registerOption({kOptionShowAnalysis, config::OptionKind::Boolean,
                           config::OptionValue(false), "Show Analysis",
                           "Display analysis messages produced by the model."});
  registry.registerOption({kOptionParseMarkdownLinks, config::OptionKind::Boolean,
                           config::OptionValue(false), "Parse Markdown Links",
                           "Render Markdown links using terminal-supported hyperlinks."});
  registry.registerOption({kOptionActiveModelId, config::OptionKind::String,
                           config::OptionValue(std::string()), "Active Model ID",
                           "Identifier of the downloaded model to activate on startup."});
  registry.registerOption({kOptionActivePromptId, config::OptionKind::String,
                           config::OptionValue(std::string()), "Active Prompt ID",
                           "Identifier of the system prompt to activate on startup."});
}

} // namespace ck::chat

