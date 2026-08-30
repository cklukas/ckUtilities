#include "ck/edit/markdown_transformations.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace ck::edit
{
namespace
{

bool valid_range(std::string_view source, MarkdownByteRange range) noexcept
{
    return range.begin <= range.end && range.end <= source.size();
}

bool has_non_whitespace(std::string_view value) noexcept
{
    return std::any_of(value.begin(), value.end(), [](char character) {
        return character != ' ' && character != '\t' && character != '\n' && character != '\r';
    });
}

std::size_t marker_run_before(std::string_view source, std::size_t offset, char marker) noexcept
{
    std::size_t begin = offset;
    while (begin > 0 && source[begin - 1] == marker)
        --begin;
    return offset - begin;
}

std::size_t marker_run_after(std::string_view source, std::size_t offset, char marker) noexcept
{
    std::size_t end = offset;
    while (end < source.size() && source[end] == marker)
        ++end;
    return end - offset;
}

bool surrounded_by_exact_marker(std::string_view source,
                                MarkdownByteRange selection,
                                std::string_view marker) noexcept
{
    if (selection.begin < marker.size() || selection.end + marker.size() > source.size())
        return false;
    if (source.substr(selection.begin - marker.size(), marker.size()) != marker ||
        source.substr(selection.end, marker.size()) != marker)
        return false;

    const char marker_character = marker.front();
    return marker_run_before(source, selection.begin, marker_character) == marker.size() &&
           marker_run_after(source, selection.end, marker_character) == marker.size();
}

std::optional<MarkdownTransformEdit> remove_included_marker(MarkdownByteRange selection,
                                                             std::string_view selected,
                                                             std::string_view marker)
{
    if (selected.size() <= marker.size() * 2U || !selected.starts_with(marker) || !selected.ends_with(marker))
        return std::nullopt;

    const std::string inner(selected.substr(marker.size(), selected.size() - marker.size() * 2U));
    if (!has_non_whitespace(inner))
        return std::nullopt;
    return MarkdownTransformEdit{
        .replaced = selection,
        .replacement = inner,
        .selection = {selection.begin, selection.begin + inner.size()},
    };
}

std::size_t longest_backtick_run(std::string_view value) noexcept
{
    std::size_t longest = 0;
    std::size_t current = 0;
    for (const char character : value)
    {
        if (character == '`')
        {
            ++current;
            longest = std::max(longest, current);
        }
        else
        {
            current = 0;
        }
    }
    return longest;
}

std::size_t line_start(std::string_view source, std::size_t offset) noexcept
{
    const std::size_t previous_newline = source.rfind('\n', offset == 0 ? 0 : offset - 1U);
    return previous_newline == std::string_view::npos ? 0 : previous_newline + 1U;
}

std::size_t line_end(std::string_view source, std::size_t offset) noexcept
{
    const std::size_t next_newline = source.find('\n', offset);
    return next_newline == std::string_view::npos ? source.size() : next_newline;
}

struct Fence
{
    char marker = '\0';
    std::size_t width = 0;
};

std::optional<Fence> fence_at(std::string_view line) noexcept
{
    std::size_t offset = 0;
    while (offset < line.size() && offset < 3U && line[offset] == ' ')
        ++offset;
    if (offset == line.size() || (line[offset] != '`' && line[offset] != '~'))
        return std::nullopt;

    const char marker = line[offset];
    std::size_t end = offset;
    while (end < line.size() && line[end] == marker)
        ++end;
    if (end - offset < 3U)
        return std::nullopt;
    return Fence{marker, end - offset};
}

bool fence_closes(std::string_view line, Fence fence) noexcept
{
    const auto candidate = fence_at(line);
    if (!candidate || candidate->marker != fence.marker || candidate->width < fence.width)
        return false;

    std::size_t offset = 0;
    while (offset < line.size() && offset < 3U && line[offset] == ' ')
        ++offset;
    while (offset < line.size() && line[offset] == fence.marker)
        ++offset;
    while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t' || line[offset] == '\r'))
        ++offset;
    return offset == line.size();
}

bool indented_code(std::string_view line) noexcept
{
    return line.starts_with('\t') || line.starts_with("    ");
}

std::optional<Fence> fence_before(std::string_view source, std::size_t offset)
{
    std::optional<Fence> active;
    for (std::size_t begin = 0; begin < offset;)
    {
        const std::size_t end = source.find('\n', begin);
        const std::size_t line_end_offset = end == std::string_view::npos ? source.size() : end;
        if (line_end_offset >= offset)
            break;

        const std::string_view line = source.substr(begin, line_end_offset - begin);
        if (active)
        {
            if (fence_closes(line, *active))
                active.reset();
        }
        else if (const auto opening = fence_at(line))
        {
            active = opening;
        }

        if (end == std::string_view::npos)
            break;
        begin = end + 1U;
    }
    return active;
}

std::size_t heading_indent(std::string_view line) noexcept
{
    std::size_t indent = 0;
    while (indent < line.size() && indent < 4U && line[indent] == ' ')
        ++indent;
    return indent;
}

struct Heading
{
    int level = 0;
    std::size_t content = 0;
};

std::optional<Heading> heading_at(std::string_view line, std::size_t indent) noexcept
{
    std::size_t marker_end = indent;
    while (marker_end < line.size() && marker_end - indent < 7U && line[marker_end] == '#')
        ++marker_end;
    const std::size_t count = marker_end - indent;
    if (count == 0 || count > 6 || (marker_end < line.size() && line[marker_end] != ' ' && line[marker_end] != '\t'))
        return std::nullopt;

    std::size_t content = marker_end;
    while (content < line.size() && (line[content] == ' ' || line[content] == '\t'))
        ++content;
    return Heading{static_cast<int>(count), content};
}

std::string transform_heading_line(std::string_view line, int level, bool &changed)
{
    if (line.empty() || indented_code(line))
        return std::string(line);

    const std::size_t indent = heading_indent(line);
    if (indent >= 4U)
        return std::string(line);

    const auto heading = heading_at(line, indent);
    if (heading && heading->level == level)
    {
        changed = true;
        return std::string(line.substr(0, indent)) + std::string(line.substr(heading->content));
    }

    const std::string_view content = heading ? line.substr(heading->content) : line.substr(indent);
    if (!has_non_whitespace(content))
        return std::string(line);

    std::string transformed(line.substr(0, indent));
    transformed.append(static_cast<std::size_t>(level), '#');
    transformed.push_back(' ');
    transformed.append(content);
    if (transformed != line)
        changed = true;
    return transformed;
}

struct ListItem
{
    MarkdownListStyle style = MarkdownListStyle::Bullet;
    std::size_t indent = 0;
    std::size_t content = 0;
};

std::optional<ListItem> list_item_at(std::string_view line, std::size_t indent) noexcept
{
    if (indent >= line.size())
        return std::nullopt;

    std::size_t marker_end = indent;
    MarkdownListStyle style = MarkdownListStyle::Bullet;
    if (line[marker_end] == '-' || line[marker_end] == '*' || line[marker_end] == '+')
    {
        ++marker_end;
    }
    else
    {
        const std::size_t digits_begin = marker_end;
        while (marker_end < line.size() && line[marker_end] >= '0' && line[marker_end] <= '9')
            ++marker_end;
        if (marker_end == digits_begin || marker_end == line.size() ||
            (line[marker_end] != '.' && line[marker_end] != ')'))
            return std::nullopt;
        ++marker_end;
        style = MarkdownListStyle::Ordered;
    }

    if (marker_end == line.size() || (line[marker_end] != ' ' && line[marker_end] != '\t'))
        return std::nullopt;
    while (marker_end < line.size() && (line[marker_end] == ' ' || line[marker_end] == '\t'))
        ++marker_end;
    return ListItem{style, indent, marker_end};
}

std::optional<std::string> continued_list_marker(std::string_view line, ListItem item)
{
    if (item.style == MarkdownListStyle::Bullet)
        return std::string(line.substr(item.indent, item.content - item.indent));

    std::size_t cursor = item.indent;
    std::size_t ordinal = 0;
    while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9')
    {
        const std::size_t digit = static_cast<std::size_t>(line[cursor] - '0');
        if (ordinal > (std::numeric_limits<std::size_t>::max() - digit) / 10U)
            return std::nullopt;
        ordinal = ordinal * 10U + digit;
        ++cursor;
    }
    if (cursor == item.indent || cursor >= line.size() || (line[cursor] != '.' && line[cursor] != ')') ||
        ordinal == std::numeric_limits<std::size_t>::max())
        return std::nullopt;
    return std::to_string(ordinal + 1U) + line[cursor] + " ";
}

std::optional<bool> task_checked_at(std::string_view line, std::size_t offset) noexcept
{
    if (offset + 3U > line.size() || line[offset] != '[' || line[offset + 2U] != ']')
        return std::nullopt;
    if (line[offset + 1U] != ' ' && line[offset + 1U] != 'x' && line[offset + 1U] != 'X')
        return std::nullopt;
    if (offset + 3U < line.size() && line[offset + 3U] != ' ' && line[offset + 3U] != '\t')
        return std::nullopt;
    return line[offset + 1U] == 'x' || line[offset + 1U] == 'X';
}

std::string transform_task_line(std::string_view line, bool &changed)
{
    const bool has_carriage_return = !line.empty() && line.back() == '\r';
    const std::string_view content = has_carriage_return ? line.substr(0, line.size() - 1U) : line;
    if (content.empty() || indented_code(content))
        return std::string(line);

    const std::size_t indent = heading_indent(content);
    if (heading_at(content, indent) || content.substr(indent).starts_with('>') || content.find('|') != std::string_view::npos)
        return std::string(line);

    if (const auto list = list_item_at(content, indent))
    {
        if (const auto checked = task_checked_at(content, list->content))
        {
            std::string transformed(line);
            transformed[list->content + 1U] = *checked ? ' ' : 'x';
            changed = true;
            return transformed;
        }

        std::string transformed(content.substr(0, list->content));
        transformed += "[ ] ";
        transformed += content.substr(list->content);
        if (has_carriage_return)
            transformed.push_back('\r');
        changed = true;
        return transformed;
    }

    if (indent >= 4U || !has_non_whitespace(content.substr(indent)))
        return std::string(line);

    std::string transformed(content.substr(0, indent));
    transformed += "- [ ] ";
    transformed += content.substr(indent);
    if (has_carriage_return)
        transformed.push_back('\r');
    changed = true;
    return transformed;
}

bool thematic_break(std::string_view line) noexcept
{
    char marker = '\0';
    std::size_t count = 0;
    for (const char character : line)
    {
        if (character == ' ' || character == '\t')
            continue;
        if ((character != '-' && character != '*' && character != '_') || (marker != '\0' && character != marker))
            return false;
        marker = character;
        ++count;
    }
    return count >= 3U;
}

bool list_transformable(std::string_view line) noexcept
{
    if (line.empty() || indented_code(line))
        return false;

    const std::size_t indent = heading_indent(line);
    if (indent >= 4U || !has_non_whitespace(line.substr(indent)) || heading_at(line, indent) ||
        line.substr(indent).starts_with('>') || line.find('|') != std::string_view::npos || thematic_break(line))
        return false;
    return true;
}

std::size_t task_content_start(std::string_view line, std::size_t offset) noexcept
{
    if (!task_checked_at(line, offset))
        return offset;
    offset += 3U;
    while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t'))
        ++offset;
    return offset;
}

std::string transform_list_line(std::string_view line,
                                MarkdownListStyle style,
                                std::size_t ordinal,
                                bool remove,
                                bool &changed)
{
    const bool has_carriage_return = !line.empty() && line.back() == '\r';
    const std::string_view content = has_carriage_return ? line.substr(0, line.size() - 1U) : line;
    if (!list_transformable(content))
        return std::string(line);

    const std::size_t indent = heading_indent(content);
    const auto list = list_item_at(content, indent);
    if (remove)
    {
        if (!list || list->style != style)
            return std::string(line);

        std::string transformed(content.substr(0, list->indent));
        transformed += content.substr(task_content_start(content, list->content));
        if (has_carriage_return)
            transformed.push_back('\r');
        if (transformed != line)
            changed = true;
        return transformed;
    }

    std::string transformed(content.substr(0, indent));
    if (style == MarkdownListStyle::Bullet)
        transformed += "- ";
    else
        transformed += std::to_string(ordinal) + ". ";
    transformed += list ? content.substr(list->content) : content.substr(indent);
    if (has_carriage_return)
        transformed.push_back('\r');
    if (transformed != line)
        changed = true;
    return transformed;
}

bool selection_has_only_list_style(std::string_view source,
                                   std::size_t first_line,
                                   std::size_t last_line_end,
                                   MarkdownListStyle style)
{
    std::optional<Fence> active_fence = fence_before(source, first_line);
    bool saw_transformable = false;
    bool all_matching = true;

    for (std::size_t begin = first_line; begin <= last_line_end;)
    {
        const std::size_t end = source.find('\n', begin);
        const std::size_t current_end = end == std::string_view::npos ? source.size() : end;
        const std::string_view line = source.substr(begin, current_end - begin);

        if (active_fence)
        {
            if (fence_closes(line, *active_fence))
                active_fence.reset();
        }
        else if (const auto opening = fence_at(line))
        {
            active_fence = opening;
        }
        else
        {
            const std::string_view content = !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1U) : line;
            if (list_transformable(content))
            {
                saw_transformable = true;
                const auto list = list_item_at(content, heading_indent(content));
                all_matching = all_matching && list && list->style == style;
            }
        }

        if (current_end == last_line_end)
            break;
        begin = current_end + 1U;
    }
    return saw_transformable && all_matching;
}

struct MarkdownLink
{
    MarkdownByteRange range;
    MarkdownByteRange label;
};

std::optional<MarkdownLink> markdown_link_at(std::string_view source, std::size_t begin) noexcept
{
    if (begin >= source.size() || source[begin] != '[')
        return std::nullopt;

    bool escaped = false;
    std::size_t label_end = begin + 1U;
    for (; label_end + 1U < source.size(); ++label_end)
    {
        const char character = source[label_end];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (character == '\\')
        {
            escaped = true;
            continue;
        }
        if (character == ']' && source[label_end + 1U] == '(')
            break;
    }
    if (label_end + 1U >= source.size())
        return std::nullopt;

    escaped = false;
    int depth = 1;
    std::size_t link_end = label_end + 2U;
    for (; link_end < source.size(); ++link_end)
    {
        const char character = source[link_end];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (character == '\\')
        {
            escaped = true;
            continue;
        }
        if (character == '(')
        {
            ++depth;
        }
        else if (character == ')' && --depth == 0)
        {
            return MarkdownLink{{begin, link_end + 1U}, {begin + 1U, label_end}};
        }
    }
    return std::nullopt;
}

bool valid_link_destination(std::string_view destination) noexcept
{
    return !destination.empty() && std::none_of(destination.begin(), destination.end(), [](char character) {
        return character == ' ' || character == '\t' || character == '\r' || character == '\n';
    });
}

std::optional<MarkdownTransformEdit> remove_markdown_link(std::string_view source, MarkdownLink link)
{
    const std::string label(source.substr(link.label.begin, link.label.end - link.label.begin));
    if (label.empty())
        return std::nullopt;
    return MarkdownTransformEdit{
        .replaced = link.range,
        .replacement = label,
        .selection = {link.range.begin, link.range.begin + label.size()},
    };
}

bool reference_definition(std::string_view line) noexcept
{
    if (line.empty() || line.front() != '[')
        return false;
    const std::size_t close = line.find("]:");
    return close != std::string_view::npos && close > 1U;
}

bool setext_underline(std::string_view line) noexcept
{
    const std::size_t indent = heading_indent(line);
    if (indent >= 4U || indent == line.size() || (line[indent] != '=' && line[indent] != '-'))
        return false;
    const char marker = line[indent];
    for (std::size_t offset = indent; offset < line.size(); ++offset)
    {
        if (line[offset] != marker && line[offset] != ' ' && line[offset] != '\t')
            return false;
    }
    return true;
}

bool paragraph_reflowable_line(std::string_view line) noexcept
{
    if (line.empty() || indented_code(line))
        return false;

    const std::size_t indent = heading_indent(line);
    if (indent >= 4U)
        return false;
    const std::string_view content = line.substr(indent);
    return has_non_whitespace(content) && !heading_at(line, indent) && !content.starts_with('>') &&
           !list_item_at(line, indent) && content.find('|') == std::string_view::npos && !thematic_break(content) &&
           !reference_definition(content);
}

struct MarkdownSourceLine
{
    std::size_t begin = 0;
    std::size_t end = 0;
    std::string_view content;
    bool crlf = false;
    bool reflowable = false;
};

std::vector<MarkdownSourceLine> markdown_source_lines(std::string_view source)
{
    std::vector<MarkdownSourceLine> lines;
    std::optional<Fence> active_fence;
    for (std::size_t begin = 0;;)
    {
        const std::size_t newline = source.find('\n', begin);
        const std::size_t end = newline == std::string_view::npos ? source.size() : newline;
        const std::string_view raw = source.substr(begin, end - begin);
        const bool crlf = !raw.empty() && raw.back() == '\r';
        const std::string_view content = crlf ? raw.substr(0, raw.size() - 1U) : raw;
        bool reflowable = false;

        if (active_fence)
        {
            if (fence_closes(content, *active_fence))
                active_fence.reset();
        }
        else if (const auto opening = fence_at(content))
        {
            active_fence = opening;
        }
        else
        {
            reflowable = paragraph_reflowable_line(content);
        }
        lines.push_back({begin, end, content, crlf, reflowable});

        if (newline == std::string_view::npos)
            break;
        begin = newline + 1U;
    }

    for (std::size_t index = 0; index + 1U < lines.size(); ++index)
    {
        if (lines[index].reflowable && setext_underline(lines[index + 1U].content))
            lines[index].reflowable = false;
    }
    return lines;
}

bool paragraph_reflow_safe(const std::vector<MarkdownSourceLine> &lines,
                           std::size_t begin,
                           std::size_t end) noexcept
{
    for (std::size_t index = begin; index < end; ++index)
    {
        const std::string_view content = lines[index].content;
        if (content.find('`') != std::string_view::npos || content.ends_with("  ") || content.ends_with('\\'))
            return false;
    }
    return true;
}

std::size_t utf8_code_point_count(std::string_view value) noexcept
{
    return static_cast<std::size_t>(std::count_if(value.begin(), value.end(), [](char character) {
        return (static_cast<unsigned char>(character) & 0xC0U) != 0x80U;
    }));
}

std::string reflow_paragraph(const std::vector<MarkdownSourceLine> &lines,
                             std::size_t begin,
                             std::size_t end,
                             std::size_t width)
{
    const std::size_t indent_size = heading_indent(lines[begin].content);
    const std::string indent(lines[begin].content.substr(0, indent_size));
    const std::string_view newline = lines[begin].crlf ? "\r\n" : "\n";
    std::vector<std::string> words;

    for (std::size_t index = begin; index < end; ++index)
    {
        std::string_view content = lines[index].content.substr(heading_indent(lines[index].content));
        while (!content.empty() && (content.back() == ' ' || content.back() == '\t'))
            content.remove_suffix(1U);
        for (std::size_t word_begin = 0; word_begin < content.size();)
        {
            while (word_begin < content.size() && (content[word_begin] == ' ' || content[word_begin] == '\t'))
                ++word_begin;
            const std::size_t word_end = content.find_first_of(" \t", word_begin);
            if (word_begin < content.size())
                words.emplace_back(content.substr(word_begin, word_end - word_begin));
            if (word_end == std::string_view::npos)
                break;
            word_begin = word_end + 1U;
        }
    }

    std::string reflowed;
    std::string current = indent;
    std::size_t current_width = utf8_code_point_count(indent);
    bool has_word = false;
    for (const std::string &word : words)
    {
        const std::size_t word_width = utf8_code_point_count(word);
        if (has_word && current_width + 1U + word_width > width)
        {
            if (!reflowed.empty())
                reflowed += newline;
            reflowed += current;
            current = indent;
            current_width = utf8_code_point_count(indent);
            has_word = false;
        }
        if (has_word)
        {
            current.push_back(' ');
            ++current_width;
        }
        current += word;
        current_width += word_width;
        has_word = true;
    }
    if (!reflowed.empty())
        reflowed += newline;
    reflowed += current;
    if (lines[end - 1U].crlf)
        reflowed.push_back('\r');
    return reflowed;
}

std::size_t quote_indent(std::string_view line) noexcept
{
    std::size_t indent = 0;
    while (indent < line.size() && indent < 3U && line[indent] == ' ')
        ++indent;
    return indent;
}

std::optional<std::size_t> quote_content_start(std::string_view line) noexcept
{
    const std::size_t indent = quote_indent(line);
    if (indent == line.size() || line[indent] != '>')
        return std::nullopt;
    std::size_t content = indent + 1U;
    if (content < line.size() && (line[content] == ' ' || line[content] == '\t'))
        ++content;
    return content;
}

bool quote_transformable(std::string_view line) noexcept
{
    const std::string_view content = !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1U) : line;
    return !indented_code(content);
}

