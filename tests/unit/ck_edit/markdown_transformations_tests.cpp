#include <gtest/gtest.h>

#include "ck/edit/markdown_transformations.hpp"

namespace
{

using ck::edit::MarkdownByteRange;
using ck::edit::MarkdownInlineStyle;
using ck::edit::toggle_markdown_heading;
using ck::edit::toggle_markdown_inline_style;

TEST(MarkdownTransformations, TogglesInlineStylesAndRestoresTheContentSelection)
{
    constexpr std::string_view source = "before caf\xC3\xA9 after";
    const MarkdownByteRange selected{7, 12};

    const auto bold = toggle_markdown_inline_style(source, selected, MarkdownInlineStyle::Bold);
    ASSERT_TRUE(bold.has_value());
    EXPECT_EQ(bold->replaced, selected);
    EXPECT_EQ(bold->replacement, "**caf\xC3\xA9**");
    EXPECT_EQ(bold->selection, (MarkdownByteRange{9, 14}));

    const std::string wrapped = "before **caf\xC3\xA9** after";
    const auto unwrapped = toggle_markdown_inline_style(wrapped, {9, 14}, MarkdownInlineStyle::Bold);
    ASSERT_TRUE(unwrapped.has_value());
    EXPECT_EQ(unwrapped->replaced, (MarkdownByteRange{7, 16}));
    EXPECT_EQ(unwrapped->replacement, "caf\xC3\xA9");
    EXPECT_EQ(unwrapped->selection, selected);
}

TEST(MarkdownTransformations, ChoosesASafeInlineCodeDelimiter)
{
    const auto edit = toggle_markdown_inline_style("use token `inside` here", {4, 18}, MarkdownInlineStyle::Code);
    ASSERT_TRUE(edit.has_value());
    EXPECT_EQ(edit->replacement, "`` token `inside` ``");
    EXPECT_EQ(edit->selection, (MarkdownByteRange{7, 21}));

    constexpr std::string_view wrapped = "use `` token `inside` `` here";
    const auto unwrapped = toggle_markdown_inline_style(wrapped, {7, 21}, MarkdownInlineStyle::Code);
    ASSERT_TRUE(unwrapped.has_value());
    EXPECT_EQ(unwrapped->replaced, (MarkdownByteRange{4, 24}));
    EXPECT_EQ(unwrapped->replacement, "token `inside`");
    EXPECT_EQ(unwrapped->selection, (MarkdownByteRange{4, 18}));

    const auto fully_selected = toggle_markdown_inline_style(wrapped, {4, 24}, MarkdownInlineStyle::Code);
    ASSERT_TRUE(fully_selected.has_value());
    EXPECT_EQ(fully_selected->replacement, "token `inside`");
    EXPECT_EQ(fully_selected->selection, (MarkdownByteRange{4, 18}));
}

TEST(MarkdownTransformations, TogglesTouchedOrdinaryLinesWithoutChangingCode)
{
    constexpr std::string_view source = "Intro\n```cpp\n# not a heading\n```\nHeading\n";
    const auto heading = toggle_markdown_heading(source, {0, source.size()}, 2);
    ASSERT_TRUE(heading.has_value());
    EXPECT_EQ(heading->replacement, "## Intro\n```cpp\n# not a heading\n```\n## Heading");

    const std::string once = "## Intro\n```cpp\n# not a heading\n```\n## Heading\n";
    const auto restored = toggle_markdown_heading(once, {0, once.size()}, 2);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->replacement, "Intro\n```cpp\n# not a heading\n```\nHeading");

    constexpr std::string_view crlf_source = "One\r\n```cpp\r\n# not a heading\r\n```\r\nTwo\r\n";
    const auto crlf_heading = toggle_markdown_heading(crlf_source, {0, crlf_source.size()}, 1);
    ASSERT_TRUE(crlf_heading.has_value());
    EXPECT_EQ(crlf_heading->replacement, "# One\r\n```cpp\r\n# not a heading\r\n```\r\n# Two\r");
}

TEST(MarkdownTransformations, ASelectionEndingAtANewlineDoesNotTouchTheFollowingLine)
{
    const auto edit = toggle_markdown_heading("One\nTwo", {0, 4}, 1);
    ASSERT_TRUE(edit.has_value());
    EXPECT_EQ(edit->replaced, (MarkdownByteRange{0, 3}));
    EXPECT_EQ(edit->replacement, "# One");
}

TEST(MarkdownTransformations, RejectsEmptyAndInvalidRanges)
{
    EXPECT_FALSE(toggle_markdown_inline_style("text", {2, 2}, MarkdownInlineStyle::Italic));
    EXPECT_FALSE(toggle_markdown_inline_style("text", {3, 2}, MarkdownInlineStyle::Italic));
    EXPECT_FALSE(toggle_markdown_heading("text", {0, 4}, 0));
    EXPECT_FALSE(toggle_markdown_heading("text", {0, 5}, 1));
}

} // namespace
