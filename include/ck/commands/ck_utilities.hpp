#pragma once

#include <cstdint>

namespace ck::commands::utilities
{

inline constexpr std::uint16_t LaunchTool = 6000;
inline constexpr std::uint16_t NewLauncher = 6001;
inline constexpr std::uint16_t ShowCalendar = 6002;
inline constexpr std::uint16_t ShowAsciiTable = 6003;
inline constexpr std::uint16_t ShowCalculator = 6004;
inline constexpr std::uint16_t ToggleEventViewer = 6005;
inline constexpr std::uint16_t CalcButtonCommand = 6100;
inline constexpr std::uint16_t AsciiSelectionChanged = 6101;
inline constexpr std::uint16_t FindEventViewer = 6102;

} // namespace ck::commands::utilities

