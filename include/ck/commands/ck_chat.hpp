#pragma once

#include <cstdint>

#include "ck/commands/common.hpp"

namespace ck::commands::chat
{

inline constexpr std::uint16_t NewChat = 1000;
inline constexpr std::uint16_t ReturnToLauncher = ck::commands::common::ReturnToLauncher;
inline constexpr std::uint16_t About = ck::commands::common::About;
inline constexpr std::uint16_t SendPrompt = 1003;
inline constexpr std::uint16_t CopyLastResponse = 1004;
inline constexpr std::uint16_t CopyResponseBase = 2000;
inline constexpr std::uint16_t SelectModel1 = 1100;
inline constexpr std::uint16_t SelectModel2 = 1101;
inline constexpr std::uint16_t SelectModel3 = 1102;
inline constexpr std::uint16_t SelectModel4 = 1103;
inline constexpr std::uint16_t SelectModel5 = 1104;
inline constexpr std::uint16_t SelectModel6 = 1105;
inline constexpr std::uint16_t SelectModel7 = 1106;
inline constexpr std::uint16_t SelectModel8 = 1107;
inline constexpr std::uint16_t SelectModel9 = 1108;
inline constexpr std::uint16_t SelectModel10 = 1109;
inline constexpr std::uint16_t ManageModels = 1110;
inline constexpr std::uint16_t DownloadModel = 1111;
inline constexpr std::uint16_t ActivateModel = 1112;
inline constexpr std::uint16_t DeactivateModel = 1113;
inline constexpr std::uint16_t DeleteModel = 1114;
inline constexpr std::uint16_t RefreshModels = 1115;
inline constexpr std::uint16_t CancelDownload = 1116;
inline constexpr std::uint16_t CopyFullConversation = 1117;
inline constexpr std::uint16_t NoOp = 1117;
inline constexpr std::uint16_t ApplyRuntimeSettings = 1118;
inline constexpr std::uint16_t SelectPromptBase = 1200;
inline constexpr std::uint16_t ManagePrompts = 1210;
inline constexpr std::uint16_t ShowThinking = 1300;
inline constexpr std::uint16_t HideThinking = 1301;
inline constexpr std::uint16_t ShowAnalysis = 1302;
inline constexpr std::uint16_t HideAnalysis = 1303;
inline constexpr std::uint16_t ToggleParseMarkdownLinks = 1304;

} // namespace ck::commands::chat
