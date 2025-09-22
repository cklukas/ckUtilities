#include "ck/edit/markdown_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ck::edit
{
namespace
{
bool isWhitespace(char ch) noexcept
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

bool isAlphaNumeric(char ch) noexcept
{
    return std::isalnum(static_cast<unsigned char>(ch)) != 0;
}

std::string lower(std::string_view view)
{
    std::string result(view.begin(), view.end());
    for (char &ch : result)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return result;
}

bool isDigitString(std::string_view view)
{
    if (view.empty())
        return false;
    for (char ch : view)
        if (!std::isdigit(static_cast<unsigned char>(ch)))
            return false;
    return true;
}

bool looksLikeUrl(std::string_view view)
{
    auto lowerView = lower(view.substr(0, view.find(':')));
    return lowerView == "http" || lowerView == "https" || lowerView == "ftp" || lowerView == "mailto";
}

std::string columnNameFromIndex(std::size_t index)
{
    std::string name;
    std::size_t value = index;
    while (true)
    {
        char letter = static_cast<char>('A' + (value % 26));
        name.insert(name.begin(), letter);
        if (value < 26)
            break;
        value = value / 26 - 1;
    }
    return name;
}

} // namespace

MarkdownParserState MarkdownAnalyzer::computeStateBefore(const std::string &text)
{
    MarkdownParserState state;
    std::size_t offset = 0;
    while (offset < text.size())
    {
        std::size_t end = text.find('\n', offset);
        std::string line;
        if (end == std::string::npos)
        {
            line = text.substr(offset);
            offset = text.size();
        }
        else
        {
            line = text.substr(offset, end - offset);
            offset = end + 1;
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        analyzeLine(line, state);
    }
    return state;
}

MarkdownLineInfo MarkdownAnalyzer::analyzeLine(const std::string &line, MarkdownParserState &state) const
{
    auto resetTable = [&]() {
        state.tableActive = false;
        state.tableHeaderConfirmed = false;
        state.tableRowCounter = 0;
        state.tableAlignments.clear();
    };

    if (state.inFence)
    {
        resetTable();
        return analyzeFencedState(line, state);
    }

    MarkdownLineInfo info;
    std::string trimmed = trim(line);
    if (trimmed.empty())
    {
        info.kind = MarkdownLineKind::Blank;
        resetTable();
        parseInline(line, info);
        return info;
    }

    if (isHtmlBlockStart(trimmed))
    {
        info.kind = MarkdownLineKind::Html;
        resetTable();
        parseInline(line, info);
        return info;
    }

    if (trimmed.size() >= 3)
    {
        char c = trimmed.front();
        if ((c == '`' || c == '~'))
        {
            std::size_t count = 0;
            while (count < trimmed.size() && trimmed[count] == c)
                ++count;
            if (count >= 3)
            {
                info.kind = MarkdownLineKind::CodeFenceStart;
                info.fenceOpens = true;
                state.inFence = true;
                state.fenceMarker = std::string(count, c);
                state.fenceIndented = false;
                if (trimmed.size() > count)
                {
                    auto lang = trim(trimmed.substr(count));
                    info.language = lang;
                }
                info.inFence = true;
                info.fenceLabel = describeLine(info);
                state.fenceLabel = info.fenceLabel;
                state.fenceLanguage = info.language;
                resetTable();
                parseInline(line, info);
                return info;
            }
        }
    }

    bool indented = false;
    int spaceCount = 0;
    for (char ch : line)
    {
        if (ch == ' ')
            ++spaceCount;
        else if (ch == '\t')
            spaceCount += 4;
        else
            break;
    }
    if (spaceCount >= 4 && trimmed.front() != '-' && trimmed.front() != '*' && trimmed.front() != '+')
        indented = true;

    if (indented)
    {
        info.kind = MarkdownLineKind::IndentedCode;
        resetTable();
        parseInline(line, info);
        return info;
    }

    if (trimmed.front() == '>')
    {
        info.kind = MarkdownLineKind::BlockQuote;
        resetTable();
        parseInline(trimmed.substr(1), info);
        return info;
    }

    if (trimmed.front() == '#')
    {
        std::size_t level = 0;
        while (level < trimmed.size() && trimmed[level] == '#')
            ++level;
        if (level > 0 && level <= 6 && (trimmed.size() == level || trimmed[level] == ' '))
        {
            info.kind = MarkdownLineKind::Heading;
            info.headingLevel = static_cast<int>(level);
            resetTable();
            parseInline(trimmed.substr(level), info);
            return info;
        }
    }

    if (isHorizontalRule(trimmed))
    {
        info.kind = MarkdownLineKind::HorizontalRule;
        resetTable();
        parseInline(line, info);
        return info;
    }

    if (isTableSeparator(trimmed))
    {
        info.kind = MarkdownLineKind::TableSeparator;
        info.tableCells = parseTableRow(line);
        info.tableAlignments = parseAlignmentRow(line);
        state.tableActive = true;
        state.tableHeaderConfirmed = true;
        if (state.tableRowCounter == 0)
            state.tableRowCounter = 1;
        info.tableRowIndex = state.tableRowCounter;
        state.tableAlignments = info.tableAlignments;
        parseInline(line, info);
        return info;
    }

    bool isBullet = false;
    bool isOrdered = false;
    std::string marker;
    if (!trimmed.empty())
    {
        char first = trimmed.front();
        if (first == '-' || first == '*' || first == '+')
        {
            std::size_t second = 1;
            if (second < trimmed.size() && trimmed[second] == ' ')
            {
                isBullet = true;
                marker.push_back(first);
            }
        }
        else
        {
            std::size_t pos = 0;
            while (pos < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[pos])))
                ++pos;
            if (pos > 0 && pos < trimmed.size() && (trimmed[pos] == '.' || trimmed[pos] == ')'))
            {
                marker = trimmed.substr(0, pos + 1);
                if (pos + 1 < trimmed.size() && trimmed[pos + 1] == ' ')
                    isOrdered = true;
            }
        }
    }

    if (isBullet || isOrdered)
    {
        info.kind = isOrdered ? MarkdownLineKind::OrderedListItem : MarkdownLineKind::BulletListItem;
        info.isOrdered = isOrdered;
        info.marker = marker;
        std::string rest;
        if (isOrdered)
        {
            std::size_t skip = marker.size();
            if (skip < trimmed.size() && trimmed[skip] == ' ')
                rest = trimmed.substr(skip + 1);
        }
        else
            rest = trimmed.substr(2);
        auto trimmedRest = trim(rest);
        if (trimmedRest.size() >= 3 && trimmedRest.front() == '[' && trimmedRest[2] == ']' &&
            (trimmedRest[1] == ' ' || trimmedRest[1] == 'x' || trimmedRest[1] == 'X'))
        {
            info.isTask = true;
        }
        resetTable();
        parseInline(rest, info);
        return info;
    }

    if (trimmed.find('|') != std::string::npos)
    {
        if (!state.tableActive)
        {
            state.tableActive = true;
            state.tableHeaderConfirmed = false;
            state.tableRowCounter = 0;
            state.tableAlignments.clear();
        }
        info.kind = MarkdownLineKind::TableRow;
        info.tableCells = parseTableRow(line);
        info.tableAlignments = state.tableAlignments;
        info.isTableHeader = !state.tableHeaderConfirmed;
        info.tableRowIndex = state.tableRowCounter + 1;
        state.tableRowCounter = info.tableRowIndex;
        parseInline(line, info);
        return info;
    }

    info.kind = MarkdownLineKind::Paragraph;
    resetTable();
    parseInline(line, info);
    return info;
}

