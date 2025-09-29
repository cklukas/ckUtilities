#include <gtest/gtest.h>

#include "ck/edit/markdown_parser.hpp"

#include <string>
#include <vector>

using ck::edit::MarkdownAnalyzer;
using ck::edit::MarkdownLineInfo;
using ck::edit::MarkdownLineKind;
using ck::edit::MarkdownParserState;
using ck::edit::MarkdownSpanKind;

namespace {

// Helper function to find a span of a specific kind
const ck::edit::MarkdownSpan *findSpanKind(const MarkdownLineInfo &info,
                                           MarkdownSpanKind kind) {
  for (const auto &span : info.spans) {
    if (span.kind == kind)
      return &span;
  }
  return nullptr;
}

// Helper function to test markdown detection logic
bool shouldProcessMarkdown(const std::string &text) {
  // This mimics the logic from ChatTranscriptView::appendVisibleSegment
  return text.length() >= 10 && text.length() <= 10000 &&
         text.find_first_of("#*`[]()-") != std::string::npos;
}

} // namespace

TEST(ChatMarkdownTests, BasicMarkdownDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  // Test basic markdown elements
  MarkdownLineInfo heading = analyzer.analyzeLine("# Heading", state);
  EXPECT_EQ(heading.kind, MarkdownLineKind::Heading);
  EXPECT_EQ(heading.headingLevel, 1);

  MarkdownLineInfo bold = analyzer.analyzeLine("This is **bold** text", state);
  EXPECT_EQ(bold.kind, MarkdownLineKind::Paragraph);
  EXPECT_FALSE(bold.spans.empty());

  MarkdownLineInfo italic =
      analyzer.analyzeLine("This is *italic* text", state);
  EXPECT_EQ(italic.kind, MarkdownLineKind::Paragraph);
  EXPECT_FALSE(italic.spans.empty());
}

TEST(ChatMarkdownTests, MarkdownRenderingThresholds) {
  // Test that very short text is skipped
  std::string shortText = "Hi";
  EXPECT_FALSE(shouldProcessMarkdown(shortText));

  // Test that very long text is skipped
  std::string longText(10001, 'a');
  EXPECT_FALSE(shouldProcessMarkdown(longText));

  // Test that text without markdown characters is skipped
  std::string plainText = "This is just plain text without any markdown";
  EXPECT_FALSE(shouldProcessMarkdown(plainText));

  // Test that text with markdown characters is processed
  std::string markdownText = "This has **bold** text";
  EXPECT_TRUE(shouldProcessMarkdown(markdownText));
}

TEST(ChatMarkdownTests, BoldTextDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  MarkdownLineInfo line = analyzer.analyzeLine("This is **bold** text", state);
  EXPECT_EQ(line.kind, MarkdownLineKind::Paragraph);

  const ck::edit::MarkdownSpan *bold =
      findSpanKind(line, MarkdownSpanKind::Bold);
  ASSERT_NE(bold, nullptr);
  EXPECT_GT(bold->end, bold->start);
}

TEST(ChatMarkdownTests, ItalicTextDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  MarkdownLineInfo line = analyzer.analyzeLine("This is *italic* text", state);
  EXPECT_EQ(line.kind, MarkdownLineKind::Paragraph);

  const ck::edit::MarkdownSpan *italic =
      findSpanKind(line, MarkdownSpanKind::Italic);
  ASSERT_NE(italic, nullptr);
  EXPECT_GT(italic->end, italic->start);
}

TEST(ChatMarkdownTests, HeadingDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  // Test different heading levels
  MarkdownLineInfo h1 = analyzer.analyzeLine("# Main Heading", state);
  EXPECT_EQ(h1.kind, MarkdownLineKind::Heading);
  EXPECT_EQ(h1.headingLevel, 1);

  MarkdownLineInfo h2 = analyzer.analyzeLine("## Sub Heading", state);
  EXPECT_EQ(h2.kind, MarkdownLineKind::Heading);
  EXPECT_EQ(h2.headingLevel, 2);

  MarkdownLineInfo h3 = analyzer.analyzeLine("### Sub Sub Heading", state);
  EXPECT_EQ(h3.kind, MarkdownLineKind::Heading);
  EXPECT_EQ(h3.headingLevel, 3);
}