std::string transform_quote_line(std::string_view line, bool remove, bool &changed)
{
    if (!quote_transformable(line))
        return std::string(line);

    const bool has_carriage_return = !line.empty() && line.back() == '\r';
    const std::string_view content = has_carriage_return ? line.substr(0, line.size() - 1U) : line;
    if (remove)
    {
        const auto quoted = quote_content_start(content);
        if (!quoted)
            return std::string(line);
        std::string transformed(content.substr(0, quote_indent(content)));
        transformed += content.substr(*quoted);
        if (has_carriage_return)
            transformed.push_back('\r');
        changed = true;
        return transformed;
    }

    std::string transformed(content.substr(0, quote_indent(content)));
    transformed += content.empty() ? ">" : "> ";
    transformed += content.substr(quote_indent(content));
    if (has_carriage_return)
        transformed.push_back('\r');
    changed = true;
    return transformed;
}

bool selection_has_only_quoted_ordinary_lines(std::string_view source,
                                              std::size_t first_line,
                                              std::size_t last_line_end)
{
    std::optional<Fence> active_fence = fence_before(source, first_line);
    bool saw_nonblank = false;
    bool all_quoted = true;

    for (std::size_t begin = first_line; begin <= last_line_end;)
    {
        const std::size_t end = source.find('\n', begin);
        const std::size_t current_end = end == std::string_view::npos ? source.size() : end;
        const std::string_view line = source.substr(begin, current_end - begin);

        if (active_fence)
        {
            if (fence_closes(line, *active_fence))
                active_fence.reset();
        }
        else if (const auto opening = fence_at(line))
        {
            active_fence = opening;
        }
        else if (quote_transformable(line))
        {
            const std::string_view content = !line.empty() && line.back() == '\r'
                                                 ? line.substr(0, line.size() - 1U)
                                                 : line;
            if (!content.empty())
            {
                saw_nonblank = true;
                all_quoted = all_quoted && quote_content_start(content).has_value();
            }
        }

        if (current_end == last_line_end)
            break;
        begin = current_end + 1U;
    }
    return saw_nonblank && all_quoted;
}

