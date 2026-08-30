#include <gtest/gtest.h>

#include "ck/edit/markdown_transformations.hpp"

namespace
{

using ck::edit::MarkdownByteRange;
using ck::edit::MarkdownInlineStyle;
using ck::edit::MarkdownListStyle;
using ck::edit::toggle_markdown_heading;
using ck::edit::toggle_markdown_inline_style;
using ck::edit::toggle_markdown_list;
using ck::edit::toggle_markdown_quote;
using ck::edit::toggle_markdown_task;

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
    EXPECT_EQ(heading->selection, (MarkdownByteRange{0, heading->replacement.size()}));

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

TEST(MarkdownTransformations, TogglesTasksWithoutChangingProtectedBlockSyntax)
{
    constexpr std::string_view source =
        "Plan\n  - [ ] Draft\n1. [X] Review\n```cpp\n- [ ] code\n```\n# Heading\n> Quote\n| Cell |\n";
    const auto task = toggle_markdown_task(source, {0, source.size()});
    ASSERT_TRUE(task.has_value());
    EXPECT_EQ(task->replacement,
              "- [ ] Plan\n  - [x] Draft\n1. [ ] Review\n```cpp\n- [ ] code\n```\n# Heading\n> Quote\n| Cell |");
    EXPECT_EQ(task->selection, (MarkdownByteRange{0, task->replacement.size()}));

    constexpr std::string_view once =
        "- [ ] Plan\n  - [x] Draft\n1. [ ] Review\n```cpp\n- [ ] code\n```\n# Heading\n> Quote\n| Cell |\n";
    const auto toggled_again = toggle_markdown_task(once, {0, once.size()});
    ASSERT_TRUE(toggled_again.has_value());
    EXPECT_EQ(toggled_again->replacement,
              "- [x] Plan\n  - [ ] Draft\n1. [x] Review\n```cpp\n- [ ] code\n```\n# Heading\n> Quote\n| Cell |");

    const auto zero_width = toggle_markdown_task("Note\n", {0, 0});
    ASSERT_TRUE(zero_width.has_value());
    EXPECT_EQ(zero_width->replacement, "- [ ] Note");
}

TEST(MarkdownTransformations, TogglesQuoteLevelsWithoutRewritingCode)
{
    constexpr std::string_view source = "Intro\n\n- item\n```cpp\ncode\n```\n    indented\n> Existing\n";
    const auto quoted = toggle_markdown_quote(source, {0, source.size()});
    ASSERT_TRUE(quoted.has_value());
    EXPECT_EQ(quoted->replacement, "> Intro\n>\n> - item\n```cpp\ncode\n```\n    indented\n> > Existing");
    EXPECT_EQ(quoted->selection, (MarkdownByteRange{0, quoted->replacement.size()}));

    constexpr std::string_view once = "> Intro\n>\n> - item\n```cpp\ncode\n```\n    indented\n> > Existing\n";
    const auto restored = toggle_markdown_quote(once, {0, once.size()});
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->replacement, "Intro\n\n- item\n```cpp\ncode\n```\n    indented\n> Existing");

    const auto zero_width = toggle_markdown_quote("Note\n", {0, 0});
    ASSERT_TRUE(zero_width.has_value());
    EXPECT_EQ(zero_width->replacement, "> Note");
}

TEST(MarkdownTransformations, TogglesListsWithoutRewritingProtectedBlockSyntax)
{
    constexpr std::string_view source =
        "Draft\n  * [ ] Review\n3. Done\n---\n```cpp\n- code\n```\n# Heading\n> Quote\n| Cell |\n";
    const auto bullets = toggle_markdown_list(source, {0, source.size()}, MarkdownListStyle::Bullet);
    ASSERT_TRUE(bullets.has_value());
    EXPECT_EQ(bullets->replacement,
              "- Draft\n  - [ ] Review\n- Done\n---\n```cpp\n- code\n```\n# Heading\n> Quote\n| Cell |");
    EXPECT_EQ(bullets->selection, (MarkdownByteRange{0, bullets->replacement.size()}));

    constexpr std::string_view bullet_list =
        "- Draft\n  - [ ] Review\n- Done\n---\n```cpp\n- code\n```\n# Heading\n> Quote\n| Cell |\n";
    const auto plain = toggle_markdown_list(bullet_list, {0, bullet_list.size()}, MarkdownListStyle::Bullet);
    ASSERT_TRUE(plain.has_value());
    EXPECT_EQ(plain->replacement,
              "Draft\n  Review\nDone\n---\n```cpp\n- code\n```\n# Heading\n> Quote\n| Cell |");

    constexpr std::string_view mixed = "First\n- Second\n9. Third\n";
    const auto ordered = toggle_markdown_list(mixed, {0, mixed.size()}, MarkdownListStyle::Ordered);
    ASSERT_TRUE(ordered.has_value());
    EXPECT_EQ(ordered->replacement, "1. First\n2. Second\n3. Third");

    constexpr std::string_view ordered_list = "1. First\n2. Second\n3. Third\n";
    const auto restored = toggle_markdown_list(ordered_list, {0, ordered_list.size()}, MarkdownListStyle::Ordered);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->replacement, "First\nSecond\nThird");

    const auto zero_width = toggle_markdown_list("Note\n", {0, 0}, MarkdownListStyle::Ordered);
    ASSERT_TRUE(zero_width.has_value());
    EXPECT_EQ(zero_width->replacement, "1. Note");

    constexpr std::string_view crlf_source = "One\r\nTwo\r\n";
    const auto crlf = toggle_markdown_list(crlf_source, {0, crlf_source.size()}, MarkdownListStyle::Ordered);
    ASSERT_TRUE(crlf.has_value());
    EXPECT_EQ(crlf->replacement, "1. One\r\n2. Two\r");
}

TEST(MarkdownTransformations, RejectsEmptyAndInvalidRanges)
{
    EXPECT_FALSE(toggle_markdown_inline_style("text", {2, 2}, MarkdownInlineStyle::Italic));
    EXPECT_FALSE(toggle_markdown_inline_style("text", {3, 2}, MarkdownInlineStyle::Italic));
    EXPECT_FALSE(toggle_markdown_heading("text", {0, 4}, 0));
    EXPECT_FALSE(toggle_markdown_heading("text", {0, 5}, 1));
}

} // namespace
