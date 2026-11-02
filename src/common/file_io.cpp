#include "ck/file_io.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
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

} // namespace ck::io
