#include "chat_transcript_view.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "ck/edit/markdown_parser.hpp"

namespace {

constexpr std::uint16_t kStyleNone = 0;
constexpr std::uint16_t kStyleBold = 1u << 0;
constexpr std::uint16_t kStyleItalic = 1u << 1;
constexpr std::uint16_t kStyleStrikethrough = 1u << 2;
constexpr std::uint16_t kStyleInlineCode = 1u << 3;
constexpr std::uint16_t kStyleLink = 1u << 4;
constexpr std::uint16_t kStyleHeading = 1u << 5;
constexpr std::uint16_t kStyleQuote = 1u << 6;
constexpr std::uint16_t kStyleListMarker = 1u << 7;
constexpr std::uint16_t kStyleTableBorder = 1u << 8;
constexpr std::uint16_t kStyleTableHeader = 1u << 9;
constexpr std::uint16_t kStyleTableCell = 1u << 10;
constexpr std::uint16_t kStyleCodeBlock = 1u << 11;
constexpr std::uint16_t kStylePrefix = 1u << 12;

struct StyledLine
{
    std::string text;
    std::vector<std::uint16_t> styles;
};

struct ChannelSegment
{
  std::string channel;
  std::string text;
  bool from_marker = false;
};

std::string_view trim_view(std::string_view view)
{
  const char *whitespace = " \t\r\n";
  auto begin = view.find_first_not_of(whitespace);
  if (begin == std::string_view::npos)
    return std::string_view();
  auto end = view.find_last_not_of(whitespace);
  return view.substr(begin, end - begin + 1);
}

std::string trim_copy(std::string_view view)
{
  std::string_view trimmed = trim_view(view);
  return std::string(trimmed);
}

std::string to_lower_copy(std::string_view view)
{
  std::string lowered;
  lowered.reserve(view.size());
  for (char ch : view)
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return lowered;
}

bool is_final_channel(std::string_view channel)
{
  std::string trimmed = trim_copy(channel);
  std::string lowered = to_lower_copy(trimmed);
  return lowered.empty() || lowered == "final";
}

std::string sanitize_for_display(const std::string &input)
{
  std::string output;
  output.reserve(input.size());

  const unsigned char *data = reinterpret_cast<const unsigned char *>(input.data());
  std::size_t len = input.size();
  std::size_t i = 0;

  auto append_ascii = [&](char ch) {
    if (ch == '\r')
      return;
    if (ch >= 0x20 || ch == '\n' || ch == '\t')
      output.push_back(ch);
    else
      output.push_back(' ');
  };

  while (i < len)
  {
    unsigned char byte = data[i];
    if (byte < 0x80)
    {
      append_ascii(static_cast<char>(byte));
      ++i;
      continue;
    }

    std::size_t startIndex = i;
    std::size_t extra = 0;
    uint32_t codepoint = 0;
    if ((byte & 0xE0) == 0xC0)
    {
      extra = 1;
      codepoint = byte & 0x1F;
    }
    else if ((byte & 0xF0) == 0xE0)
    {
      extra = 2;
      codepoint = byte & 0x0F;
    }
    else if ((byte & 0xF8) == 0xF0)
    {
      extra = 3;
      codepoint = byte & 0x07;
    }
    else
    {
      output.push_back('?');
      ++i;
      continue;
    }

    if (i + extra >= len)
    {
      output.push_back('?');
      break;
    }

    bool valid = true;
    for (std::size_t j = 1; j <= extra; ++j)
    {
      unsigned char follow = data[i + j];
      if ((follow & 0xC0) != 0x80)
      {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6) | (follow & 0x3F);
    }

    if (!valid)
    {
      output.push_back('?');
      ++i;
      continue;
    }

    i += extra + 1;

    auto append_replacement = [&](char ch) {
      if (ch == '\r')
        return;
      output.push_back(ch);
    };

    switch (codepoint)
    {
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x2032:
      append_replacement('\'');
      break;
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x2033:
      append_replacement('"');
      break;
    case 0x2013:
    case 0x2014:
    case 0x2212:
      append_replacement('-');
      break;
    case 0x2026:
      output.append("...");
      break;
    case 0x2122:
      output.append("(TM)");
      break;
    case 0x00A9:
      output.append("(C)");
      break;
    case 0x00AE:
      output.append("(R)");
      break;
    case 0x00A0:
    case 0x2007:
    case 0x2009:
    case 0x200A:
    case 0x200B:
      output.push_back(' ');
      break;
    default:
      if (codepoint <= 0x7E)
        append_replacement(static_cast<char>(codepoint));
      else if (codepoint == '\n')
        output.push_back('\n');
      else if (codepoint == '\t')
        output.push_back('\t');
      else
        output.append(input, startIndex, extra + 1);
      break;
    }
  }

  return output;
}

std::string last_non_empty_line(const std::string &text)
{
  std::string sanitized = sanitize_for_display(text);
  std::string_view view(sanitized);
  if (view.empty())
    return std::string();

  std::size_t pos = view.size();
  while (pos > 0)
  {
    std::size_t newlinePos = view.rfind('\n', pos - 1);
    std::size_t start = (newlinePos == std::string_view::npos) ? 0 : newlinePos + 1;
    std::string_view candidate = view.substr(start, pos - start);
    candidate = trim_view(candidate);
    if (!candidate.empty())
      return std::string(candidate);
    if (newlinePos == std::string_view::npos)
      break;
    pos = newlinePos;
  }
  return std::string(trim_view(view));
}

std::vector<ChannelSegment> parse_harmony_segments(const std::string &content)
{
  std::vector<ChannelSegment> segments;
  const std::string startToken = "<|start|>assistant<|channel|>";
  const std::string messageToken = "<|message|>";
  const std::string endToken = "<|end|>";

  std::size_t pos = 0;
  while (pos < content.size())
  {
    std::size_t start = content.find("<|start|>", pos);
    if (start == std::string::npos)
    {
      std::string trailing = content.substr(pos);
      if (!trailing.empty())
        segments.push_back(ChannelSegment{"", std::move(trailing), false});
      break;
    }

    if (start > pos)
    {
      std::string plain = content.substr(pos, start - pos);
      if (!plain.empty())
        segments.push_back(ChannelSegment{"", std::move(plain), false});
    }

    if (content.compare(start, startToken.size(), startToken) != 0)
    {
      // Not a harmony assistant marker; treat as literal character.
      std::string literal = content.substr(start, 1);
      segments.push_back(ChannelSegment{"", std::move(literal)});
      pos = start + 1;
      continue;
    }

    std::size_t channelStart = start + startToken.size();
    std::size_t messagePos = content.find(messageToken, channelStart);
    if (messagePos == std::string::npos)
    {
      std::string remainder = content.substr(start);
      if (!remainder.empty())
        segments.push_back(ChannelSegment{"", std::move(remainder), false});
      break;
    }

    std::string channel = content.substr(channelStart, messagePos - channelStart);
    std::size_t bodyStart = messagePos + messageToken.size();
    std::size_t endPos = content.find(endToken, bodyStart);
    if (endPos == std::string::npos)
    {
      std::string body = content.substr(bodyStart);
      segments.push_back(ChannelSegment{std::move(channel), std::move(body)});
      break;
    }

    std::string body = content.substr(bodyStart, endPos - bodyStart);
    segments.push_back(ChannelSegment{std::move(channel), std::move(body), true});
    pos = endPos + endToken.size();
  }

  return segments;
}

void applyStyleRange(std::vector<std::uint16_t> &styles,
                     std::size_t start,
                     std::size_t end,
                     std::uint16_t mask)
{
    if (start >= end)
        return;
    if (end > styles.size())
        end = styles.size();
    for (std::size_t i = start; i < end; ++i)
        styles[i] = static_cast<std::uint16_t>(styles[i] | mask);
}

std::vector<StyledLine> wrapStyledLine(const std::string &text,
                                       const std::vector<std::uint16_t> &styles,
                                       int width,
                                       bool hardWrap)
{
    std::vector<StyledLine> result;
    if (width <= 0)
        width = 1;

    std::size_t pos = 0;
    while (pos < text.size())
    {
        if (text[pos] == '\n')
        {
            StyledLine blank;
            blank.text = std::string();
            result.push_back(std::move(blank));
            ++pos;
            continue;
        }

        std::size_t lineStart = pos;
        std::size_t remaining = text.size() - pos;
        std::size_t take = 0;

        if (!hardWrap)
        {
            if (remaining <= static_cast<std::size_t>(width))
            {
                take = remaining;
            }
            else
            {
                std::size_t limit = pos + static_cast<std::size_t>(width);
                std::size_t wrapPos = limit;
                bool foundSpace = false;
                for (std::size_t i = pos; i < limit; ++i)
                {
                    char ch = text[i];
                    if (std::isspace(static_cast<unsigned char>(ch)))
                    {
                        wrapPos = i;
                        foundSpace = true;
                    }
                }
                if (foundSpace && wrapPos > pos)
                {
                    take = wrapPos - pos;
                }
                else
                {
                    take = static_cast<std::size_t>(width);
                }
            }
        }
        else
        {
            take = std::min<std::size_t>(remaining, static_cast<std::size_t>(width));
        }

        std::size_t lineEnd = pos + take;
        StyledLine line;
        line.text.assign(text.begin() + lineStart, text.begin() + lineEnd);
        line.styles.reserve(line.text.size());
        for (std::size_t i = lineStart; i < lineEnd; ++i)
            line.styles.push_back(styles[i]);
        result.push_back(std::move(line));

        pos = lineEnd;
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) && text[pos] != '\n')
            ++pos;
    }

    if (result.empty())
    {
        StyledLine blank;
        result.push_back(std::move(blank));
    }

    return result;
}

