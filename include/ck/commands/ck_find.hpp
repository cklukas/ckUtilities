#pragma once

#include <cstdint>

#include "ck/commands/common.hpp"

namespace ck::commands::find
{

inline constexpr std::uint16_t NewSearch = 5000;
inline constexpr std::uint16_t LoadSpec = 5001;
inline constexpr std::uint16_t SaveSpec = 5002;
inline constexpr std::uint16_t ReturnToLauncher = ck::commands::common::ReturnToLauncher;
inline constexpr std::uint16_t About = ck::commands::common::About;
inline constexpr std::uint16_t BrowseStart = 5005;
inline constexpr std::uint16_t TextOptions = 5006;
inline constexpr std::uint16_t NamePathOptions = 5007;
inline constexpr std::uint16_t TimeFilters = 5008;
inline constexpr std::uint16_t SizeFilters = 5009;
inline constexpr std::uint16_t TypeFilters = 5010;
inline constexpr std::uint16_t PermissionOwnership = 5011;
inline constexpr std::uint16_t TraversalFilters = 5012;
inline constexpr std::uint16_t ActionOptions = 5013;
inline constexpr std::uint16_t DialogLoadSpec = 5014;
inline constexpr std::uint16_t DialogSaveSpec = 5015;
inline constexpr std::uint16_t TogglePreview = 5016;
inline constexpr std::uint16_t TabQuickStart = 5020;
inline constexpr std::uint16_t TabContentNames = 5021;
inline constexpr std::uint16_t TabDatesSizes = 5022;
inline constexpr std::uint16_t TabTypesOwnership = 5023;
inline constexpr std::uint16_t TabTraversal = 5024;
inline constexpr std::uint16_t TabActions = 5025;
inline constexpr std::uint16_t CopySearchToName = 5026;
inline constexpr std::uint16_t ClearNameFilters = 5027;

} // namespace ck::commands::find
