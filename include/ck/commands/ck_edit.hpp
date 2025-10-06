#pragma once

#include <cstdint>

namespace ck::commands::edit
{

inline constexpr std::uint16_t cmToggleWrap = 3000;
inline constexpr std::uint16_t cmToggleMarkdownMode = 3001;
inline constexpr std::uint16_t cmHeading1 = 3010;
inline constexpr std::uint16_t cmHeading2 = 3011;
inline constexpr std::uint16_t cmHeading3 = 3012;
inline constexpr std::uint16_t cmHeading4 = 3013;
inline constexpr std::uint16_t cmHeading5 = 3014;
inline constexpr std::uint16_t cmHeading6 = 3015;
inline constexpr std::uint16_t cmClearHeading = 3016;
inline constexpr std::uint16_t cmMakeParagraph = 3017;
inline constexpr std::uint16_t cmInsertLineBreak = 3018;
inline constexpr std::uint16_t cmBold = 3020;
inline constexpr std::uint16_t cmItalic = 3021;
inline constexpr std::uint16_t cmBoldItalic = 3022;
inline constexpr std::uint16_t cmStrikethrough = 3023;
inline constexpr std::uint16_t cmInlineCode = 3024;
inline constexpr std::uint16_t cmCodeBlock = 3025;
inline constexpr std::uint16_t cmRemoveFormatting = 3026;
inline constexpr std::uint16_t cmToggleBlockQuote = 3030;
inline constexpr std::uint16_t cmToggleBulletList = 3031;
inline constexpr std::uint16_t cmToggleNumberedList = 3032;
inline constexpr std::uint16_t cmConvertTaskList = 3033;
inline constexpr std::uint16_t cmToggleTaskCheckbox = 3034;
inline constexpr std::uint16_t cmIncreaseIndent = 3035;
inline constexpr std::uint16_t cmDecreaseIndent = 3036;
inline constexpr std::uint16_t cmDefinitionList = 3037;
inline constexpr std::uint16_t cmInsertLink = 3040;
inline constexpr std::uint16_t cmInsertReferenceLink = 3041;
inline constexpr std::uint16_t cmAutoLinkSelection = 3042;
inline constexpr std::uint16_t cmInsertImage = 3043;
inline constexpr std::uint16_t cmInsertFootnote = 3044;
inline constexpr std::uint16_t cmInsertHorizontalRule = 3045;
inline constexpr std::uint16_t cmEscapeSelection = 3046;
inline constexpr std::uint16_t cmInsertTable = 3050;
inline constexpr std::uint16_t cmTableInsertRowAbove = 3051;
inline constexpr std::uint16_t cmTableInsertRowBelow = 3052;
inline constexpr std::uint16_t cmTableDeleteRow = 3053;
inline constexpr std::uint16_t cmTableInsertColumnBefore = 3054;
inline constexpr std::uint16_t cmTableInsertColumnAfter = 3055;
inline constexpr std::uint16_t cmTableDeleteColumn = 3056;
inline constexpr std::uint16_t cmTableDeleteTable = 3057;
inline constexpr std::uint16_t cmTableAlignDefault = 3058;
inline constexpr std::uint16_t cmTableAlignLeft = 3059;
inline constexpr std::uint16_t cmTableAlignCenter = 3060;
inline constexpr std::uint16_t cmTableAlignRight = 3061;
inline constexpr std::uint16_t cmTableAlignNumber = 3062;
inline constexpr std::uint16_t cmReflowParagraphs = 3070;
inline constexpr std::uint16_t cmFormatDocument = 3071;
inline constexpr std::uint16_t cmToggleSmartList = 3080;
inline constexpr std::uint16_t cmAbout = 3090;
inline constexpr std::uint16_t cmReturnToLauncher = 3091;

} // namespace ck::commands::edit