class MarkdownSegmentRenderer
{
public:
    explicit MarkdownSegmentRenderer(int wrapWidth)
        : width_(std::max(1, wrapWidth))
    {
    }

    std::vector<StyledLine> render(const std::string &text)
    {
        lines_.clear();
        tableBuffer_.clear();
        state_ = ck::edit::MarkdownParserState{};

        std::size_t offset = 0;
        while (offset <= text.size())
        {
            std::size_t newline = text.find('\n', offset);
            std::string line;
            if (newline == std::string::npos)
            {
                line = text.substr(offset);
                offset = text.size();
            }
            else
            {
                line = text.substr(offset, newline - offset);
                offset = newline + 1;
            }
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            processLine(line);
        }

        flushTable();
        return lines_;
    }

private:
    struct TableRow
    {
        std::string raw;
        ck::edit::MarkdownLineInfo info;
    };

    int width_;
    ck::edit::MarkdownAnalyzer analyzer_;
    ck::edit::MarkdownParserState state_{};
    std::vector<StyledLine> lines_;
    std::vector<TableRow> tableBuffer_;

    void processLine(const std::string &line)
    {
        auto info = analyzer_.analyzeLine(line, state_);

        if (info.kind == ck::edit::MarkdownLineKind::TableRow ||
            info.kind == ck::edit::MarkdownLineKind::TableSeparator)
        {
            tableBuffer_.push_back(TableRow{line, info});
            return;
        }

        flushTable();

        switch (info.kind)
        {
        case ck::edit::MarkdownLineKind::Blank:
            addBlankLine();
            break;
        case ck::edit::MarkdownLineKind::Heading:
            renderHeading(line, info);
            break;
        case ck::edit::MarkdownLineKind::BlockQuote:
            renderBlockQuote(line, info);
            break;
        case ck::edit::MarkdownLineKind::BulletListItem:
        case ck::edit::MarkdownLineKind::OrderedListItem:
            renderListItem(line, info);
            break;
        case ck::edit::MarkdownLineKind::CodeFenceStart:
        case ck::edit::MarkdownLineKind::CodeFenceEnd:
        case ck::edit::MarkdownLineKind::FencedCode:
        case ck::edit::MarkdownLineKind::IndentedCode:
            renderCode(line, info);
            break;
        case ck::edit::MarkdownLineKind::HorizontalRule:
            renderHorizontalRule();
            break;
        case ck::edit::MarkdownLineKind::Paragraph:
        case ck::edit::MarkdownLineKind::Html:
        case ck::edit::MarkdownLineKind::Unknown:
        default:
            renderParagraph(line, info);
            break;
        }
    }

