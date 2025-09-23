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

} // namespace ck::find

