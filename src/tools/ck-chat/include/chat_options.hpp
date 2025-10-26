#pragma once

#include "ck/options.hpp"

namespace ck::chat
{

inline constexpr char kOptionParseMarkdownLinks[] = "parseMarkdownLinks";
inline constexpr char kOptionShowThinking[] = "showThinking";
inline constexpr char kOptionShowAnalysis[] = "showAnalysis";
inline constexpr char kOptionActiveModelId[] = "activeModelId";
inline constexpr char kOptionActivePromptId[] = "activePromptId";

void registerChatOptions(config::OptionRegistry &registry);

} // namespace ck::chat

