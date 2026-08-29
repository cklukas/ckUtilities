#include "markdown_normalization.hpp"

#include <string>

namespace ck::vision
{
namespace
{
bool has_markdown_hard_break(const std::string &line)
{
    if (line.size() < 2)
        return false;
    return line[line.size() - 1] == ' ' && line[line.size() - 2] == ' ';
}

void append_normalised_line(std::string &output, std::string line)
{
    if (!has_markdown_hard_break(line))
    {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
    }
    output += line;
    output.push_back('\n');
}
} // namespace

std::string normalise_markdown_whitespace(std::string_view text)
{
    std::string output;
    output.reserve(text.size() + 1);

    std::size_t line_begin = 0;
    while (line_begin < text.size())
    {
        const std::size_t line_end = text.find('\n', line_begin);
        const std::size_t end = line_end == std::string_view::npos ? text.size() : line_end;
        std::string line(text.substr(line_begin, end - line_begin));
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        append_normalised_line(output, std::move(line));
        if (line_end == std::string_view::npos)
            break;
        line_begin = line_end + 1;
    }

    while (output.size() > 1 && output.back() == '\n' && output[output.size() - 2] == '\n')
        output.pop_back();
    return output;
}

} // namespace ck::vision
