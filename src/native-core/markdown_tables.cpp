#include "ck/edit/markdown_tables.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace ck::edit
{
namespace
{

constexpr std::size_t kMaximumColumns = 64U;
constexpr std::size_t kMaximumBodyRows = 256U;

struct SourceLine
{
    std::size_t begin = 0;
    std::size_t content_end = 0;
    std::size_t end = 0;
    bool terminated = false;
};

struct ParsedTable
{
    std::size_t begin = 0;
    std::size_t end = 0;
    std::vector<std::string> header;
    std::vector<std::string> separator;
    std::vector<std::vector<std::string>> body;
    std::string newline = "\n";
    bool trailing_newline = false;
    std::size_t active_column = 0;
    std::optional<std::size_t> active_body_row;
};

struct RenderedTable
{
    std::string text;
    std::vector<std::vector<MarkdownByteRange>> editable_cells;
};

struct TableInsertionContext
{
    std::string newline;
    bool needs_leading_newline = false;
    bool needs_trailing_newline = true;
};

bool is_space(unsigned char value)
{
    return value == ' ' || value == '\t';
}

std::string_view trim(std::string_view value)
{
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

std::vector<SourceLine> split_lines(std::string_view source)
{
    std::vector<SourceLine> lines;
    std::size_t begin = 0;
    while (begin < source.size())
    {
        const std::size_t newline = source.find('\n', begin);
        const bool terminated = newline != std::string_view::npos;
        const std::size_t end = terminated ? newline + 1U : source.size();
        std::size_t content_end = terminated ? newline : end;
        if (content_end > begin && source[content_end - 1U] == '\r')
            --content_end;
        lines.push_back({begin, content_end, end, terminated});
        begin = end;
    }
    if (source.empty() || source.ends_with('\n'))
        lines.push_back({source.size(), source.size(), source.size(), false});
    return lines;
}

std::optional<std::size_t> line_at(const std::vector<SourceLine> &lines, std::size_t byte)
{
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        const SourceLine &line = lines[index];
        if (byte >= line.begin && (byte < line.end || (!line.terminated && byte == line.end)))
            return index;
    }
    return std::nullopt;
}

bool is_unescaped_pipe(std::string_view text, std::size_t index)
{
    if (index >= text.size() || text[index] != '|')
        return false;
    std::size_t slashes = 0;
    for (std::size_t cursor = index; cursor > 0 && text[cursor - 1U] == '\\'; --cursor)
        ++slashes;
    return (slashes % 2U) == 0U;
}

bool is_indented_code(std::string_view text)
{
    return text.starts_with("    ") || text.starts_with("\t");
}

bool is_fence(std::string_view text)
{
    text = trim(text);
    return text.starts_with("```") || text.starts_with("~~~");
}

std::vector<bool> fence_context(const std::vector<SourceLine> &lines, std::string_view source)
{
    std::vector<bool> inside(lines.size(), false);
    bool fenced = false;
    for (std::size_t index = 0; index < lines.size(); ++index)
    {
        inside[index] = fenced;
        const SourceLine &line = lines[index];
        if (is_fence(source.substr(line.begin, line.content_end - line.begin)))
            fenced = !fenced;
    }
    return inside;
}

std::optional<std::vector<std::string>> parse_row(std::string_view text)
{
    if (is_indented_code(text))
        return std::nullopt;
    text = trim(text);
    if (text.empty())
        return std::nullopt;

    bool has_pipe = false;
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (is_unescaped_pipe(text, index))
        {
            has_pipe = true;
            break;
        }
    }
    if (!has_pipe)
        return std::nullopt;

    std::size_t begin = text.front() == '|' ? 1U : 0U;
    std::size_t end = text.back() == '|' ? text.size() - 1U : text.size();
    if (begin > end)
        return std::nullopt;

    std::vector<std::string> cells;
    std::size_t cell_begin = begin;
    for (std::size_t index = begin; index < end; ++index)
    {
        if (!is_unescaped_pipe(text, index))
            continue;
        cells.emplace_back(trim(text.substr(cell_begin, index - cell_begin)));
        cell_begin = index + 1U;
    }
    cells.emplace_back(trim(text.substr(cell_begin, end - cell_begin)));
    return cells.empty() ? std::nullopt : std::optional<std::vector<std::string>>(std::move(cells));
}

bool is_separator_cell(std::string_view value)
{
    value = trim(value);
    if (!value.empty() && value.front() == ':')
        value.remove_prefix(1);
    if (!value.empty() && value.back() == ':')
        value.remove_suffix(1);
    if (value.size() < 3U)
        return false;
    return std::all_of(value.begin(), value.end(), [](char character) { return character == '-'; });
}

bool is_separator_row(const std::vector<std::string> &cells, std::size_t expected_columns)
{
    return cells.size() == expected_columns &&
           std::all_of(cells.begin(), cells.end(), [](const std::string &cell) { return is_separator_cell(cell); });
}

std::string source_newline(std::string_view source)
{
    const std::size_t found = source.find('\n');
    if (found == std::string_view::npos)
        return "\n";
    return found > 0U && source[found - 1U] == '\r' ? "\r\n" : "\n";
}

std::size_t column_at(std::string_view source, const SourceLine &line, std::size_t byte, std::size_t columns)
{
    if (columns == 0U)
        return 0U;
    const std::string_view text = source.substr(line.begin, line.content_end - line.begin);
    std::size_t first = 0;
    while (first < text.size() && is_space(static_cast<unsigned char>(text[first])))
        ++first;
    const bool leading_pipe = first < text.size() && text[first] == '|';
    const std::size_t relative = std::min(byte - line.begin, text.size());
    std::size_t seen = 0;
    for (std::size_t index = first; index < relative; ++index)
    {
        if (is_unescaped_pipe(text, index))
            ++seen;
    }
    const std::size_t result = leading_pipe ? (seen == 0U ? 0U : seen - 1U) : seen;
    return std::min(result, columns - 1U);
}

std::optional<ParsedTable> locate_table(std::string_view source, MarkdownByteRange selection)
{
    if (selection.begin > selection.end || selection.end > source.size())
        return std::nullopt;
    const std::vector<SourceLine> lines = split_lines(source);
    const std::optional<std::size_t> active_line = line_at(lines, selection.begin);
    if (!active_line)
        return std::nullopt;
    const std::vector<bool> fenced = fence_context(lines, source);

    for (std::size_t header_line = 0; header_line + 1U < lines.size(); ++header_line)
    {
        if (fenced[header_line] || fenced[header_line + 1U])
            continue;
        const SourceLine &header_source = lines[header_line];
        const SourceLine &separator_source = lines[header_line + 1U];
        const auto header = parse_row(source.substr(header_source.begin, header_source.content_end - header_source.begin));
        const auto separator = parse_row(source.substr(separator_source.begin, separator_source.content_end - separator_source.begin));
        if (!header || !separator || !is_separator_row(*separator, header->size()))
            continue;

        std::vector<std::vector<std::string>> body;
        std::size_t next_line = header_line + 2U;
        while (next_line < lines.size() && !fenced[next_line])
        {
            const SourceLine &candidate = lines[next_line];
            const auto row = parse_row(source.substr(candidate.begin, candidate.content_end - candidate.begin));
            if (!row || row->size() != header->size())
                break;
            body.push_back(*row);
            ++next_line;
        }

        const std::size_t last_line = next_line == header_line + 2U ? header_line + 1U : next_line - 1U;
        const std::size_t begin = lines[header_line].begin;
        const std::size_t end = lines[last_line].end;
        if (selection.begin < begin || selection.end > end || *active_line < header_line || *active_line > last_line)
            continue;

        ParsedTable table;
        table.begin = begin;
        table.end = end;
        table.header = *header;
        table.separator = *separator;
        table.body = std::move(body);
        table.newline = source_newline(source);
        table.trailing_newline = lines[last_line].terminated;
        table.active_column = column_at(source, lines[*active_line], selection.begin, table.header.size());
        if (*active_line >= header_line + 2U)
            table.active_body_row = *active_line - (header_line + 2U);
        return table;
    }
    return std::nullopt;
}

void append_row(RenderedTable &rendered,
                const std::vector<std::string> &cells,
                std::string_view newline,
                bool terminate,
                bool capture_cells)
{
    std::vector<MarkdownByteRange> ranges;
    rendered.text.push_back('|');
    for (const std::string &cell : cells)
    {
        rendered.text.push_back(' ');
        const std::size_t begin = rendered.text.size();
        rendered.text += cell;
        const std::size_t end = rendered.text.size();
        rendered.text += " |";
        if (capture_cells)
            ranges.push_back({begin, end});
    }
    if (terminate)
        rendered.text += newline;
    if (capture_cells)
        rendered.editable_cells.push_back(std::move(ranges));
}

RenderedTable render_table(const ParsedTable &table)
{
    RenderedTable rendered;
    append_row(rendered, table.header, table.newline, true, true);
    append_row(rendered, table.separator, table.newline, !table.body.empty() || table.trailing_newline, false);
    for (std::size_t index = 0; index < table.body.size(); ++index)
        append_row(rendered, table.body[index], table.newline, index + 1U < table.body.size() || table.trailing_newline, true);
    return rendered;
}

MarkdownTransformEdit table_edit(const ParsedTable &table,
                                 RenderedTable rendered,
                                 MarkdownByteRange selection)
{
    return {{table.begin, table.end}, std::move(rendered.text),
            {table.begin + selection.begin, table.begin + selection.end}};
}

MarkdownByteRange header_selection(const RenderedTable &rendered, std::size_t column)
{
    return rendered.editable_cells.front().at(column);
}

MarkdownByteRange body_selection(const RenderedTable &rendered, std::size_t row, std::size_t column)
{
    return rendered.editable_cells.at(row + 1U).at(column);
}

bool valid_dimensions(std::size_t columns, std::size_t body_rows)
{
    return columns > 0U && columns <= kMaximumColumns && body_rows <= kMaximumBodyRows;
}

std::optional<TableInsertionContext> table_insertion_context(std::string_view source, MarkdownByteRange cursor)
{
    const std::vector<SourceLine> lines = split_lines(source);
    const std::optional<std::size_t> line = line_at(lines, cursor.begin);
    if (!line)
        return std::nullopt;
    if (cursor.begin > 0U && cursor.begin < source.size() && source[cursor.begin - 1U] == '\r' &&
        source[cursor.begin] == '\n')
        return std::nullopt;
    const std::vector<bool> fenced = fence_context(lines, source);
    if (fenced[*line])
        return std::nullopt;
    const SourceLine &source_line = lines[*line];
    if (is_indented_code(source.substr(source_line.begin, source_line.content_end - source_line.begin)) ||
        locate_table(source, cursor))
        return std::nullopt;

    // A cursor at the end of an ordinary terminated line can reuse that line
    // ending as the table's final line break. An empty line is intentionally
    // preserved as an empty separator after the inserted table.
    const bool reuses_following_newline = source_line.terminated && cursor.begin == source_line.content_end &&
                                           source_line.content_end > source_line.begin;
    const bool follows_table = cursor.begin > 0U && source[cursor.begin - 1U] == '\n' &&
                               locate_table(source, {cursor.begin - 1U, cursor.begin - 1U}).has_value();
    return TableInsertionContext{source_newline(source),
                                 (cursor.begin > 0U && source[cursor.begin - 1U] != '\n') || follows_table,
                                 !reuses_following_newline};
}

} // namespace