const MarkdownSpan *MarkdownAnalyzer::spanAtColumn(const MarkdownLineInfo &info, std::size_t column) const
{
    for (const auto &span : info.spans)
    {
        if (span.start <= column && column < span.end)
            return &span;
    }
    return nullptr;
}

std::string MarkdownAnalyzer::describeLine(const MarkdownLineInfo &info) const
{
    switch (info.kind)
    {
    case MarkdownLineKind::Blank:
        return "Blank";
    case MarkdownLineKind::Heading:
    {
        std::ostringstream out;
        out << "Heading " << info.headingLevel;
        return out.str();
    }
    case MarkdownLineKind::BlockQuote:
        return "Block Quote";
    case MarkdownLineKind::BulletListItem:
        return info.isTask ? "Task Item" : "Bullet List";
    case MarkdownLineKind::OrderedListItem:
        return info.isTask ? "Task Item" : "Numbered List";
    case MarkdownLineKind::CodeFenceStart:
        return info.language.empty() ? "Code Fence" : "Code Fence (" + info.language + ")";
    case MarkdownLineKind::CodeFenceEnd:
        return "Code Fence End";
    case MarkdownLineKind::FencedCode:
        return "Code";
    case MarkdownLineKind::IndentedCode:
        return "Indented Code";
    case MarkdownLineKind::HorizontalRule:
        return "Horizontal Rule";
    case MarkdownLineKind::TableSeparator:
        return "Table Alignments";
    case MarkdownLineKind::TableRow:
        if (info.isTableHeader)
            return "Table Header";
        else
        {
            std::ostringstream out;
            out << "Table Row " << info.tableRowIndex;
            return out.str();
        }
    case MarkdownLineKind::Paragraph:
        return "Paragraph";
    case MarkdownLineKind::Html:
        return "HTML";
    default:
        return "Text";
    }
}