    void addBlankLine()
    {
        StyledLine blank;
        lines_.push_back(std::move(blank));
    }

    void renderHeading(const std::string &line, const ck::edit::MarkdownLineInfo &info)
    {
        std::string content = info.inlineText;
        std::vector<std::uint16_t> styles(content.size(), kStyleHeading);
        applyInlineSpans(styles, 0, info.spans);
        appendWrapped(content, styles, false);
    }

    void renderParagraph(const std::string &line, const ck::edit::MarkdownLineInfo &info)
    {
        std::string content = info.inlineText;
        std::vector<std::uint16_t> styles(content.size(), kStyleNone);
        applyInlineSpans(styles, 0, info.spans);
        appendWrapped(content, styles, false);
    }

    void renderBlockQuote(const std::string &line, const ck::edit::MarkdownLineInfo &info)
    {
        std::string content = info.inlineText;

        std::string text = "> " + content;
        std::vector<std::uint16_t> styles(text.size(), kStyleQuote);
        applyStyleRange(styles, 0, 2, static_cast<std::uint16_t>(kStyleQuote | kStyleListMarker));
        if (!content.empty())
            applyInlineSpans(styles, 2, info.spans);
        appendWrapped(text, styles, false);
    }

    void renderListItem(const std::string &line, const ck::edit::MarkdownLineInfo &info)
    {
        std::string marker = info.kind == ck::edit::MarkdownLineKind::OrderedListItem ? info.marker : std::string();
        std::string content = info.inlineText;

        bool taskCompleted = false;
        if (info.isTask)
        {
            std::size_t bracket = content.find('[');
            if (bracket != std::string::npos && bracket + 1 < content.size())
            {
                char status = content[bracket + 1];
                taskCompleted = (status == 'x' || status == 'X');
            }
        }

        if (marker.empty())
            marker = info.isTask ? (taskCompleted ? "☑" : "☐") : "•";

        std::string text = marker + " " + content;
        std::vector<std::uint16_t> styles(text.size(), kStyleNone);
        applyStyleRange(styles, 0, marker.size(), static_cast<std::uint16_t>(kStyleListMarker | (info.isTask ? kStylePrefix : 0)));
        applyStyleRange(styles, marker.size(), marker.size() + 1, kStyleListMarker);
        applyInlineSpans(styles, marker.size() + 1, info.spans);
        appendWrapped(text, styles, false);
    }