std::optional<MarkdownTransformEdit> remove_code_wrapper(std::string_view source,
                                                          MarkdownByteRange selection)
{
    const std::string_view selected = source.substr(selection.begin, selection.end - selection.begin);
    const std::size_t leading = marker_run_after(selected, 0, '`');
    const std::size_t trailing = marker_run_before(selected, selected.size(), '`');
    if (leading != 0U && leading == trailing && selected.size() > leading * 2U)
    {
        std::string_view content = selected.substr(leading, selected.size() - leading * 2U);
        if (content.size() >= 2U && content.front() == ' ' && content.back() == ' ')
            content = content.substr(1U, content.size() - 2U);
        if (has_non_whitespace(content) && leading > longest_backtick_run(content))
        {
            return MarkdownTransformEdit{
                .replaced = selection,
                .replacement = std::string(content),
                .selection = {selection.begin, selection.begin + content.size()},
            };
        }
    }

    const std::size_t longest = longest_backtick_run(selected);

    const auto remove = [&](std::size_t marker_begin, std::size_t marker_end) -> std::optional<MarkdownTransformEdit> {
        const std::size_t left = marker_run_after(source, marker_begin, '`');
        const std::size_t right = marker_run_before(source, marker_end, '`');
        if (left == 0U || left != right || left <= longest)
            return std::nullopt;
        return MarkdownTransformEdit{
            .replaced = {marker_begin, marker_end},
            .replacement = std::string(selected),
            .selection = {marker_begin, marker_begin + selected.size()},
        };
    };

    if (selection.begin > 0U && selection.end < source.size() && source[selection.begin - 1U] == '`' &&
        source[selection.end] == '`')
    {
        const std::size_t marker_begin = selection.begin - marker_run_before(source, selection.begin, '`');
        const std::size_t marker_end = selection.end + marker_run_after(source, selection.end, '`');
        if (const auto removed = remove(marker_begin, marker_end))
            return removed;
    }

    if (selection.begin > 1U && selection.end + 1U < source.size() && source[selection.begin - 1U] == ' ' &&
        source[selection.end] == ' ' && source[selection.begin - 2U] == '`' && source[selection.end + 1U] == '`')
    {
        const std::size_t marker_begin = selection.begin - 1U - marker_run_before(source, selection.begin - 1U, '`');
        const std::size_t marker_end = selection.end + 1U + marker_run_after(source, selection.end + 1U, '`');
        if (const auto removed = remove(marker_begin, marker_end))
            return removed;
    }

    return std::nullopt;
}

} // namespace