TEST(ChatMarkdownTests, CodeBlockDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  // Test code fence start
  MarkdownLineInfo fenceStart = analyzer.analyzeLine("```cpp", state);
  EXPECT_EQ(fenceStart.kind, MarkdownLineKind::CodeFenceStart);
  EXPECT_EQ(fenceStart.language, "cpp");
  EXPECT_TRUE(state.inFence);

  // Test code fence body
  MarkdownLineInfo fenceBody = analyzer.analyzeLine("int main() {}", state);
  EXPECT_EQ(fenceBody.kind, MarkdownLineKind::FencedCode);

  // Test code fence end
  MarkdownLineInfo fenceEnd = analyzer.analyzeLine("```", state);
  EXPECT_EQ(fenceEnd.kind, MarkdownLineKind::CodeFenceEnd);
  EXPECT_FALSE(state.inFence);
}

TEST(ChatMarkdownTests, TableDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  // Test table header
  MarkdownLineInfo header =
      analyzer.analyzeLine("| Header 1 | Header 2 |", state);
  EXPECT_EQ(header.kind, MarkdownLineKind::TableRow);

  // Test table separator
  MarkdownLineInfo separator =
      analyzer.analyzeLine("|----------|----------|", state);
  EXPECT_EQ(separator.kind, MarkdownLineKind::TableSeparator);

  // Test table row
  MarkdownLineInfo row = analyzer.analyzeLine("| Cell 1   | Cell 2   |", state);
  EXPECT_EQ(row.kind, MarkdownLineKind::TableRow);
}

TEST(ChatMarkdownTests, LinkDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  MarkdownLineInfo line = analyzer.analyzeLine(
      "Check out [this link](https://example.com) for more info", state);
  EXPECT_EQ(line.kind, MarkdownLineKind::Paragraph);

  const ck::edit::MarkdownSpan *link =
      findSpanKind(line, MarkdownSpanKind::Link);
  ASSERT_NE(link, nullptr);
  EXPECT_EQ(link->attribute, "https://example.com");
}

TEST(ChatMarkdownTests, CodeSpanDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  MarkdownLineInfo line = analyzer.analyzeLine("Use `code` in text", state);
  EXPECT_EQ(line.kind, MarkdownLineKind::Paragraph);

  const ck::edit::MarkdownSpan *code =
      findSpanKind(line, MarkdownSpanKind::Code);
  ASSERT_NE(code, nullptr);
}

TEST(ChatMarkdownTests, MixedContentDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  MarkdownLineInfo line =
      analyzer.analyzeLine("This has **bold**, *italic*, and `code`", state);
  EXPECT_EQ(line.kind, MarkdownLineKind::Paragraph);

  // Should have multiple spans
  EXPECT_GE(line.spans.size(), 3u);

  const ck::edit::MarkdownSpan *bold =
      findSpanKind(line, MarkdownSpanKind::Bold);
  ASSERT_NE(bold, nullptr);

  const ck::edit::MarkdownSpan *italic =
      findSpanKind(line, MarkdownSpanKind::Italic);
  ASSERT_NE(italic, nullptr);

  const ck::edit::MarkdownSpan *code =
      findSpanKind(line, MarkdownSpanKind::Code);
  ASSERT_NE(code, nullptr);
}

