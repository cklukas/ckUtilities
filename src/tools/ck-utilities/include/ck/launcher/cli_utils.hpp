#pragma once

#include "ck/app_info.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ck::launcher
{

inline std::string quoteArgument(std::string_view value)
{
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (char ch : value)
    {
        if (ch == '\'')
            quoted.append("'\\''");
        else
            quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

inline std::filesystem::path resolveToolDirectory(const char *argv0)
{
    std::filesystem::path base = std::filesystem::current_path();
    if (!argv0 || !argv0[0])
        return base;

    std::filesystem::path candidate = argv0;
    if (!candidate.is_absolute())
        candidate = std::filesystem::current_path() / candidate;

    candidate = candidate.lexically_normal();

    if (!candidate.has_parent_path())
        return base;
    return candidate.parent_path();
}

inline std::optional<std::filesystem::path> locateProgramPath(const std::filesystem::path &toolDirectory,
                                                              const ck::appinfo::ToolInfo &info)
{
    std::filesystem::path programPath = (toolDirectory / std::filesystem::path(info.executable)).lexically_normal();

    std::error_code existsEc;
    if (!std::filesystem::exists(programPath, existsEc))
        return std::nullopt;
    return programPath;
}

inline std::vector<std::string> wrapText(std::string_view text, int width)
{
    std::vector<std::string> lines;
    if (width <= 0)
    {
        if (!text.empty())
            lines.emplace_back(text);
        return lines;
    }

    std::string currentLine;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        unsigned char ch = static_cast<unsigned char>(text[pos]);
        if (ch == '\r')
        {
            ++pos;
            continue;
        }
        if (ch == '\n')
        {
            if (!currentLine.empty())
            {
                lines.push_back(currentLine);
                currentLine.clear();
            }
            else if (lines.empty() || !lines.back().empty())
            {
                lines.emplace_back();
            }
            ++pos;
            continue;
        }
        if (std::isspace(ch))
        {
            ++pos;
            continue;
        }

        std::size_t wordEnd = pos;
        while (wordEnd < text.size())
        {
            unsigned char wc = static_cast<unsigned char>(text[wordEnd]);
            if (wc == '\n' || std::isspace(wc))
                break;
            ++wordEnd;
        }

        std::string_view word = text.substr(pos, wordEnd - pos);
        pos = wordEnd;

        if (static_cast<int>(word.size()) >= width)
        {
            if (!currentLine.empty())
            {
                lines.push_back(currentLine);
                currentLine.clear();
            }
            std::size_t offset = 0;
            while (offset < word.size())
            {
                std::size_t chunkLen = std::min<std::size_t>(width, word.size() - offset);
                lines.emplace_back(word.substr(offset, chunkLen));
                offset += chunkLen;
            }
            continue;
        }

        if (currentLine.empty())
        {
            currentLine.assign(word);
        }
        else if (static_cast<int>(currentLine.size() + 1 + word.size()) <= width)
        {
            currentLine.push_back(' ');
            currentLine.append(word);
        }
        else
        {
            lines.push_back(currentLine);
            currentLine.assign(word);
        }
    }

    if (!currentLine.empty())
        lines.push_back(currentLine);

    return lines;
}

} // namespace ck::launcher
