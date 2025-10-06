#pragma once

#include <cstdint>

namespace ck::commands::disk_usage
{

inline constexpr std::uint16_t ViewFiles = 2001;
inline constexpr std::uint16_t ViewFilesRecursive = 2002;
inline constexpr std::uint16_t ViewFileTypes = 2003;
inline constexpr std::uint16_t ViewFileTypesRecursive = 2004;
inline constexpr std::uint16_t ViewFilesForType = 2005;

inline constexpr std::uint16_t About = 2100;

inline constexpr std::uint16_t UnitAuto = 2200;
inline constexpr std::uint16_t UnitBytes = 2201;
inline constexpr std::uint16_t UnitKB = 2202;
inline constexpr std::uint16_t UnitMB = 2203;
inline constexpr std::uint16_t UnitGB = 2204;
inline constexpr std::uint16_t UnitTB = 2205;
inline constexpr std::uint16_t UnitBlocks = 2206;

inline constexpr std::uint16_t SortUnsorted = 2300;
inline constexpr std::uint16_t SortNameAsc = 2301;
inline constexpr std::uint16_t SortNameDesc = 2302;
inline constexpr std::uint16_t SortSizeDesc = 2303;
inline constexpr std::uint16_t SortSizeAsc = 2304;
inline constexpr std::uint16_t SortModifiedDesc = 2305;
inline constexpr std::uint16_t SortModifiedAsc = 2306;

inline constexpr std::uint16_t OptionFollowNever = 2400;
inline constexpr std::uint16_t OptionFollowCommandLine = 2401;
inline constexpr std::uint16_t OptionFollowAll = 2402;
inline constexpr std::uint16_t OptionToggleHardLinks = 2403;
inline constexpr std::uint16_t OptionToggleNodump = 2404;
inline constexpr std::uint16_t OptionToggleErrors = 2405;
inline constexpr std::uint16_t OptionToggleOneFs = 2406;
inline constexpr std::uint16_t OptionEditIgnores = 2407;
inline constexpr std::uint16_t OptionEditThreshold = 2408;
inline constexpr std::uint16_t OptionLoad = 2409;
inline constexpr std::uint16_t OptionSave = 2410;
inline constexpr std::uint16_t OptionSaveDefaults = 2411;

inline constexpr std::uint16_t PatternAdd = 2500;
inline constexpr std::uint16_t PatternEdit = 2501;
inline constexpr std::uint16_t PatternDelete = 2502;

inline constexpr std::uint16_t ReturnToLauncher = 2600;

} // namespace ck::commands::disk_usage

