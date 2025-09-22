#include <gtest/gtest.h>

#include "ck/edit/markdown_parser.hpp"

#include <string>

using ck::edit::MarkdownAnalyzer;
using ck::edit::MarkdownLineInfo;
using ck::edit::MarkdownLineKind;
using ck::edit::MarkdownParserState;
using ck::edit::MarkdownSpan;
using ck::edit::MarkdownSpanKind;

namespace
{

const MarkdownSpan *findSpanKind(const MarkdownLineInfo &info, MarkdownSpanKind kind)
{
    for (const auto &span : info.spans)
    {
        if (span.kind == kind)
            return &span;
    }
    return nullptr;
}

} // namespace

TEST(MarkdownParser, DetectsHeadingsAndTasks)
{
    MarkdownAnalyzer analyzer;
    MarkdownParserState state;

    MarkdownLineInfo heading = analyzer.analyzeLine("## Heading", state);
    EXPECT_EQ(heading.kind, MarkdownLineKind::Heading);
    EXPECT_EQ(heading.headingLevel, 2);

    MarkdownLineInfo task = analyzer.analyzeLine("- [x] finish docs", state);
    EXPECT_EQ(task.kind, MarkdownLineKind::BulletListItem);
    EXPECT_TRUE(task.isTask);
}

TEST(MarkdownParser, TracksCodeFences)
{
    MarkdownAnalyzer analyzer;
    MarkdownParserState state;

    MarkdownLineInfo fenceStart = analyzer.analyzeLine("```cpp", state);
    EXPECT_EQ(fenceStart.kind, MarkdownLineKind::CodeFenceStart);
    EXPECT_EQ(fenceStart.language, "cpp");
    EXPECT_TRUE(state.inFence);

    MarkdownLineInfo fenceBody = analyzer.analyzeLine("int main() {}", state);
    EXPECT_EQ(fenceBody.kind, MarkdownLineKind::FencedCode);

    MarkdownLineInfo fenceEnd = analyzer.analyzeLine("```", state);
    EXPECT_EQ(fenceEnd.kind, MarkdownLineKind::CodeFenceEnd);
    EXPECT_FALSE(state.inFence);
}

TEST(MarkdownParser, IdentifiesInlineSpans)
{
    MarkdownAnalyzer analyzer;
    MarkdownParserState state;
    MarkdownLineInfo line = analyzer.analyzeLine("This has **bold** text and `code` plus [link](https://example.com)", state);

    const MarkdownSpan *bold = findSpanKind(line, MarkdownSpanKind::Bold);
    ASSERT_NE(bold, nullptr);
    EXPECT_GT(bold->end, bold->start);

    const MarkdownSpan *code = findSpanKind(line, MarkdownSpanKind::Code);
    ASSERT_NE(code, nullptr);
    EXPECT_EQ(line.spans.size(), 3u);

    const MarkdownSpan *link = findSpanKind(line, MarkdownSpanKind::Link);
    ASSERT_NE(link, nullptr);
    EXPECT_EQ(link->attribute, "https://example.com");
}