    void renderCode(const std::string &line, const ck::edit::MarkdownLineInfo &info)
    {
        std::string text = line;
        if (info.kind == ck::edit::MarkdownLineKind::CodeFenceStart)
        {
            text = "```";
            if (!info.language.empty())
            {
                text.append(" ");
                text.append(info.language);
            }
        }
        else if (info.kind == ck::edit::MarkdownLineKind::CodeFenceEnd)
        {
            text = "```";
        }

        std::vector<std::uint16_t> styles(text.size(), kStyleCodeBlock);
        appendWrapped(text, styles, true);
    }

    void renderHorizontalRule()
    {
        std::string text(width_, '-');
        std::vector<std::uint16_t> styles(text.size(), kStyleCodeBlock);
        appendWrapped(text, styles, true);
    }

    void applyInlineSpans(std::vector<std::uint16_t> &styles,
                          std::size_t offset,
                          const std::vector<ck::edit::MarkdownSpan> &spans)
    {
        for (const auto &span : spans)
        {
            std::uint16_t mask = kStyleNone;
            switch (span.kind)
            {
            case ck::edit::MarkdownSpanKind::Bold:
                mask |= kStyleBold;
                break;
            case ck::edit::MarkdownSpanKind::Italic:
                mask |= kStyleItalic;
                break;
            case ck::edit::MarkdownSpanKind::BoldItalic:
                mask |= kStyleBold | kStyleItalic;
                break;
            case ck::edit::MarkdownSpanKind::Strikethrough:
                mask |= kStyleStrikethrough;
                break;
            case ck::edit::MarkdownSpanKind::Code:
                mask |= kStyleInlineCode;
                break;
            case ck::edit::MarkdownSpanKind::Link:
                mask |= kStyleLink;
                break;
            case ck::edit::MarkdownSpanKind::InlineHtml:
                mask |= kStyleLink;
                break;
            default:
                break;
            }
            if (mask != kStyleNone)
                applyStyleRange(styles, offset + span.start, offset + span.end, mask);
        }
    }

    void appendWrapped(const std::string &text,
                       const std::vector<std::uint16_t> &styles,
                       bool hardWrap)
    {
        auto wrapped = wrapStyledLine(text, styles, width_, hardWrap);
        lines_.insert(lines_.end(), wrapped.begin(), wrapped.end());
    }

    void flushTable()
    {
        if (tableBuffer_.empty())
            return;

        std::size_t columnCount = 0;
        for (const auto &row : tableBuffer_)
        {
            columnCount = std::max(columnCount, row.info.tableCells.size());
        }
        if (columnCount == 0)
        {
            tableBuffer_.clear();
            return;
        }

        std::vector<int> colWidths(columnCount, 1);
        for (const auto &row : tableBuffer_)
        {
            for (std::size_t i = 0; i < row.info.tableCells.size(); ++i)
            {
                const auto &cell = row.info.tableCells[i];
                colWidths[i] = std::max(colWidths[i], static_cast<int>(cell.text.size()));
            }
        }

        int borderWidth = static_cast<int>(columnCount) + 1;
        int totalWidth = borderWidth;
        for (int w : colWidths)
            totalWidth += w + 2;

        if (totalWidth > width_)
        {
            int available = width_ - borderWidth;
            if (available < static_cast<int>(columnCount))
                available = static_cast<int>(columnCount);
            for (std::size_t i = 0; i < colWidths.size(); ++i)
            {
                int maxAllow = std::max(1, available / static_cast<int>(columnCount));
                if (colWidths[i] > maxAllow)
                    colWidths[i] = maxAllow;
            }
        }

        auto makeBorder = [&](char corner, char lineChar) {
            std::string border;
            border.reserve(static_cast<std::size_t>(totalWidth));
            border.push_back(corner);
            for (std::size_t c = 0; c < columnCount; ++c)
            {
                border.append(colWidths[c] + 2, lineChar);
                border.push_back(corner);
            }
            std::vector<std::uint16_t> styles(border.size(), kStyleTableBorder);
            appendWrapped(border, styles, true);
        };

        makeBorder('+', '-');

        bool headerRendered = false;
        for (const auto &row : tableBuffer_)
        {
            bool isSeparator = row.info.kind == ck::edit::MarkdownLineKind::TableSeparator;
            if (isSeparator)
            {
                makeBorder('+', '=');
                headerRendered = true;
                continue;
            }

            bool headerStyle = row.info.isTableHeader && !headerRendered;

            std::vector<std::vector<std::string>> cellLines(columnCount);
            for (std::size_t c = 0; c < columnCount; ++c)
            {
                std::string cellText;
                if (c < row.info.tableCells.size())
                    cellText = row.info.tableCells[c].text;

                auto wrapped = wrapStyledLine(cellText,
                                              std::vector<std::uint16_t>(cellText.size(), kStyleNone),
                                              colWidths[c],
                                              false);
                cellLines[c].reserve(wrapped.size());
                for (auto &part : wrapped)
                {
                    std::string padded = part.text;
                    if (static_cast<int>(padded.size()) < colWidths[c])
                        padded.append(static_cast<std::size_t>(colWidths[c] - static_cast<int>(padded.size())), ' ');
                    cellLines[c].push_back(std::move(padded));
                }
                if (cellLines[c].empty())
                    cellLines[c].push_back(std::string(static_cast<std::size_t>(colWidths[c]), ' '));
            }

                std::size_t rowHeight = 0;
                for (const auto &cl : cellLines)
                    rowHeight = std::max(rowHeight, cl.size());

                for (std::size_t r = 0; r < rowHeight; ++r)
            {
                std::string line;
                std::vector<std::uint16_t> styles;
                line.reserve(static_cast<std::size_t>(totalWidth));
                styles.reserve(static_cast<std::size_t>(totalWidth));

                    line.push_back('|');
                    styles.push_back(kStyleTableBorder);

                    for (std::size_t c = 0; c < columnCount; ++c)
                    {
                        line.push_back(' ');
                        styles.push_back(headerStyle ? kStyleTableHeader : kStyleTableCell);

                        const auto &cl = cellLines[c];
                        const std::string &cellLine = r < cl.size() ? cl[r] : std::string(static_cast<std::size_t>(colWidths[c]), ' ');
                        for (char ch : cellLine)
                        {
                            line.push_back(ch);
                            styles.push_back(headerStyle ? kStyleTableHeader : kStyleTableCell);
                        }

                        line.push_back(' ');
                        styles.push_back(headerStyle ? kStyleTableHeader : kStyleTableCell);

                        line.push_back('|');
                        styles.push_back(kStyleTableBorder);
                    }

                appendWrapped(line, styles, true);
            }

            if (row.info.isTableHeader && !headerRendered)
                headerRendered = true;
        }

        makeBorder('+', '-');
        tableBuffer_.clear();
    }
};

