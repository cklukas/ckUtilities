#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>

namespace ck::find
{

template <std::size_t N>
void copyToArray(std::array<char, N> &dest, const char *src)
{
    if (!src)
        src = "";
    std::snprintf(dest.data(), dest.size(), "%s", src);
}

template <std::size_t N>
std::string bufferToString(const std::array<char, N> &buffer)
{
    auto it = std::find(buffer.begin(), buffer.end(), '\0');
    if (it != buffer.end())
        return std::string(buffer.data(), static_cast<std::size_t>(std::distance(buffer.begin(), it)));
    return std::string(buffer.data(), buffer.size());
}

template <std::size_t N, std::size_t M>
void copyToArray(std::array<char, N> &dest, const std::array<char, M> &src)
{
    const std::string value = bufferToString(src);
    copyToArray(dest, value.c_str());
}

} // namespace ck::find