std::optional<MarkdownTransformEdit> toggle_markdown_inline_style(
    std::string_view source,
    MarkdownByteRange selection,
    MarkdownInlineStyle style)
{
    if (!valid_range(source, selection) || selection.begin == selection.end)
        return std::nullopt;

    const std::string_view selected = source.substr(selection.begin, selection.end - selection.begin);
    if (!has_non_whitespace(selected))
        return std::nullopt;

    std::string marker;
    switch (style)
    {
    case MarkdownInlineStyle::Bold: marker = "**"; break;
    case MarkdownInlineStyle::Italic: marker = "*"; break;
    case MarkdownInlineStyle::Strikethrough: marker = "~~"; break;
    case MarkdownInlineStyle::Code:
        if (const auto removed = remove_code_wrapper(source, selection))
            return removed;
        marker.assign(longest_backtick_run(selected) + 1U, '`');
        {
            const bool pad = selected.front() == '`' || selected.back() == '`' || selected.front() == ' ' ||
                             selected.back() == ' ' || selected.front() == '\t' || selected.back() == '\t' ||
                             selected.front() == '\n' || selected.back() == '\n' || selected.front() == '\r' ||
                             selected.back() == '\r';
            return MarkdownTransformEdit{
                .replaced = selection,
                .replacement = marker + (pad ? " " : "") + std::string(selected) + (pad ? " " : "") + marker,
                .selection = {selection.begin + marker.size() + (pad ? 1U : 0U),
                              selection.end + marker.size() + (pad ? 1U : 0U)},
            };
        }
    }

    if (style != MarkdownInlineStyle::Code)
    {
        if (const auto removed = remove_included_marker(selection, selected, marker))
            return removed;
        if (surrounded_by_exact_marker(source, selection, marker))
        {
            const std::size_t begin = selection.begin - marker.size();
            return MarkdownTransformEdit{
                .replaced = {begin, selection.end + marker.size()},
                .replacement = std::string(selected),
                .selection = {begin, begin + selected.size()},
            };
        }
    }

    return MarkdownTransformEdit{
        .replaced = selection,
        .replacement = marker + std::string(selected) + marker,
        .selection = {selection.begin + marker.size(), selection.end + marker.size()},
    };
}