std::vector<StyledLine> render_markdown_to_styled_lines(const std::string &text, int wrapWidth)
{
    MarkdownSegmentRenderer renderer(wrapWidth);
    return renderer.render(text);
}

TColorAttr applyStyleToAttr(TColorAttr base, std::uint16_t mask)
{
    TColorAttr attr = base;

    auto setFg = [&](int code) { setFore(attr, TColorDesired(TColorBIOS(code))); };
    auto setBg = [&](int code) { setBack(attr, TColorDesired(TColorBIOS(code))); };

    int fg = -1;
    int bg = -1;

    auto chooseFg = [&](int code) {
        if (fg == -1)
            fg = code;
    };

    if (mask & kStyleTableBorder)
        chooseFg(0x08);
    if (mask & kStyleTableHeader)
        chooseFg(0x0E);
    if (mask & kStyleTableCell)
        chooseFg(0x07);
    if (mask & kStyleHeading)
        chooseFg(0x0E);
    if (mask & kStyleInlineCode)
    {
        chooseFg(0x0A);
        bg = 0x01;
    }
    if (mask & kStyleCodeBlock)
    {
        chooseFg(0x0A);
        bg = 0x01;
    }
    if (mask & kStyleLink)
        chooseFg(0x09);
    if (mask & kStyleQuote)
        chooseFg(0x0B);
    if (mask & kStyleListMarker)
        chooseFg(0x0D);
    if (mask & kStyleStrikethrough)
        chooseFg(0x08);
    if (mask & kStyleItalic)
        chooseFg(0x0C);
    if ((mask & kStyleBold) && fg == -1)
        fg = 0x0F;
    if (mask & kStylePrefix)
        fg = 0x0C;

    if (fg != -1)
        setFg(fg);
    if (bg != -1)
        setBg(bg);

    return attr;
}

} // namespace

ChatTranscriptView::ChatTranscriptView(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll)
    : TScroller(bounds, hScroll, vScroll)
{
    options |= ofFirstClick;
    growMode = gfGrowHiX | gfGrowHiY;
    setLimit(1, 1);
}

void ChatTranscriptView::setMessages(const std::vector<ck::chat::ChatSession::Message> &sessionMessages)
{
    messages.clear();
    messages.reserve(sessionMessages.size());
    for (const auto &msg : sessionMessages)
    {
        messages.push_back(Message{msg.role, msg.content, msg.pending});
    }
    layoutDirty = true;
    rebuildLayout();
}

void ChatTranscriptView::clearMessages()
{
    messages.clear();
    rows.clear();
    layoutDirty = true;
    setLimit(1, 1);
    scrollTo(0, 0);
    drawView();
    notifyLayoutChanged();
}

void ChatTranscriptView::scrollToBottom()
{
    rebuildLayoutIfNeeded();
    int totalRows = static_cast<int>(rows.size());
    if (totalRows <= 0)
        totalRows = 1;
    int desired = std::max(0, totalRows - size.y);
    scrollTo(delta.x, desired);
    notifyLayoutChanged();
}