std::optional<MarkdownTransformEdit> insert_markdown_table(std::string_view source,
                                                            MarkdownByteRange cursor,
                                                            std::size_t columns,
                                                            std::size_t body_rows)
{
    if (cursor.begin != cursor.end || cursor.end > source.size() || !valid_dimensions(columns, body_rows))
        return std::nullopt;
    const std::optional<TableInsertionContext> context = table_insertion_context(source, cursor);
    if (!context)
        return std::nullopt;

    ParsedTable table;
    table.begin = cursor.begin;
    table.end = cursor.end;
    table.newline = context->newline;
    table.trailing_newline = context->needs_trailing_newline;
    table.header.reserve(columns);
    table.separator.assign(columns, "---");
    for (std::size_t column = 0; column < columns; ++column)
        table.header.push_back("Header " + std::to_string(column + 1U));
    table.body.assign(body_rows, std::vector<std::string>(columns));

    RenderedTable rendered = render_table(table);
    const MarkdownByteRange selected = header_selection(rendered, 0U);
    std::string replacement;
    if (context->needs_leading_newline)
        replacement += context->newline;
    const std::size_t selection_offset = replacement.size();
    replacement += rendered.text;
    return MarkdownTransformEdit{{cursor.begin, cursor.end}, std::move(replacement),
                                 {cursor.begin + selection_offset + selected.begin,
                                  cursor.begin + selection_offset + selected.end}};
}