std::optional<MarkdownTransformEdit> toggle_markdown_heading(
    std::string_view source,
    MarkdownByteRange selection,
    int level)
{
    if (!valid_range(source, selection) || level < 1 || level > 6)
        return std::nullopt;

    const std::size_t first_line = line_start(source, selection.begin);
    std::size_t last_position = selection.begin;
    if (selection.end > selection.begin)
    {
        last_position = selection.end - 1U;
        if (source[last_position] == '\n' && last_position > 0U)
            --last_position;
    }
    const std::size_t last_line_end = line_end(source, last_position);

    std::optional<Fence> active_fence = fence_before(source, first_line);
    std::string replacement;
    replacement.reserve(last_line_end - first_line);
    bool changed = false;

    for (std::size_t begin = first_line; begin <= last_line_end;)
    {
        const std::size_t end = source.find('\n', begin);
        const std::size_t current_end = end == std::string_view::npos ? source.size() : end;
        const std::string_view line = source.substr(begin, current_end - begin);

        if (active_fence)
        {
            replacement.append(line);
            if (fence_closes(line, *active_fence))
                active_fence.reset();
        }
        else if (const auto opening = fence_at(line))
        {
            replacement.append(line);
            active_fence = opening;
        }
        else
        {
            replacement += transform_heading_line(line, level, changed);
        }

        if (current_end == last_line_end)
            break;
        replacement.push_back('\n');
        begin = current_end + 1U;
    }

    if (!changed)
        return std::nullopt;
    const MarkdownByteRange restored_selection{first_line, first_line + replacement.size()};
    return MarkdownTransformEdit{
        .replaced = {first_line, last_line_end},
        .replacement = std::move(replacement),
        .selection = restored_selection,
    };
}

