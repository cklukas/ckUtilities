#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

namespace ck::io
{

enum class WriteMode
{
    Truncate,
    Append
};

inline constexpr std::size_t kReadAll = std::numeric_limits<std::size_t>::max();

// Reads up to max_bytes from path. kReadAll reads the complete file.
// On Windows this is a wide-character, extended-length handle operation.
bool read_file(const std::filesystem::path &path, std::string &contents,
               std::size_t max_bytes = kReadAll);

// Writes bytes to path using a wide-character, extended-length handle on
// Windows. The caller owns directory creation and any atomic-publish policy.
bool write_file(const std::filesystem::path &path, std::string_view contents,
                WriteMode mode = WriteMode::Truncate);

} // namespace ck::io