void ChatTranscriptView::setLayoutChangedCallback(std::function<void()> cb)
{
    layoutChangedCallback = std::move(cb);
}

void ChatTranscriptView::setHiddenDetailCallback(
    std::function<void(std::size_t, const std::string &, const std::string &)> cb)
{
    hiddenDetailCallback_ = std::move(cb);
}

bool ChatTranscriptView::messageForCopy(std::size_t index, std::string &out) const
{
    if (index >= messages.size())
        return false;
    const auto &msg = messages[index];
    if (msg.role != Role::Assistant)
        return false;
    auto segments = parse_harmony_segments(msg.content);
    if (!segments.empty())
    {
        std::string assembled;
        for (const auto &segment : segments)
        {
            if (is_final_channel(segment.channel))
                assembled += segment.text;
        }
        if (!assembled.empty())
        {
            out = std::move(assembled);
            return true;
        }
    }
    out = msg.content;
    return true;
}

void ChatTranscriptView::setMessagePending(std::size_t index, bool pending)
{
    if (index >= messages.size())
        return;
    messages[index].pending = pending;
}

bool ChatTranscriptView::isMessagePending(std::size_t index) const
{
    if (index >= messages.size())
        return false;
    return messages[index].pending;
}

std::optional<int> ChatTranscriptView::firstRowForMessage(std::size_t index) const
{
    for (std::size_t row = 0; row < rows.size(); ++row)
    {
        if (rows[row].messageIndex == index && rows[row].isFirstLine)
            return static_cast<int>(row);
    }
    return std::nullopt;
}

void ChatTranscriptView::setShowThinking(bool show)
{
    if (showThinking_ == show)
        return;
    showThinking_ = show;
    layoutDirty = true;
    rebuildLayoutIfNeeded();
    drawView();
    notifyLayoutChanged();
}

void ChatTranscriptView::setShowAnalysis(bool show)
{
    if (showAnalysis_ == show)
        return;
    showAnalysis_ = show;
    layoutDirty = true;
    rebuildLayoutIfNeeded();
    drawView();
    notifyLayoutChanged();
}

void ChatTranscriptView::draw()
{
    rebuildLayoutIfNeeded();

    auto colors = getColor(1);
    TColorAttr baseAttr = colors[0];

    TDrawBuffer buffer;
    for (int y = 0; y < size.y; ++y)
    {
        buffer.moveChar(0, ' ', baseAttr, size.x);
        std::size_t rowIndex = static_cast<std::size_t>(delta.y + y);
        if (rowIndex < rows.size())
        {
            const auto &row = rows[rowIndex];
            TColorAttr attr = baseAttr;
            if (row.role == Role::Assistant)
            {
                if (row.isThinking)
                    setFore(attr, TColorDesired(TColorBIOS(0x08)));
                else
                    setFore(attr, TColorDesired(TColorBIOS(0x01)));
            }
            if (!row.text.empty())
            {
                if (row.styleMask.empty())
                {
                    buffer.moveStr(0, row.text.c_str(), attr);
                }
                else
                {
                    std::size_t pos = 0;
                    while (pos < row.text.size())
                    {
                        std::size_t end = pos + 1;
                        std::uint16_t mask = row.styleMask[pos];
                        while (end < row.text.size() && row.styleMask[end] == mask)
                            ++end;
                        TColorAttr runAttr = (mask == 0) ? attr : applyStyleToAttr(attr, mask);
                        TStringView fragment(row.text.c_str() + pos, end - pos);
                        buffer.moveStr(static_cast<int>(pos), fragment, runAttr);
                        pos = end;
                    }
                }
            }
        }
        writeLine(0, y, size.x, 1, buffer);
    }
}

void ChatTranscriptView::changeBounds(const TRect &bounds)
{
    TScroller::changeBounds(bounds);
    layoutDirty = true;
    rebuildLayoutIfNeeded();
    notifyLayoutChanged();
}

void ChatTranscriptView::handleEvent(TEvent &event)
{
    TPoint before = delta;
    TScroller::handleEvent(event);
    if (before.x != delta.x || before.y != delta.y)
        notifyLayoutChanged();
}

std::string ChatTranscriptView::prefixForRole(Role role)
{
    switch (role)
    {
    case Role::User:
        return "You: ";
    case Role::Assistant:
        return "Assistant: ";
    default:
        return std::string{};
    }
}

void ChatTranscriptView::rebuildLayoutIfNeeded()
{
    if (!layoutDirty)
        return;
    rebuildLayout();
}