std::optional<MarkdownTransformEdit> toggle_markdown_task(
    std::string_view source,
    MarkdownByteRange selection)
{
    if (!valid_range(source, selection))
        return std::nullopt;

    const std::size_t first_line = line_start(source, selection.begin);
    std::size_t last_position = selection.begin;
    if (selection.end > selection.begin)
    {
        last_position = selection.end - 1U;
        if (source[last_position] == '\n' && last_position > 0U)
            --last_position;
    }
    const std::size_t last_line_end = line_end(source, last_position);

    std::optional<Fence> active_fence = fence_before(source, first_line);
    std::string replacement;
    replacement.reserve(last_line_end - first_line);
    bool changed = false;

    for (std::size_t begin = first_line; begin <= last_line_end;)
    {
        const std::size_t end = source.find('\n', begin);
        const std::size_t current_end = end == std::string_view::npos ? source.size() : end;
        const std::string_view line = source.substr(begin, current_end - begin);

        if (active_fence)
        {
            replacement.append(line);
            if (fence_closes(line, *active_fence))
                active_fence.reset();
        }
        else if (const auto opening = fence_at(line))
        {
            replacement.append(line);
            active_fence = opening;
        }
        else
        {
            replacement += transform_task_line(line, changed);
        }

        if (current_end == last_line_end)
            break;
        replacement.push_back('\n');
        begin = current_end + 1U;
    }

    if (!changed)
        return std::nullopt;
    const MarkdownByteRange restored_selection{first_line, first_line + replacement.size()};
    return MarkdownTransformEdit{
        .replaced = {first_line, last_line_end},
        .replacement = std::move(replacement),
        .selection = restored_selection,
    };
}

