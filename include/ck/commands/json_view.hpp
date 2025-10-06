#pragma once

#include <cstdint>

namespace ck::commands::json_view
{

inline constexpr std::uint16_t Find = 1000;
inline constexpr std::uint16_t FindNext = 1001;
inline constexpr std::uint16_t FindPrev = 1002;
inline constexpr std::uint16_t About = 1003;
inline constexpr std::uint16_t EndSearch = 1004;
inline constexpr std::uint16_t Level0 = 1100;
inline constexpr std::uint16_t Level1 = 1101;
inline constexpr std::uint16_t Level2 = 1102;
inline constexpr std::uint16_t Level3 = 1103;
inline constexpr std::uint16_t Level4 = 1104;
inline constexpr std::uint16_t Level5 = 1105;
inline constexpr std::uint16_t Level6 = 1106;
inline constexpr std::uint16_t Level7 = 1107;
inline constexpr std::uint16_t Level8 = 1108;
inline constexpr std::uint16_t Level9 = 1109;
inline constexpr std::uint16_t ReturnToLauncher = 1200;

} // namespace ck::commands::json_view

