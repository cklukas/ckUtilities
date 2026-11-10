#include "ck/file_io.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ck::io
{
namespace
{

#if defined(_WIN32)
class Handle
{
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~Handle()
    {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }

    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE value_;
};

bool starts_with(std::wstring_view value, std::wstring_view prefix) noexcept
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

std::optional<std::wstring> absolute_windows_path(const std::filesystem::path &path)
{
    std::wstring value = path.native();
    if (value.empty())
        return std::nullopt;
    if (starts_with(value, L"\\\\?\\"))
        return value;

    std::vector<wchar_t> buffer(32768);
    for (;;)
    {
        const DWORD length = GetFullPathNameW(value.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length == 0)
            return std::nullopt;
        if (length < buffer.size() - 1)
            return std::wstring(buffer.data(), length);
        if (length >= 32768 * 16)
            return std::nullopt;
        const std::size_t required = static_cast<std::size_t>(length) + 2;
        const std::size_t grown = buffer.size() > std::numeric_limits<std::size_t>::max() / 2
                                      ? required
                                      : buffer.size() * 2;
        buffer.resize(std::max(required, grown));
    }
}

std::optional<std::wstring> extended_windows_path(const std::filesystem::path &path)
{
    const auto absolute = absolute_windows_path(path);
    if (!absolute)
        return std::nullopt;
    if (starts_with(*absolute, L"\\\\?\\"))
        return absolute;
    if (starts_with(*absolute, L"\\\\"))
        return L"\\\\?\\UNC\\" + absolute->substr(2);
    if (absolute->size() >= 3 && (*absolute)[1] == L':' && (*absolute)[2] == L'\\')
        return L"\\\\?\\" + *absolute;
    return std::nullopt;
}

bool read_windows(const std::filesystem::path &path, std::string &contents, std::size_t max_bytes)
{
    const auto native = extended_windows_path(path);
    if (!native)
        return false;
    Handle file(CreateFileW(native->c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
        return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0)
        return false;
    const auto file_size = static_cast<unsigned long long>(size.QuadPart);
    const auto limit = static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max());
    if (file_size > limit)
        return false;
    const std::size_t requested = std::min<std::size_t>(static_cast<std::size_t>(file_size), max_bytes);
    contents.clear();
    contents.resize(requested);
    std::size_t offset = 0;
    while (offset < requested)
    {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(requested - offset, 1024 * 1024));
        DWORD read = 0;
        if (!ReadFile(file.get(), contents.data() + offset, chunk, &read, nullptr))
        {
            contents.clear();
            return false;
        }
        if (read == 0)
            break;
        offset += read;
    }
    contents.resize(offset);
    return true;
}

bool write_windows(const std::filesystem::path &path, std::string_view contents, WriteMode mode)
{
    const auto native = extended_windows_path(path);
    if (!native)
        return false;
    const bool append = mode == WriteMode::Append;
    const DWORD access = append ? FILE_APPEND_DATA : GENERIC_WRITE;
    Handle file(CreateFileW(native->c_str(), access,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file)
        return false;

    std::size_t offset = 0;
    while (offset < contents.size())
    {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(contents.size() - offset, 1024 * 1024));
        DWORD written = 0;
        if (!WriteFile(file.get(), contents.data() + offset, chunk, &written, nullptr) || written == 0)
            return false;
        offset += written;
    }
    return FlushFileBuffers(file.get()) != 0;
}

std::optional<std::wstring> plain_absolute_windows_path(const std::filesystem::path &path)
{
    const auto absolute = absolute_windows_path(path);
    if (!absolute)
        return std::nullopt;
    if (starts_with(*absolute, L"\\\\?\\UNC\\"))
        return L"\\\\" + absolute->substr(8);
    if (starts_with(*absolute, L"\\\\?\\"))
        return absolute->substr(4);
    return absolute;
}

std::size_t windows_root_length(std::wstring_view path) noexcept
{
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\')
        return 3;
    if (!starts_with(path, L"\\\\"))
        return 0;
    const auto server = path.find(L'\\', 2);
    if (server == std::wstring_view::npos || server == 2)
        return 0;
    const auto share = path.find(L'\\', server + 1);
    if (share == server + 1)
        return 0;
    return share == std::wstring_view::npos ? path.size() : share;
}

bool is_directory_windows(std::wstring_view extended) noexcept
{
    const DWORD attributes = GetFileAttributesW(std::wstring(extended).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool create_directories_windows(const std::filesystem::path &path)
{
    const auto plain = plain_absolute_windows_path(path);
    if (!plain)
        return false;
    const auto root = windows_root_length(*plain);
    if (root == 0)
        return false;
    for (std::size_t end = root; end <= plain->size(); ++end)
    {
        if (end != plain->size() && (*plain)[end] != L'\\')
            continue;
        const std::wstring prefix = plain->substr(0, end);
        const auto extended = extended_windows_path(std::filesystem::path(prefix));
        if (!extended)
            return false;
        if (is_directory_windows(*extended))
            continue;
        if (CreateDirectoryW(extended->c_str(), nullptr) == 0 && !is_directory_windows(*extended))
            return false;
    }
    return true;
}

std::vector<std::filesystem::path> list_directories_windows(const std::filesystem::path &path)
{
    const auto native = extended_windows_path(path);
    if (!native)
        return {};
    const std::wstring pattern = *native + L"\\*";
    WIN32_FIND_DATAW data{};
    Handle enumeration(FindFirstFileW(pattern.c_str(), &data));
    if (!enumeration)
        return {};
    std::vector<std::filesystem::path> result;
    for (;;)
    {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L".." && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            result.emplace_back(path / std::wstring(name));
        if (FindNextFileW(enumeration.get(), &data) == 0)
            break;
    }
    return GetLastError() == ERROR_NO_MORE_FILES ? result : std::vector<std::filesystem::path>{};
}

std::vector<std::filesystem::path> list_regular_files_windows(const std::filesystem::path &path)
{
    const auto native = extended_windows_path(path);
    if (!native)
        return {};
    const std::wstring pattern = *native + L"\\*";
    WIN32_FIND_DATAW data{};
    Handle enumeration(FindFirstFileW(pattern.c_str(), &data));
    if (!enumeration)
        return {};
    std::vector<std::filesystem::path> result;
    for (;;)
    {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L".." && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
            result.emplace_back(path / std::wstring(name));
        if (FindNextFileW(enumeration.get(), &data) == 0)
            break;
    }
    return GetLastError() == ERROR_NO_MORE_FILES ? result : std::vector<std::filesystem::path>{};
}

bool remove_tree_windows(const std::filesystem::path &path)
{
    const auto native = extended_windows_path(path);
    if (!native)
        return false;
    const DWORD attributes = GetFileAttributesW(native->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return DeleteFileW(native->c_str()) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return RemoveDirectoryW(native->c_str()) != 0 || GetLastError() == ERROR_PATH_NOT_FOUND;

    const std::wstring pattern = *native + L"\\*";
    WIN32_FIND_DATAW data{};
    Handle enumeration(FindFirstFileW(pattern.c_str(), &data));
    if (!enumeration)
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    for (;;)
    {
        const std::wstring_view name(data.cFileName);
        if (name != L"." && name != L".." && !remove_tree_windows(path / std::wstring(name)))
            return false;
        if (FindNextFileW(enumeration.get(), &data) == 0)
            break;
    }
    if (GetLastError() != ERROR_NO_MORE_FILES)
        return false;
    return RemoveDirectoryW(native->c_str()) != 0 || GetLastError() == ERROR_PATH_NOT_FOUND;
}

bool remove_regular_windows(const std::filesystem::path &path)
{
    const auto native = extended_windows_path(path);
    if (!native)
        return false;
    const DWORD attributes = GetFileAttributesW(native->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return false;
    return DeleteFileW(native->c_str()) != 0;
}

bool is_regular_windows(const std::filesystem::path &path) noexcept
{
    const auto native = extended_windows_path(path);
    if (!native)
        return false;
    const DWORD attributes = GetFileAttributesW(native->c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool copy_file_windows(const std::filesystem::path &source, const std::filesystem::path &destination)
{
    const auto native_source = extended_windows_path(source);
    const auto native_destination = extended_windows_path(destination);
    if (!native_source || !native_destination || !is_regular_windows(source))
        return false;

    const DWORD destination_attributes = GetFileAttributesW(native_destination->c_str());
    if (destination_attributes != INVALID_FILE_ATTRIBUTES &&
        (destination_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return false;
    if (destination_attributes != INVALID_FILE_ATTRIBUTES &&
        (destination_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    return CopyFileW(native_source->c_str(), native_destination->c_str(), FALSE) != 0;
}

bool remove_windows(const std::filesystem::path &path)
{
    const auto native = extended_windows_path(path);
    if (!native)
        return false;
    if (DeleteFileW(native->c_str()) != 0)
        return true;
    return GetLastError() == ERROR_FILE_NOT_FOUND;
}
#endif

} // namespace

bool read_file(const std::filesystem::path &path, std::string &contents, std::size_t max_bytes)
{
#if defined(_WIN32)
    return read_windows(path, contents, max_bytes);
#else
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    contents.clear();
    if (max_bytes == kReadAll)
    {
        input.seekg(0, std::ios::end);
        const auto end = input.tellg();
        if (end < 0)
            return false;
        contents.resize(static_cast<std::size_t>(end));
        input.seekg(0, std::ios::beg);
    }
    else
    {
        contents.resize(max_bytes);
    }
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(input.gcount()));
    return !input.bad();
#endif
}

bool write_file(const std::filesystem::path &path, std::string_view contents, WriteMode mode)
{
#if defined(_WIN32)
    return write_windows(path, contents, mode);
#else
    std::ofstream output(path, std::ios::binary |
                                   (mode == WriteMode::Append ? std::ios::app : std::ios::trunc));
    if (!output)
        return false;
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    return static_cast<bool>(output);
#endif
}

bool create_directories(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return create_directories_windows(path);
#else
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error && std::filesystem::is_directory(path, error) && !error;
#endif
}

std::vector<std::filesystem::path> list_directories(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return list_directories_windows(path);
#else
    std::error_code error;
    std::vector<std::filesystem::path> result;
    for (const auto &entry : std::filesystem::directory_iterator(path, error))
    {
        if (error)
            return {};
        if (entry.is_directory(error))
            result.push_back(entry.path());
        if (error)
            return {};
    }
    return error ? std::vector<std::filesystem::path>{} : result;
#endif
}

std::vector<std::filesystem::path> list_regular_files(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return list_regular_files_windows(path);
#else
    std::error_code error;
    std::vector<std::filesystem::path> result;
    for (const auto &entry : std::filesystem::directory_iterator(path, error))
    {
        if (error)
            return {};
        if (entry.is_regular_file(error))
            result.push_back(entry.path());
        if (error)
            return {};
    }
    return error ? std::vector<std::filesystem::path>{} : result;
#endif
}

bool remove_tree(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return remove_tree_windows(path);
#else
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return !error;
#endif
}

bool remove_regular_file(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return remove_regular_windows(path);
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_regular_file(status))
        return false;
    return std::filesystem::remove(path, error) && !error;
#endif
}

bool is_regular_file(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return is_regular_windows(path);
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status);
#endif
}

bool copy_file(const std::filesystem::path &source, const std::filesystem::path &destination)
{
#if defined(_WIN32)
    return copy_file_windows(source, destination);
#else
    std::error_code error;
    return std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error) &&
           !error;
#endif
}

bool remove_file(const std::filesystem::path &path)
{
#if defined(_WIN32)
    return remove_windows(path);
#else
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    return !error && (removed || !std::filesystem::exists(path, error));
#endif
}

} // namespace ck::io
