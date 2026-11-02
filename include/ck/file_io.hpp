#pragma once

#include <cstddef>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

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

// Creates path and all missing parent directories. On Windows this uses
// wide-character, extended-length directory handles at the final boundary.
bool create_directories(const std::filesystem::path &path);

// Lists immediate child directories without following the final entry. An
// empty result represents either no matching children or an enumeration
// failure; callers that need the distinction should use a higher-level native
// filesystem adapter.
std::vector<std::filesystem::path> list_directories(const std::filesystem::path &path);

// Lists immediate ordinary files without following the final entry. Reparse
// points/symlinks are excluded on every platform so callers can safely use
// the result for profile or cache discovery.
std::vector<std::filesystem::path> list_regular_files(const std::filesystem::path &path);

// Removes a file or directory tree. On Windows this is a wide-character,
// extended-length traversal and does not recurse through reparse points.
bool remove_tree(const std::filesystem::path &path);

// Removes only an ordinary regular file. Directories and reparse/symlink
// entries are refused so callers cannot accidentally turn a path cleanup into
// recursive or link-following deletion.
bool remove_regular_file(const std::filesystem::path &path);

bool is_regular_file(const std::filesystem::path &path);

// Copies an ordinary file, replacing an existing ordinary destination. On
// Windows this is a wide-character, extended-length operation.
bool copy_file(const std::filesystem::path &source, const std::filesystem::path &destination);

bool remove_file(const std::filesystem::path &path);

} // namespace ck::io
