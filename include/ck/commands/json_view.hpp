#pragma once

#include <cstdint>

#include "ck/commands/common.hpp"

namespace ck::commands::json_view
{

inline constexpr std::uint16_t Find = 4000;
inline constexpr std::uint16_t FindNext = 4001;
inline constexpr std::uint16_t FindPrev = 4002;
inline constexpr std::uint16_t About = ck::commands::common::About;
inline constexpr std::uint16_t EndSearch = 4004;
inline constexpr std::uint16_t ReturnToLauncher = ck::commands::common::ReturnToLauncher;
inline constexpr std::uint16_t Level0 = 4010;
inline constexpr std::uint16_t Level1 = 4011;
inline constexpr std::uint16_t Level2 = 4012;
inline constexpr std::uint16_t Level3 = 4013;
inline constexpr std::uint16_t Level4 = 4014;
inline constexpr std::uint16_t Level5 = 4015;
inline constexpr std::uint16_t Level6 = 4016;
inline constexpr std::uint16_t Level7 = 4017;
inline constexpr std::uint16_t Level8 = 4018;
inline constexpr std::uint16_t Level9 = 4019;

} // namespace ck::commands::json_view