std::string MarkdownAnalyzer::describeSpan(const MarkdownSpan &span) const
{
    switch (span.kind)
    {
    case MarkdownSpanKind::Bold:
        return "Bold";
    case MarkdownSpanKind::Italic:
        return "Italic";
    case MarkdownSpanKind::BoldItalic:
        return "Bold+Italic";
    case MarkdownSpanKind::Strikethrough:
        return "Strikethrough";
    case MarkdownSpanKind::Code:
        return "Inline Code";
    case MarkdownSpanKind::Link:
        if (!span.attribute.empty())
            return "Link: " + span.attribute;
        return "Link";
    case MarkdownSpanKind::Image:
        if (!span.attribute.empty())
            return "Image: " + span.attribute;
        return "Image";
    case MarkdownSpanKind::InlineHtml:
        return "Inline HTML";
    default:
        return "Text";
    }
}

std::string MarkdownAnalyzer::describeTableCell(const MarkdownLineInfo &info, std::size_t column) const
{
    auto alignmentName = [](MarkdownTableAlignment alignment) {
        switch (alignment)
        {
        case MarkdownTableAlignment::Left:
            return "Left";
        case MarkdownTableAlignment::Center:
            return "Center";
        case MarkdownTableAlignment::Right:
            return "Right";
        case MarkdownTableAlignment::Number:
            return "Number";
        default:
            return "Default";
        }
    };

    std::string columnLabel = columnNameFromIndex(column);
    if (info.kind == MarkdownLineKind::TableSeparator)
    {
        MarkdownTableAlignment alignment = MarkdownTableAlignment::Default;
        if (column < info.tableAlignments.size())
            alignment = info.tableAlignments[column];
        std::ostringstream out;
        out << columnLabel << " alignment: " << alignmentName(alignment);
        return out.str();
    }

    if (info.kind != MarkdownLineKind::TableRow)
        return columnLabel;

    std::string text;
    if (column < info.tableCells.size())
        text = info.tableCells[column].text;
    if (text.empty())
        text = info.isTableHeader ? "Header" : "Cell";

    MarkdownTableAlignment alignment = MarkdownTableAlignment::Default;
    if (column < info.tableAlignments.size())
        alignment = info.tableAlignments[column];

    std::ostringstream out;
    out << columnLabel << info.tableRowIndex << ": " << text;
    if (alignment != MarkdownTableAlignment::Default)
        out << " (" << alignmentName(alignment) << ')';
    return out.str();
}

bool MarkdownAnalyzer::isHorizontalRule(const std::string &trimmed) noexcept
{
    if (trimmed.size() < 3)
        return false;
    char first = trimmed.front();
    if (first != '-' && first != '*' && first != '_')
        return false;
    int count = 0;
    for (char ch : trimmed)
    {
        if (ch == first)
            ++count;
        else if (!isWhitespace(ch))
            return false;
    }
    return count >= 3;
}

bool MarkdownAnalyzer::isTableSeparator(const std::string &trimmed) noexcept
{
    if (trimmed.empty())
        return false;
    if (trimmed.find('|') == std::string::npos)
        return false;
    std::size_t start = 0;
    while (start < trimmed.size())
    {
        std::size_t end = trimmed.find('|', start);
        if (end == std::string::npos)
            end = trimmed.size();
        std::string_view cell(trimmed.data() + start, end - start);
        auto t = trim(cell);
        if (!t.empty())
        {
            for (char ch : t)
                if (ch != '-' && ch != ':' && ch != ' ')
                    return false;
        }
        start = end + 1;
    }
    return true;
}