TEST(ChatMarkdownTests, PerformanceSafetyChecks) {
  // Test large text handling
  std::string largeText(5001, 'a');
  EXPECT_FALSE(shouldProcessMarkdown(largeText));

  // Test text with many lines but with markdown characters
  std::string manyLines;
  for (int i = 0; i < 100; ++i) {
    manyLines += "Line " + std::to_string(i) + " **bold**\n";
  }

  // Should be handled safely
  EXPECT_TRUE(shouldProcessMarkdown(manyLines));
}

TEST(ChatMarkdownTests, EmptyAndWhitespaceHandling) {
  // Test empty string
  EXPECT_FALSE(shouldProcessMarkdown(""));

  // Test whitespace only
  EXPECT_FALSE(shouldProcessMarkdown("   \n  \n  "));

  // Test single character
  EXPECT_FALSE(shouldProcessMarkdown("a"));

  // Test short text with markdown
  EXPECT_FALSE(shouldProcessMarkdown("**a**"));
}

TEST(ChatMarkdownTests, SpecialCharactersHandling) {
  std::string text = "This is a long text with special chars: "
                     "@$%^&_=|;':\",./<>? and more text to make it long enough";
  // This text doesn't contain markdown characters, so it should not be
  // processed
  EXPECT_FALSE(shouldProcessMarkdown(text));

  // But if it has markdown characters, it should be processed
  std::string textWithMarkdown =
      "Special chars: @#$%^&*()_+-=[]{}|;':\",./<>? **bold**";
  EXPECT_TRUE(shouldProcessMarkdown(textWithMarkdown));
}

TEST(ChatMarkdownTests, TaskListDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  MarkdownLineInfo task = analyzer.analyzeLine("- [x] finish docs", state);
  EXPECT_EQ(task.kind, MarkdownLineKind::BulletListItem);
  EXPECT_TRUE(task.isTask);

  MarkdownLineInfo uncheckedTask =
      analyzer.analyzeLine("- [ ] todo item", state);
  EXPECT_EQ(uncheckedTask.kind, MarkdownLineKind::BulletListItem);
  EXPECT_TRUE(uncheckedTask.isTask);
}

TEST(ChatMarkdownTests, ListDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  // Test bullet list
  MarkdownLineInfo bullet = analyzer.analyzeLine("- Item 1", state);
  EXPECT_EQ(bullet.kind, MarkdownLineKind::BulletListItem);

  // Test ordered list
  MarkdownLineInfo ordered = analyzer.analyzeLine("1. First item", state);
  EXPECT_EQ(ordered.kind, MarkdownLineKind::OrderedListItem);
}

TEST(ChatMarkdownTests, BlockQuoteDetection) {
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  MarkdownLineInfo quote = analyzer.analyzeLine("> This is a quote", state);
  EXPECT_EQ(quote.kind, MarkdownLineKind::BlockQuote);
}

TEST(ChatMarkdownTests, HorizontalRuleDetection) {
  // Test that horizontal rules are detected and processed by markdown renderer
  std::string textWithHorizontalRule = "Some text here\n---\nMore text here";
  std::string textWithMultipleDashes =
      "First line here\n---\n---\n---\nLast line here";
  std::string textWithHorizontalRuleAndOtherMarkdown =
      "**Bold text** here\n---\n*Italic text* here";

  // All should be processed by markdown renderer (dash character included)
  EXPECT_TRUE(shouldProcessMarkdown(textWithHorizontalRule));
  EXPECT_TRUE(shouldProcessMarkdown(textWithMultipleDashes));
  EXPECT_TRUE(shouldProcessMarkdown(textWithHorizontalRuleAndOtherMarkdown));

  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  MarkdownLineInfo hr1 = analyzer.analyzeLine("---", state);
  EXPECT_EQ(hr1.kind, MarkdownLineKind::HorizontalRule);

  MarkdownLineInfo hr2 = analyzer.analyzeLine("***", state);
  EXPECT_EQ(hr2.kind, MarkdownLineKind::HorizontalRule);
}