std::optional<MarkdownTransformEdit> toggle_markdown_quote(
    std::string_view source,
    MarkdownByteRange selection)
{
    if (!valid_range(source, selection))
        return std::nullopt;

    const std::size_t first_line = line_start(source, selection.begin);
    std::size_t last_position = selection.begin;
    if (selection.end > selection.begin)
    {
        last_position = selection.end - 1U;
        if (source[last_position] == '\n' && last_position > 0U)
            --last_position;
    }
    const std::size_t last_line_end = line_end(source, last_position);
    const bool remove = selection_has_only_quoted_ordinary_lines(source, first_line, last_line_end);

    std::optional<Fence> active_fence = fence_before(source, first_line);
    std::string replacement;
    replacement.reserve(last_line_end - first_line);
    bool changed = false;

    for (std::size_t begin = first_line; begin <= last_line_end;)
    {
        const std::size_t end = source.find('\n', begin);
        const std::size_t current_end = end == std::string_view::npos ? source.size() : end;
        const std::string_view line = source.substr(begin, current_end - begin);

        if (active_fence)
        {
            replacement.append(line);
            if (fence_closes(line, *active_fence))
                active_fence.reset();
        }
        else if (const auto opening = fence_at(line))
        {
            replacement.append(line);
            active_fence = opening;
        }
        else
        {
            replacement += transform_quote_line(line, remove, changed);
        }

        if (current_end == last_line_end)
            break;
        replacement.push_back('\n');
        begin = current_end + 1U;
    }

    if (!changed)
        return std::nullopt;
    const MarkdownByteRange restored_selection{first_line, first_line + replacement.size()};
    return MarkdownTransformEdit{
        .replaced = {first_line, last_line_end},
        .replacement = std::move(replacement),
        .selection = restored_selection,
    };
}

std::optional<MarkdownTransformEdit> toggle_markdown_list(
    std::string_view source,
    MarkdownByteRange selection,
    MarkdownListStyle style)
{
    if (!valid_range(source, selection))
        return std::nullopt;

    const std::size_t first_line = line_start(source, selection.begin);
    std::size_t last_position = selection.begin;
    if (selection.end > selection.begin)
    {
        last_position = selection.end - 1U;
        if (source[last_position] == '\n' && last_position > 0U)
            --last_position;
    }
    const std::size_t last_line_end = line_end(source, last_position);
    const bool remove = selection_has_only_list_style(source, first_line, last_line_end, style);

    std::optional<Fence> active_fence = fence_before(source, first_line);
    std::string replacement;
    replacement.reserve(last_line_end - first_line);
    bool changed = false;
    std::size_t ordinal = 1;

    for (std::size_t begin = first_line; begin <= last_line_end;)
    {
        const std::size_t end = source.find('\n', begin);
        const std::size_t current_end = end == std::string_view::npos ? source.size() : end;
        const std::string_view line = source.substr(begin, current_end - begin);

        if (active_fence)
        {
            replacement.append(line);
            if (fence_closes(line, *active_fence))
                active_fence.reset();
        }
        else if (const auto opening = fence_at(line))
        {
            replacement.append(line);
            active_fence = opening;
        }
        else
        {
            const std::string_view content = !line.empty() && line.back() == '\r' ? line.substr(0, line.size() - 1U) : line;
            replacement += transform_list_line(line, style, ordinal, remove, changed);
            if (list_transformable(content))
                ++ordinal;
        }

        if (current_end == last_line_end)
            break;
        replacement.push_back('\n');
        begin = current_end + 1U;
    }

    if (!changed)
        return std::nullopt;
    const MarkdownByteRange restored_selection{first_line, first_line + replacement.size()};
    return MarkdownTransformEdit{
        .replaced = {first_line, last_line_end},
        .replacement = std::move(replacement),
        .selection = restored_selection,
    };
}