void ChatTranscriptView::rebuildLayout()
{
    rows.clear();
    int width = std::max(1, size.x);
    spinnerFrame_ = (spinnerFrame_ + 1) % 4;

    for (std::size_t i = 0; i < messages.size(); ++i)
    {
        const auto &msg = messages[i];
        bool messageFirstRow = true;

        if (msg.role == Role::Assistant)
        {
            auto segments = parse_harmony_segments(msg.content);
            bool hasMarker = std::any_of(segments.begin(), segments.end(),
                                         [](const ChannelSegment &seg)
                                         { return seg.from_marker; });
            bool finalSeen = false;

            struct HiddenAggregate
            {
                std::string channel;
                std::string text;
                bool pending = false;
                bool thinking = true;
            };
            std::vector<HiddenAggregate> hidden;
            std::unordered_map<std::string, std::size_t> hiddenIndex;

            auto recordHidden = [&](const std::string &channelLabel,
                                    const std::string &segmentText,
                                    bool thinking)
            {
                std::string key = channelLabel.empty() ? std::string("analysis") : channelLabel;
                auto it = hiddenIndex.find(key);
                if (it == hiddenIndex.end())
                {
                    hidden.push_back(HiddenAggregate{key, std::string(), msg.pending, thinking});
                    it = hiddenIndex.emplace(key, hidden.size() - 1).first;
                }
                auto &agg = hidden[it->second];
                if (!agg.text.empty())
                    agg.text.push_back('\n');
                agg.text += segmentText;
                agg.pending = msg.pending;
                agg.thinking = thinking;
            };

            auto handleVisibleSegment = [&](const std::string &channel, const std::string &text,
                                            bool thinking)
            {
                std::string label = channel.empty() ? std::string("analysis") : channel;
                std::string prefix = thinking ? "Assistant (" + label + "): " : "Assistant: ";
                appendVisibleSegment(Role::Assistant, prefix, text, i, messageFirstRow, width, thinking, label);
            };

            auto normalizedChannel = [](const std::string &label) {
                std::string printable = sanitize_for_display(label);
                if (printable.empty())
                    printable = "analysis";
                return printable;
            };

            std::function<bool(const std::string &, bool)> shouldHide = [&](const std::string &channelLower,
                                                                            bool thinking) {
                if (channelLower == "analysis")
                    return !showAnalysis_;
                if (thinking)
                    return !showThinking_;
                return false;
            };

            if (segments.empty())
            {
                std::string original = msg.content;
                std::string channelLabel;

                const std::string channelToken = "<|channel|>";
                const std::string messageToken = "<|message|>";

                auto channelPos = original.find(channelToken);
                if (channelPos != std::string::npos)
                {
                    std::size_t headerStart = channelPos + channelToken.size();
                    auto messagePos = original.find(messageToken, headerStart);
                    if (messagePos != std::string::npos)
                    {
                        channelLabel = trim_copy(original.substr(headerStart, messagePos - headerStart));
                        original.erase(channelPos, (messagePos + messageToken.size()) - channelPos);
                    }
                }

                std::size_t endTag = original.find("<|end|");
                if (endTag != std::string::npos)
                    original.erase(endTag);

                std::size_t startTag = original.find("<|start|");
                if (startTag != std::string::npos)
                    original.erase(startTag, std::string::npos);

                std::string cleaned = sanitize_for_display(original);

                bool thinkingSegment = hasMarker || !channelLabel.empty() ||
                                        msg.content.find("<|channel|") != std::string::npos;
                std::string printableChannel = normalizedChannel(channelLabel);
                std::string channelLower = to_lower_copy(printableChannel);

                if (shouldHide(channelLower, thinkingSegment))
                {
                    recordHidden(printableChannel, cleaned, thinkingSegment);
                    messageFirstRow = false;
                }
                else
                {
                    handleVisibleSegment(printableChannel, cleaned, thinkingSegment);
                }
            }
            else
            {
                for (const auto &segment : segments)
                {
                    std::string channelLabel = trim_copy(segment.channel);
                    std::string text = segment.text;

                    if (channelLabel.empty())
                    {
                        const std::string channelToken = "<|channel|>";
                        const std::string messageToken = "<|message|>";
                        auto channelPos = text.find(channelToken);
                        if (channelPos != std::string::npos)
                        {
                            std::size_t headerStart = channelPos + channelToken.size();
                            auto messagePos = text.find(messageToken, headerStart);
                            if (messagePos != std::string::npos)
                            {
                                channelLabel = trim_copy(text.substr(headerStart, messagePos - headerStart));
                                text.erase(channelPos, (messagePos + messageToken.size()) - channelPos);
                            }
                        }
                        auto endTag = text.find("<|end|");
                        if (endTag != std::string::npos)
                            text.erase(endTag);
                        auto startTag = text.find("<|start|");
                        if (startTag != std::string::npos)
                            text.erase(startTag, std::string::npos);
                    }

                    bool finalSegment = (!channelLabel.empty() && is_final_channel(channelLabel));
                    bool thinkingSegment = channelLabel.empty()
                                                ? (segment.from_marker && !finalSegment) || (hasMarker && !finalSeen)
                                                : !finalSegment;

                    std::string printableChannel = normalizedChannel(channelLabel);
                    std::string channelLower = to_lower_copy(printableChannel);
                    std::string segmentText = sanitize_for_display(text);

                    if (shouldHide(channelLower, thinkingSegment))
                    {
                        recordHidden(printableChannel, segmentText, thinkingSegment);
                        messageFirstRow = false;
                    }
                    else
                    {
                        handleVisibleSegment(printableChannel, segmentText, thinkingSegment);
                    }

                    if ((segment.from_marker || !channelLabel.empty()) && finalSegment)
                        finalSeen = true;
                }
            }

            for (auto &agg : hidden)
            {
                std::string trimmed = trim_copy(agg.text);
                appendHiddenPlaceholder("Assistant (" + agg.channel + "): ", agg.channel,
                                        std::string(trimmed), i, agg.pending, agg.thinking);
                messageFirstRow = false;
            }
        }
        else
        {
            std::string prefix = prefixForRole(msg.role);
            std::string sanitized = sanitize_for_display(msg.content);
            appendVisibleSegment(msg.role, prefix, sanitized, i, messageFirstRow, width, false, std::string());
        }

        if (i + 1 < messages.size())
        {
            DisplayRow spacer;
            spacer.role = Role::System;
            spacer.text = std::string();
            spacer.messageIndex = i;
            spacer.isFirstLine = false;
            rows.push_back(std::move(spacer));
        }
    }

    int total = static_cast<int>(rows.size());
    if (total <= 0)
        total = 1;
    setLimit(1, total);
    layoutDirty = false;
    notifyLayoutChanged();
}

