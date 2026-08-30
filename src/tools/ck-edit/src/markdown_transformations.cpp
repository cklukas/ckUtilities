#include "ck/edit/markdown_transformations.hpp"

#include <algorithm>
#include <string>

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

std::optional<std::size_t> task_list_content_start(std::string_view line, std::size_t indent) noexcept
{
    if (indent >= line.size())
        return std::nullopt;

    std::size_t marker_end = indent;
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
    }

    if (marker_end == line.size() || (line[marker_end] != ' ' && line[marker_end] != '\t'))
        return std::nullopt;
    while (marker_end < line.size() && (line[marker_end] == ' ' || line[marker_end] == '\t'))
        ++marker_end;
    return marker_end;
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

    if (const auto list_content = task_list_content_start(content, indent))
    {
        if (const auto checked = task_checked_at(content, *list_content))
        {
            std::string transformed(line);
            transformed[*list_content + 1U] = *checked ? ' ' : 'x';
            changed = true;
            return transformed;
        }

        std::string transformed(content.substr(0, *list_content));
        transformed += "[ ] ";
        transformed += content.substr(*list_content);
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
    return MarkdownTransformEdit{
        .replaced = {first_line, last_line_end},
        .replacement = std::move(replacement),
        .selection = {first_line, first_line + replacement.size()},
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
    return MarkdownTransformEdit{
        .replaced = {first_line, last_line_end},
        .replacement = std::move(replacement),
        .selection = {first_line, first_line + replacement.size()},
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
    return MarkdownTransformEdit{
        .replaced = {first_line, last_line_end},
        .replacement = std::move(replacement),
        .selection = {first_line, first_line + replacement.size()},
    };
}

} // namespace ck::edit