std::optional<MarkdownTransformEdit> insert_markdown_table_row(std::string_view source,
                                                                MarkdownByteRange selection,
                                                                MarkdownTableInsertPosition position)
{
    auto table = locate_table(source, selection);
    if (!table || table->body.size() >= kMaximumBodyRows)
        return std::nullopt;
    const std::size_t index = table->active_body_row
                                  ? *table->active_body_row + (position == MarkdownTableInsertPosition::After ? 1U : 0U)
                                  : 0U;
    table->body.insert(table->body.begin() + static_cast<std::ptrdiff_t>(index),
                       std::vector<std::string>(table->header.size()));
    RenderedTable rendered = render_table(*table);
    const MarkdownByteRange selected = body_selection(rendered, index, 0U);
    return table_edit(*table, std::move(rendered), selected);
}

std::optional<MarkdownTransformEdit> erase_markdown_table_row(std::string_view source, MarkdownByteRange selection)
{
    auto table = locate_table(source, selection);
    if (!table || !table->active_body_row || *table->active_body_row >= table->body.size())
        return std::nullopt;
    const std::size_t erased = *table->active_body_row;
    table->body.erase(table->body.begin() + static_cast<std::ptrdiff_t>(erased));
    RenderedTable rendered = render_table(*table);
    const MarkdownByteRange next_selection = table->body.empty()
                                                  ? header_selection(rendered, 0U)
                                                  : body_selection(rendered, std::min(erased, table->body.size() - 1U), 0U);
    return table_edit(*table, std::move(rendered), next_selection);
}