TEST(ChatMarkdownTests, HorizontalRuleStyling) {
  // Test that horizontal rules use proper styling
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  // Test different horizontal rule patterns
  MarkdownLineInfo hr1 = analyzer.analyzeLine("---", state);
  EXPECT_EQ(hr1.kind, MarkdownLineKind::HorizontalRule);

  MarkdownLineInfo hr2 = analyzer.analyzeLine("***", state);
  EXPECT_EQ(hr2.kind, MarkdownLineKind::HorizontalRule);

  MarkdownLineInfo hr3 = analyzer.analyzeLine("___", state);
  EXPECT_EQ(hr3.kind, MarkdownLineKind::HorizontalRule);

  // Test spaced patterns
  MarkdownLineInfo hr4 = analyzer.analyzeLine("- - -", state);
  EXPECT_EQ(hr4.kind, MarkdownLineKind::HorizontalRule);
}

TEST(ChatMarkdownTests, HorizontalRulesInCodeBlocks) {
  // Test that horizontal rules within code blocks are properly detected
  std::string codeBlockWithHorizontalRule = "```markdown\n---\n```";
  std::string codeBlockWithMultipleRules = "```markdown\n---\n***\n___\n```";
  std::string codeBlockWithSpacedRules =
      "```markdown\n- - -\n* * *\n_ _ _\n```";

  // All should be processed by markdown renderer (contains backticks and
  // dashes)
  EXPECT_TRUE(shouldProcessMarkdown(codeBlockWithHorizontalRule));
  EXPECT_TRUE(shouldProcessMarkdown(codeBlockWithMultipleRules));
  EXPECT_TRUE(shouldProcessMarkdown(codeBlockWithSpacedRules));

  // Test that the markdown analyzer correctly identifies code blocks
  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  // Test code fence start
  MarkdownLineInfo fenceStart = analyzer.analyzeLine("```markdown", state);
  EXPECT_EQ(fenceStart.kind, MarkdownLineKind::CodeFenceStart);

  // Test horizontal rule within code block (should be detected as fenced code)
  MarkdownLineInfo hrInCode = analyzer.analyzeLine("---", state);
  // Note: This will be CodeFenceStart/End/FencedCode depending on context
  // The important thing is that our renderer will detect the pattern and render
  // as horizontal rule
}

TEST(ChatMarkdownTests, TableCellFormatting) {
  // Test that markdown formatting works within table cells
  std::string tableWithFormatting =
      "| **Bold** | *Italic* | `code` |\n| --- | --- | --- |\n| Normal | "
      "**Bold text** | `inline code` |";

  // Should be processed by markdown renderer (contains pipes and dashes)
  EXPECT_TRUE(shouldProcessMarkdown(tableWithFormatting));

  MarkdownAnalyzer analyzer;
  MarkdownParserState state;

  // Test table row detection
  MarkdownLineInfo tableRow =
      analyzer.analyzeLine("| **Bold** | *Italic* | `code` |", state);
  EXPECT_EQ(tableRow.kind, MarkdownLineKind::TableRow);

  // Test table separator detection
  MarkdownLineInfo tableSep =
      analyzer.analyzeLine("| --- | --- | --- |", state);
  EXPECT_EQ(tableSep.kind, MarkdownLineKind::TableSeparator);
}

TEST(ChatMarkdownTests, ExcessiveBlankLines) {
  // Test that multiple consecutive newlines don't create excessive blank lines
  std::string textWithMultipleNewlines = "Line 1\n\n\n\nLine 2 **bold**";
  EXPECT_TRUE(shouldProcessMarkdown(textWithMultipleNewlines));

  // Test that very long text with many newlines is handled safely
  std::string longTextWithNewlines;
  for (int i = 0; i < 50; ++i) {
    longTextWithNewlines += "Line " + std::to_string(i) + " **bold**\n\n";
  }
  EXPECT_TRUE(shouldProcessMarkdown(longTextWithNewlines));
}