#pragma once

#include <cstdint>

namespace ck::commands::config
{

inline constexpr std::uint16_t ReloadApps = 3000;
inline constexpr std::uint16_t EditApp = 3001;
inline constexpr std::uint16_t ResetApp = 3002;
inline constexpr std::uint16_t ClearApp = 3003;
inline constexpr std::uint16_t ExportApp = 3004;
inline constexpr std::uint16_t ImportApp = 3005;
inline constexpr std::uint16_t OpenConfigDir = 3006;
inline constexpr std::uint16_t About = 3007;

inline constexpr std::uint16_t OptionEdit = 3100;
inline constexpr std::uint16_t OptionResetValue = 3101;
inline constexpr std::uint16_t OptionResetAll = 3102;

inline constexpr std::uint16_t PatternAdd = 3200;
inline constexpr std::uint16_t PatternEdit = 3201;
inline constexpr std::uint16_t PatternDelete = 3202;

inline constexpr std::uint16_t ReturnToLauncher = 3300;

} // namespace ck::commands::config