std::optional<MarkdownTransformEdit> continue_markdown_list(
    std::string_view source,
    MarkdownByteRange selection)
{
    if (!valid_range(source, selection) || selection.begin != selection.end)
        return std::nullopt;

    const std::size_t begin = line_start(source, selection.begin);
    const std::size_t end = line_end(source, selection.begin);
    const std::string_view raw_line = source.substr(begin, end - begin);
    const bool crlf = !raw_line.empty() && raw_line.back() == '\r';
    const std::string_view line = crlf ? raw_line.substr(0, raw_line.size() - 1U) : raw_line;
    if (selection.begin != begin + line.size() || fence_before(source, begin))
        return std::nullopt;

    const auto item = list_item_at(line, heading_indent(line));
    if (!item)
        return std::nullopt;

    const std::size_t text_begin = task_content_start(line, item->content);
    if (!has_non_whitespace(line.substr(text_begin)))
    {
        return MarkdownTransformEdit{
            .replaced = {begin, selection.begin},
            .replacement = "",
            .selection = {begin, begin},
        };
    }

    const auto marker = continued_list_marker(line, *item);
    if (!marker)
        return std::nullopt;
    const bool task = task_checked_at(line, item->content).has_value();
    const std::string_view newline = crlf ? "\r\n" : "\n";
    std::string replacement;
    replacement.reserve(newline.size() + item->indent + marker->size() + (task ? 4U : 0U));
    replacement += newline;
    replacement += line.substr(0, item->indent);
    replacement += *marker;
    if (task)
        replacement += "[ ] ";
    const MarkdownByteRange restored_selection{selection.begin + replacement.size(),
                                                selection.begin + replacement.size()};
    return MarkdownTransformEdit{
        .replaced = selection,
        .replacement = std::move(replacement),
        .selection = restored_selection,
    };
}

std::optional<MarkdownTransformEdit> toggle_markdown_link(
    std::string_view source,
    MarkdownByteRange selection,
    std::string_view destination)
{
    if (!valid_range(source, selection) || selection.begin == selection.end)
        return std::nullopt;

    if (const auto link = markdown_link_at(source, selection.begin);
        link && link->range.end == selection.end)
        return remove_markdown_link(source, *link);

    if (selection.begin > 0U)
    {
        if (const auto link = markdown_link_at(source, selection.begin - 1U);
            link && link->label == selection)
            return remove_markdown_link(source, *link);
    }

    const std::string_view label = source.substr(selection.begin, selection.end - selection.begin);
    if (!has_non_whitespace(label) || !valid_link_destination(destination))
        return std::nullopt;
    return MarkdownTransformEdit{
        .replaced = selection,
        .replacement = "[" + std::string(label) + "](" + std::string(destination) + ")",
        .selection = {selection.begin + 1U, selection.end + 1U},
    };
}

std::optional<MarkdownTransformEdit> reflow_markdown_paragraphs(
    std::string_view source,
    MarkdownByteRange selection,
    std::size_t width)
{
    if (!valid_range(source, selection) || width < 20U)
        return std::nullopt;

    const std::size_t first_line = line_start(source, selection.begin);
    std::size_t last_position = selection.begin;
    if (selection.end > selection.begin)
    {
        last_position = selection.end - 1U;
        if (source[last_position] == '\n' && last_position > 0U)
            --last_position;
    }
    const std::size_t last_line_end = line_end(source, last_position);
    const std::vector<MarkdownSourceLine> lines = markdown_source_lines(source);

    std::size_t first_selected = lines.size();
    std::size_t last_selected = lines.size();
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        if (lines[index].begin == first_line)
            first_selected = index;
        if (lines[index].end == last_line_end)
            last_selected = index;
    }
    if (first_selected == lines.size() || last_selected == lines.size() || first_selected > last_selected)
        return std::nullopt;

    struct ParagraphReplacement
    {
        std::size_t begin = 0;
        std::size_t end = 0;
        std::string text;
    };
    std::vector<ParagraphReplacement> replacements;

    for (std::size_t paragraph_begin = 0; paragraph_begin < lines.size();)
    {
        if (!lines[paragraph_begin].reflowable)
        {
            ++paragraph_begin;
            continue;
        }
        std::size_t paragraph_end = paragraph_begin + 1U;
        while (paragraph_end < lines.size() && lines[paragraph_end].reflowable)
            ++paragraph_end;

        const bool selected = paragraph_begin <= last_selected && paragraph_end > first_selected;
        if (selected && paragraph_reflow_safe(lines, paragraph_begin, paragraph_end))
        {
            std::string reflowed = reflow_paragraph(lines, paragraph_begin, paragraph_end, width);
            const std::size_t begin = lines[paragraph_begin].begin;
            const std::size_t end = lines[paragraph_end - 1U].end;
            if (reflowed != source.substr(begin, end - begin))
                replacements.push_back({begin, end, std::move(reflowed)});
        }
        paragraph_begin = paragraph_end;
    }

    if (replacements.empty())
        return std::nullopt;

    const std::size_t replace_begin = replacements.front().begin;
    const std::size_t replace_end = replacements.back().end;
    std::string replacement;
    std::size_t copied_until = replace_begin;
    for (const ParagraphReplacement &paragraph : replacements)
    {
        replacement += source.substr(copied_until, paragraph.begin - copied_until);
        replacement += paragraph.text;
        copied_until = paragraph.end;
    }
    replacement += source.substr(copied_until, replace_end - copied_until);
    const MarkdownByteRange restored_selection{replace_begin, replace_begin + replacement.size()};
    return MarkdownTransformEdit{
        .replaced = {replace_begin, replace_end},
        .replacement = std::move(replacement),
        .selection = restored_selection,
    };
}

} // namespace ck::edit