std::vector<MarkdownTableCell> MarkdownAnalyzer::parseTableRow(const std::string &line)
{
    std::vector<MarkdownTableCell> cells;
    std::size_t len = line.size();
    std::size_t start = 0;
    bool escape = false;
    if (len > 0 && line[0] == '|')
        start = 1;
    for (std::size_t i = start; i < len; ++i)
    {
        char ch = line[i];
        if (escape)
        {
            escape = false;
            continue;
        }
        if (ch == '\\')
        {
            escape = true;
            continue;
        }
        if (ch == '|')
        {
            MarkdownTableCell cell;
            cell.startColumn = start;
            cell.endColumn = i;
            std::size_t trimmedStart = start;
            while (trimmedStart < i && isWhitespace(line[trimmedStart]))
                ++trimmedStart;
            std::size_t trimmedEnd = i;
            while (trimmedEnd > trimmedStart && isWhitespace(line[trimmedEnd - 1]))
                --trimmedEnd;
            cell.text.assign(line.begin() + trimmedStart, line.begin() + trimmedEnd);
            cells.push_back(cell);
            start = i + 1;
        }
    }
    if (start <= len)
    {
        MarkdownTableCell cell;
        cell.startColumn = start;
        cell.endColumn = len;
        std::size_t trimmedStart = start;
        while (trimmedStart < len && isWhitespace(line[trimmedStart]))
            ++trimmedStart;
        std::size_t trimmedEnd = len;
        while (trimmedEnd > trimmedStart && trimmedEnd > 0 && isWhitespace(line[trimmedEnd - 1]))
            --trimmedEnd;
        cell.text.assign(line.begin() + trimmedStart, line.begin() + trimmedEnd);
        cells.push_back(cell);
    }
    return cells;
}

std::vector<MarkdownTableAlignment> MarkdownAnalyzer::parseAlignmentRow(const std::string &line)
{
    std::vector<MarkdownTableAlignment> alignments;
    auto cells = parseTableRow(line);
    alignments.reserve(cells.size());
    for (const auto &cell : cells)
    {
        std::string_view text(cell.text);
        bool left = !text.empty() && text.front() == ':';
        bool right = !text.empty() && text.back() == ':';
        bool numeric = right && text.size() >= 2 && text[text.size() - 2] == ':';
        if (numeric)
            alignments.push_back(MarkdownTableAlignment::Number);
        else if (left && right)
            alignments.push_back(MarkdownTableAlignment::Center);
        else if (left)
            alignments.push_back(MarkdownTableAlignment::Left);
        else if (right)
            alignments.push_back(MarkdownTableAlignment::Right);
        else
            alignments.push_back(MarkdownTableAlignment::Default);
    }
    return alignments;
}

std::string MarkdownAnalyzer::trimLeft(std::string_view view) noexcept
{
    std::size_t pos = 0;
    while (pos < view.size() && isWhitespace(view[pos]))
        ++pos;
    return std::string(view.substr(pos));
}

std::string MarkdownAnalyzer::trimRight(std::string_view view) noexcept
{
    if (view.empty())
        return std::string();
    std::size_t end = view.size();
    while (end > 0 && isWhitespace(view[end - 1]))
        --end;
    return std::string(view.substr(0, end));
}

std::string MarkdownAnalyzer::trim(std::string_view view) noexcept
{
    return trimLeft(trimRight(view));
}

bool MarkdownAnalyzer::isHtmlBlockStart(const std::string &trimmed) noexcept
{
    if (trimmed.size() < 3)
        return false;
    if (trimmed.front() != '<')
        return false;
    if (trimmed[1] == '!' || trimmed[1] == '?' || trimmed[1] == '/')
        return true;
    return std::isalpha(static_cast<unsigned char>(trimmed[1])) != 0;
}

MarkdownLineInfo MarkdownAnalyzer::analyzeFencedState(const std::string &line, MarkdownParserState &state) const
{
    MarkdownLineInfo info;
    info.inFence = true;
    info.fenceLabel = state.fenceLabel;
    info.language = state.fenceLanguage;
    std::string trimmed = trim(line);
    if (!state.fenceMarker.empty() && trimmed.rfind(state.fenceMarker, 0) == 0)
    {
        info.kind = MarkdownLineKind::CodeFenceEnd;
        info.fenceCloses = true;
        state.inFence = false;
        state.fenceMarker.clear();
        state.fenceLabel.clear();
        state.fenceLanguage.clear();
    }
    else
    {
        info.kind = MarkdownLineKind::FencedCode;
    }
    parseInline(line, info);
    return info;
}

void MarkdownAnalyzer::parseInline(const std::string &line, MarkdownLineInfo &info) const
{
    parseEmphasis(line, info.spans);
    parseCodeSpans(line, info.spans);
    parseLinksAndImages(line, info.spans);
    parseInlineHtml(line, info.spans);
}

