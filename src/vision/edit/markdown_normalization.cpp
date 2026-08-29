#include "markdown_normalization.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ck::vision
{
namespace
{
struct ParsedLine
{
    std::string text;
    bool protected_content = false;
};

struct Fence
{
    char marker = '\0';
    std::size_t width = 0;
};

std::optional<Fence> opening_fence(const std::string &line)
{
    std::size_t begin = 0;
    while (begin < line.size() && begin < 3 && line[begin] == ' ')
        ++begin;
    if (begin == line.size() || (line[begin] != '`' && line[begin] != '~'))
        return std::nullopt;

    const char marker = line[begin];
    std::size_t end = begin;
    while (end < line.size() && line[end] == marker)
        ++end;
    if (end - begin < 3)
        return std::nullopt;
    return Fence{marker, end - begin};
}

bool closing_fence(const std::string &line, Fence fence)
{
    const auto candidate = opening_fence(line);
    if (!candidate || candidate->marker != fence.marker || candidate->width < fence.width)
        return false;
    std::size_t offset = 0;
    while (offset < line.size() && offset < 3 && line[offset] == ' ')
        ++offset;
    while (offset < line.size() && line[offset] == fence.marker)
        ++offset;
    while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t'))
        ++offset;
    return offset == line.size();
}

bool indented_code_line(const std::string &line)
{
    return line.starts_with('\t') || (line.size() >= 4 && line.substr(0, 4) == "    ");
}

void normalise_ordinary_line(std::string &line)
{
    std::size_t suffix = line.size();
    while (suffix > 0 && (line[suffix - 1] == ' ' || line[suffix - 1] == '\t'))
        --suffix;
    if (suffix == line.size())
        return;

    const std::size_t trailing = line.size() - suffix;
    const bool only_spaces = line.find('\t', suffix) == std::string::npos;
    if (only_spaces && trailing >= 2)
        line.resize(suffix + 2); // Markdown's intentional two-space hard break.
    else
        line.resize(suffix);
}

void append_line(std::string &output, const std::string &line)
{
    output += line;
    output.push_back('\n');
}
} // namespace

std::string normalise_markdown_whitespace(std::string_view text)
{
    if (text.empty())
        return {};

    std::vector<std::string> lines;
    lines.reserve(32);

    std::size_t line_begin = 0;
    while (line_begin < text.size())
    {
        const std::size_t line_end = text.find('\n', line_begin);
        const std::size_t end = line_end == std::string_view::npos ? text.size() : line_end;
        std::string line(text.substr(line_begin, end - line_begin));
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        if (line_end == std::string_view::npos)
            break;
        line_begin = line_end + 1;
    }

    std::vector<ParsedLine> parsed;
    parsed.reserve(lines.size());
    std::optional<Fence> fence;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const std::string &line = lines[index];
        const bool previous_indented = index > 0 && indented_code_line(lines[index - 1]);
        const bool next_indented = index + 1 < lines.size() && indented_code_line(lines[index + 1]);
        const bool is_blank = line.empty();
        const bool protected_content = fence.has_value() || indented_code_line(line) ||
                                       (is_blank && (previous_indented || next_indented));

        ParsedLine parsed_line{line, protected_content};
        if (fence)
        {
            if (closing_fence(line, *fence))
                fence.reset();
        }
        else if (const auto opening = opening_fence(line))
        {
            fence = opening;
        }
        if (!parsed_line.protected_content)
            normalise_ordinary_line(parsed_line.text);
        parsed.push_back(std::move(parsed_line));
    }

    while (!parsed.empty() && parsed.back().text.empty() && !parsed.back().protected_content)
        parsed.pop_back();

    std::string output;
    output.reserve(text.size() + 1);
    if (parsed.empty())
    {
        output.push_back('\n');
        return output;
    }
    for (const ParsedLine &line : parsed)
        append_line(output, line.text);
    return output;
}

} // namespace ck::vision
