#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <string>
#include <type_traits>

namespace ck::find
{

template <typename Buffer>
void copyToArray(Buffer &dest, const char *src)
{
    if (!src)
        src = "";
    if (dest.empty())
        return;
    std::snprintf(dest.data(), dest.size(), "%s", src);
}

template <typename Buffer>
std::string bufferToString(const Buffer &buffer)
{
    auto it = std::find(buffer.begin(), buffer.end(), '\0');
    if (it != buffer.end())
        return std::string(buffer.data(), static_cast<std::size_t>(std::distance(buffer.begin(), it)));
    return std::string(buffer.data(), buffer.size());
}

template <typename Dest, typename Src>
    requires(!std::is_convertible_v<Src, const char *>)
void copyToArray(Dest &dest, const Src &src)
{
    const std::string value = bufferToString(src);
    copyToArray(dest, value.c_str());
}

} // namespace ck::find
