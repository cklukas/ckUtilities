#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ck::edit
{

enum class MarkdownLineKind
{
    Blank,
    Heading,
    BlockQuote,
    BulletListItem,
    OrderedListItem,
    TaskListItem,
    CodeFenceStart,
    CodeFenceEnd,
    FencedCode,
    IndentedCode,
    HorizontalRule,
    TableSeparator,
    TableRow,
    Paragraph,
    Html,
    ThematicBreak,
    Unknown
};

enum class MarkdownSpanKind
{
    Bold,
    Italic,
    BoldItalic,
    Strikethrough,
    Code,
    Link,
    Image,
    InlineHtml,
    PlainText
};

struct MarkdownSpan
{
    MarkdownSpanKind kind = MarkdownSpanKind::PlainText;
    std::size_t start = 0;
    std::size_t end = 0;
    std::string label;
    std::string attribute;
};

enum class MarkdownTableAlignment
{
    Default,
    Left,
    Center,
    Right,
    Number
};

struct MarkdownTableCell
{
    std::size_t startColumn = 0;
    std::size_t endColumn = 0;
    std::string text;
};

struct MarkdownLineInfo
{
    MarkdownLineKind kind = MarkdownLineKind::Unknown;
    int headingLevel = 0;
    bool isTask = false;
    bool inFence = false;
    bool fenceCloses = false;
    bool fenceOpens = false;
    bool isOrdered = false;
    std::string marker;
    std::string language;
    std::string fenceLabel;
    std::vector<MarkdownSpan> spans;
    std::vector<MarkdownTableCell> tableCells;
    std::vector<MarkdownTableAlignment> tableAlignments;
    bool isTableHeader = false;
    int tableRowIndex = 0;
    std::string inlineText;
};

struct MarkdownParserState
{
    bool inFence = false;
    std::string fenceMarker;
    bool fenceIndented = false;
    bool tableActive = false;
    bool tableHeaderConfirmed = false;
    int tableRowCounter = 0;
    std::vector<MarkdownTableAlignment> tableAlignments;
    std::string fenceLabel;
    std::string fenceLanguage;
};

class MarkdownAnalyzer
{
public:
    MarkdownAnalyzer() = default;

    MarkdownParserState computeStateBefore(const std::string &text);
    MarkdownLineInfo analyzeLine(const std::string &line, MarkdownParserState &state) const;
    const MarkdownSpan *spanAtColumn(const MarkdownLineInfo &info, std::size_t column) const;
    std::string describeLine(const MarkdownLineInfo &info) const;
    std::string describeSpan(const MarkdownSpan &span) const;
    std::string describeTableCell(const MarkdownLineInfo &info, std::size_t column) const;

private:
    static bool isHorizontalRule(const std::string &trimmed) noexcept;
    static bool isTableSeparator(const std::string &trimmed) noexcept;
    static std::vector<MarkdownTableCell> parseTableRow(const std::string &line);
    static std::vector<MarkdownTableAlignment> parseAlignmentRow(const std::string &line);
    static std::string trimLeft(std::string_view view) noexcept;
    static std::string trimRight(std::string_view view) noexcept;
    static std::string trim(std::string_view view) noexcept;
    static bool isHtmlBlockStart(const std::string &trimmed) noexcept;
    MarkdownLineInfo analyzeFencedState(const std::string &line, MarkdownParserState &state) const;
    void parseInline(const std::string &line, MarkdownLineInfo &info) const;
    void parseEmphasis(const std::string &line, std::vector<MarkdownSpan> &spans) const;
    void parseCodeSpans(const std::string &line, std::vector<MarkdownSpan> &spans) const;
    void parseLinksAndImages(const std::string &line, std::vector<MarkdownSpan> &spans) const;
    void parseInlineHtml(const std::string &line, std::vector<MarkdownSpan> &spans) const;
};

} // namespace ck::edit
