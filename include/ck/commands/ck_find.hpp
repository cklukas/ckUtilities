#pragma once

#include <cstdint>

namespace ck::commands::find
{

inline constexpr std::uint16_t NewSearch = 1000;
inline constexpr std::uint16_t LoadSpec = 1001;
inline constexpr std::uint16_t SaveSpec = 1002;
inline constexpr std::uint16_t ReturnToLauncher = 1003;
inline constexpr std::uint16_t About = 1004;
inline constexpr std::uint16_t BrowseStart = 1005;
inline constexpr std::uint16_t TextOptions = 1006;
inline constexpr std::uint16_t NamePathOptions = 1007;
inline constexpr std::uint16_t TimeFilters = 1008;
inline constexpr std::uint16_t SizeFilters = 1009;
inline constexpr std::uint16_t TypeFilters = 1010;
inline constexpr std::uint16_t PermissionOwnership = 1011;
inline constexpr std::uint16_t TraversalFilters = 1012;
inline constexpr std::uint16_t ActionOptions = 1013;
inline constexpr std::uint16_t DialogLoadSpec = 1014;
inline constexpr std::uint16_t DialogSaveSpec = 1015;

} // namespace ck::commands::find