std::optional<MarkdownTransformEdit> insert_markdown_table_column(std::string_view source,
                                                                   MarkdownByteRange selection,
                                                                   MarkdownTableInsertPosition position)
{
    auto table = locate_table(source, selection);
    if (!table || table->header.size() >= kMaximumColumns)
        return std::nullopt;
    const std::size_t index = table->active_column + (position == MarkdownTableInsertPosition::After ? 1U : 0U);
    table->header.insert(table->header.begin() + static_cast<std::ptrdiff_t>(index),
                         "Column " + std::to_string(index + 1U));
    table->separator.insert(table->separator.begin() + static_cast<std::ptrdiff_t>(index), "---");
    for (auto &row : table->body)
        row.insert(row.begin() + static_cast<std::ptrdiff_t>(index), std::string{});
    RenderedTable rendered = render_table(*table);
    const MarkdownByteRange selected = header_selection(rendered, index);
    return table_edit(*table, std::move(rendered), selected);
}

std::optional<MarkdownTransformEdit> erase_markdown_table_column(std::string_view source, MarkdownByteRange selection)
{
    auto table = locate_table(source, selection);
    if (!table || table->header.size() <= 1U || table->active_column >= table->header.size())
        return std::nullopt;
    const std::size_t erased = table->active_column;
    table->header.erase(table->header.begin() + static_cast<std::ptrdiff_t>(erased));
    table->separator.erase(table->separator.begin() + static_cast<std::ptrdiff_t>(erased));
    for (auto &row : table->body)
        row.erase(row.begin() + static_cast<std::ptrdiff_t>(erased));
    RenderedTable rendered = render_table(*table);
    const MarkdownByteRange selected = header_selection(rendered, std::min(erased, table->header.size() - 1U));
    return table_edit(*table, std::move(rendered), selected);
}

} // namespace ck::edit