void ChatTranscriptView::appendVisibleSegment(Role role,
                                              const std::string &prefix,
                                              const std::string &text,
                                              std::size_t messageIndex,
                                              bool &messageFirstRow,
                                              int width,
                                              bool thinking,
                                              const std::string &channelLabel)
{
    int contentWidth = width - static_cast<int>(prefix.size());
    if (contentWidth < 1)
        contentWidth = 1;

    auto styled = render_markdown_to_styled_lines(text, contentWidth);
    if (styled.empty())
        styled.push_back(StyledLine{});

    std::string indent(prefix.size(), ' ');

    for (std::size_t idx = 0; idx < styled.size(); ++idx)
    {
        const StyledLine &line = styled[idx];
        DisplayRow row;
        row.role = role;
        row.messageIndex = messageIndex;
        row.isFirstLine = messageFirstRow && idx == 0;
        row.isThinking = thinking;
        row.channelLabel = channelLabel;

        const std::string &prefixToUse = (idx == 0) ? prefix : indent;
        row.text = prefixToUse + line.text;
        row.styleMask.assign(row.text.size(), 0);
        if (!prefixToUse.empty())
            applyStyleRange(row.styleMask, 0, prefixToUse.size(), static_cast<std::uint16_t>(kStylePrefix));

        for (std::size_t i = 0; i < line.text.size(); ++i)
        {
            if (prefixToUse.size() + i < row.styleMask.size())
                row.styleMask[prefixToUse.size() + i] = line.styles.size() > i ? line.styles[i] : 0;
        }

        rows.push_back(std::move(row));
        messageFirstRow = false;
    }
}

void ChatTranscriptView::appendHiddenPlaceholder(const std::string &prefix,
                                                 const std::string &channelLabel,
                                                 const std::string &content,
                                                 std::size_t messageIndex,
                                                 bool pending,
                                                 bool thinking)
{
    DisplayRow row;
    row.role = Role::Assistant;
    row.messageIndex = messageIndex;
    row.isFirstLine = true;
    row.isThinking = thinking;
    row.isPlaceholder = true;
    row.isPending = pending;
    row.channelLabel = channelLabel;
    row.hiddenContent = content.empty() ? std::string("(no content)") : content;

    static const char spinnerChars[] = {'|', '/', '-', '\\'};
    if (pending)
    {
        char frame = spinnerChars[spinnerFrame_ % (sizeof(spinnerChars) / sizeof(spinnerChars[0]))];
        row.text = prefix + "Generating… " + frame;
    }
    else
    {
        row.text = prefix + "[Analysis finished – click to view]";
    }
    row.styleMask.assign(row.text.size(), 0);
    if (!prefix.empty())
        applyStyleRange(row.styleMask, 0, prefix.size(), kStylePrefix);

    rows.push_back(std::move(row));
}

void ChatTranscriptView::openHiddenRow(std::size_t rowIndex)
{
    if (rowIndex >= rows.size())
        return;
    const auto &row = rows[rowIndex];
    if (!row.isPlaceholder || row.isPending || !hiddenDetailCallback_)
        return;
    hiddenDetailCallback_(row.messageIndex, row.channelLabel, row.hiddenContent);
}

void ChatTranscriptView::notifyLayoutChanged()
{
    if (layoutChangedCallback)
        layoutChangedCallback();
}