void MarkdownAnalyzer::parseEmphasis(const std::string &line, std::vector<MarkdownSpan> &spans) const
{
    struct Marker
    {
        char ch;
        int length;
        std::size_t position;
    };
    std::vector<Marker> stack;
    for (std::size_t i = 0; i < line.size();)
    {
        char ch = line[i];
        if (ch == '*' || ch == '_' || ch == '~')
        {
            std::size_t j = i;
            while (j < line.size() && line[j] == ch)
                ++j;
            int sequence = static_cast<int>(j - i);
            int segment = 0;
            while (sequence > 0)
            {
                if (ch == '~')
                {
                    if (sequence < 2)
                        break;
                    segment = 2;
                }
                else if (sequence >= 3)
                    segment = 3;
                else if (sequence >= 2)
                    segment = 2;
                else
                    segment = 1;

                bool matched = false;
                for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                {
                    if (it->ch == ch && it->length == segment)
                    {
                        std::size_t start = it->position + segment;
                        std::size_t end = i;
                        if (end >= start)
                        {
                            MarkdownSpan span;
                            span.start = start;
                            span.end = end;
                            if (ch == '~')
                                span.kind = MarkdownSpanKind::Strikethrough;
                            else if (segment == 3)
                                span.kind = MarkdownSpanKind::BoldItalic;
                            else if (segment == 2)
                                span.kind = MarkdownSpanKind::Bold;
                            else
                                span.kind = MarkdownSpanKind::Italic;
                            spans.push_back(span);
                        }
                        stack.erase(std::next(it).base());
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    stack.push_back(Marker{ch, segment, i});
                i += segment;
                sequence -= segment;
            }
            i = j;
        }
        else
            ++i;
    }
}

void MarkdownAnalyzer::parseCodeSpans(const std::string &line, std::vector<MarkdownSpan> &spans) const
{
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '`')
        {
            std::size_t j = i;
            while (j < line.size() && line[j] == '`')
                ++j;
            std::size_t fenceLen = j - i;
            std::size_t end = line.find(std::string(fenceLen, '`'), j);
            if (end != std::string::npos)
            {
                MarkdownSpan span;
                span.kind = MarkdownSpanKind::Code;
                span.start = j;
                span.end = end;
                spans.push_back(span);
                i = end + fenceLen - 1;
            }
            else
                break;
        }
    }
}

void MarkdownAnalyzer::parseLinksAndImages(const std::string &line, std::vector<MarkdownSpan> &spans) const
{
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        bool isImage = false;
        if (line[i] == '!')
        {
            if (i + 1 < line.size() && line[i + 1] == '[')
            {
                isImage = true;
                ++i;
            }
            else
                continue;
        }
        if (line[i] == '[')
        {
            std::size_t depth = 1;
            std::size_t j = i + 1;
            while (j < line.size() && depth > 0)
            {
                if (line[j] == '[')
                    ++depth;
                else if (line[j] == ']')
                    --depth;
                ++j;
            }
            if (depth == 0)
            {
                std::size_t closeBracket = j - 1;
                std::size_t k = closeBracket + 1;
                while (k < line.size() && isWhitespace(line[k]))
                    ++k;
                if (k < line.size() && line[k] == '(')
                {
                    ++k;
                    std::size_t urlStart = k;
                    int parenDepth = 1;
                    while (k < line.size() && parenDepth > 0)
                    {
                        if (line[k] == '(')
                            ++parenDepth;
                        else if (line[k] == ')')
                            --parenDepth;
                        ++k;
                    }
                    if (parenDepth == 0)
                    {
                        std::size_t urlEnd = k - 1;
                        MarkdownSpan span;
                        span.kind = isImage ? MarkdownSpanKind::Image : MarkdownSpanKind::Link;
                        span.start = isImage ? i - 1 : i;
                        span.end = k;
                        std::string url = trim(std::string_view(line.data() + urlStart, urlEnd - urlStart));
                        if (!url.empty() && url.front() == '<' && url.back() == '>')
                            url = url.substr(1, url.size() - 2);
                        span.attribute = url;
                        spans.push_back(span);
                        i = k - 1;
                    }
                }
                else if (!isImage)
                {
                    std::string_view text(line.data() + i + 1, closeBracket - (i + 1));
                    if (looksLikeUrl(text))
                    {
                        MarkdownSpan span;
                        span.kind = MarkdownSpanKind::Link;
                        span.start = i;
                        span.end = closeBracket + 1;
                        span.attribute = std::string(text);
                        spans.push_back(span);
                        i = closeBracket;
                    }
                }
            }
        }
    }
}

void MarkdownAnalyzer::parseInlineHtml(const std::string &line, std::vector<MarkdownSpan> &spans) const
{
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '<')
        {
            std::size_t j = i + 1;
            while (j < line.size() && line[j] != '>')
                ++j;
            if (j < line.size())
            {
                MarkdownSpan span;
                span.kind = MarkdownSpanKind::InlineHtml;
                span.start = i;
                span.end = j + 1;
                spans.push_back(span);
                i = j;
            }
        }
    }
}

} // namespace ck::edit
