#include <gtest/gtest.h>

#include "ck/edit/markdown_transformations.hpp"

namespace
{

using ck::edit::MarkdownByteRange;
using ck::edit::MarkdownInlineStyle;
using ck::edit::MarkdownListStyle;
using ck::edit::continue_markdown_list;
using ck::edit::indent_markdown_list;
using ck::edit::outdent_markdown_list;
using ck::edit::toggle_markdown_heading;
using ck::edit::toggle_markdown_image;
using ck::edit::toggle_markdown_inline_style;
using ck::edit::toggle_markdown_link;
using ck::edit::toggle_markdown_list;
using ck::edit::toggle_markdown_quote;
using ck::edit::toggle_markdown_task;
using ck::edit::reflow_markdown_paragraphs;

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

TEST(MarkdownTransformations, ContinuesOrdinaryMarkdownListItemsAtTheEndOfTheCurrentLine)
{
    const auto bullet = continue_markdown_list("- Draft", {7, 7});
    ASSERT_TRUE(bullet.has_value());
    EXPECT_EQ(bullet->replacement, "\n- ");
    EXPECT_EQ(bullet->selection, (MarkdownByteRange{10, 10}));

    constexpr std::string_view ordered_task = "  7) [x] Review";
    const auto next_task = continue_markdown_list(ordered_task, {ordered_task.size(), ordered_task.size()});
    ASSERT_TRUE(next_task.has_value());
    EXPECT_EQ(next_task->replacement, "\n  8) [ ] ");
    EXPECT_EQ(next_task->selection,
              (MarkdownByteRange{ordered_task.size() + next_task->replacement.size(),
                                  ordered_task.size() + next_task->replacement.size()}));

    constexpr std::string_view crlf_source = "* Note\r\nNext";
    const auto crlf = continue_markdown_list(crlf_source, {6, 6});
    ASSERT_TRUE(crlf.has_value());
    EXPECT_EQ(crlf->replacement, "\r\n* ");

    constexpr std::string_view empty_item = "- \nNext";
    const auto exit_list = continue_markdown_list(empty_item, {2, 2});
    ASSERT_TRUE(exit_list.has_value());
    EXPECT_EQ(exit_list->replaced, (MarkdownByteRange{0, 2}));
    EXPECT_EQ(exit_list->replacement, "");
    EXPECT_EQ(exit_list->selection, (MarkdownByteRange{0, 0}));

    EXPECT_FALSE(continue_markdown_list("ordinary", {8, 8}));
    EXPECT_FALSE(continue_markdown_list("```md\n- code", {12, 12}));
    EXPECT_FALSE(continue_markdown_list("- Draft", {3, 3}));
}

TEST(MarkdownTransformations, IndentsAndOutdentsNestedListItemsWithoutTouchingCode)
{
    constexpr std::string_view source = "- Parent\n  - [x] Child\n- Sibling\n";
    const auto indented = indent_markdown_list(source, {0, source.size()});
    ASSERT_TRUE(indented.has_value());
    EXPECT_EQ(indented->replacement, "  - Parent\n    - [x] Child\n  - Sibling");
    EXPECT_EQ(indented->selection, (MarkdownByteRange{0, indented->replacement.size()}));

    constexpr std::string_view nested = "  - Parent\n    - [x] Child\n  - Sibling\n";
    const auto outdented = outdent_markdown_list(nested, {0, nested.size()});
    ASSERT_TRUE(outdented.has_value());
    EXPECT_EQ(outdented->replacement, "- Parent\n  - [x] Child\n- Sibling");

    constexpr std::string_view deep_task = "- Parent\n    - [x] Child";
    const auto continued = continue_markdown_list(deep_task, {deep_task.size(), deep_task.size()});
    ASSERT_TRUE(continued.has_value());
    EXPECT_EQ(continued->replacement, "\n    - [ ] ");

    constexpr std::string_view crlf = "  - One\r\n  - Two\r\n";
    const auto crlf_indented = indent_markdown_list(crlf, {0, crlf.size()});
    ASSERT_TRUE(crlf_indented.has_value());
    EXPECT_EQ(crlf_indented->replacement, "    - One\r\n    - Two\r");

    constexpr std::string_view indented_code = "paragraph\n    - indented code";
    EXPECT_FALSE(indent_markdown_list(indented_code, {10, indented_code.size()}));
    EXPECT_FALSE(continue_markdown_list("    - indented code", {19, 19}));
    EXPECT_FALSE(outdent_markdown_list("- Root", {0, 0}));
}

TEST(MarkdownTransformations, TogglesLinksAndKeepsTheLabelSelected)
{
    constexpr std::string_view source = "Read caf\xC3\xA9 docs";
    const auto inserted = toggle_markdown_link(source, {5, 10}, "https://example.test/docs");
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(inserted->replacement, "[caf\xC3\xA9](https://example.test/docs)");
    EXPECT_EQ(inserted->selection, (MarkdownByteRange{6, 11}));

    constexpr std::string_view linked = "Read [caf\xC3\xA9](https://example.test/docs) docs";
    const auto label_unwrapped = toggle_markdown_link(linked, {6, 11}, "");
    ASSERT_TRUE(label_unwrapped.has_value());
    EXPECT_EQ(label_unwrapped->replaced, (MarkdownByteRange{5, 39}));
    EXPECT_EQ(label_unwrapped->replacement, "caf\xC3\xA9");
    EXPECT_EQ(label_unwrapped->selection, (MarkdownByteRange{5, 10}));

    constexpr std::string_view nested_destination = "[Guide](https://example.test/a_(b))";
    const auto complete_unwrapped =
        toggle_markdown_link(nested_destination, {0, nested_destination.size()}, "ignored");
    ASSERT_TRUE(complete_unwrapped.has_value());
    EXPECT_EQ(complete_unwrapped->replacement, "Guide");
    EXPECT_EQ(complete_unwrapped->selection, (MarkdownByteRange{0, 5}));

    EXPECT_FALSE(toggle_markdown_link("Read this", {5, 9}, "not a destination"));
    EXPECT_FALSE(toggle_markdown_link("Read this", {5, 5}, "https://example.test"));
}

TEST(MarkdownTransformations, TogglesImagesAndKeepsAltTextSelected)
{
    constexpr std::string_view source = "See logo";
    const auto inserted = toggle_markdown_image(source, {4, 8}, "https://example.test/a_(b).png");
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(inserted->replacement, "![logo](https://example.test/a_(b).png)");
    EXPECT_EQ(inserted->selection, (MarkdownByteRange{6, 10}));

    constexpr std::string_view image = "See ![logo](https://example.test/a_(b).png)";
    const auto alt_unwrapped = toggle_markdown_image(image, {6, 10}, "");
    ASSERT_TRUE(alt_unwrapped.has_value());
    EXPECT_EQ(alt_unwrapped->replaced, (MarkdownByteRange{4, image.size()}));
    EXPECT_EQ(alt_unwrapped->replacement, "logo");
    EXPECT_EQ(alt_unwrapped->selection, (MarkdownByteRange{4, 8}));

    const auto complete_unwrapped = toggle_markdown_image(image, {4, image.size()}, "ignored");
    ASSERT_TRUE(complete_unwrapped.has_value());
    EXPECT_EQ(complete_unwrapped->replacement, "logo");

    EXPECT_FALSE(toggle_markdown_image("See logo", {4, 8}, "not a destination"));
    EXPECT_FALSE(toggle_markdown_image("See logo", {4, 4}, "https://example.test/logo.png"));
}

TEST(MarkdownTransformations, ReflowsOrdinaryParagraphsWithoutChangingProtectedSyntax)
{
    constexpr std::string_view source =
        "Alpha beta\ngamma delta epsilon\nzeta\n\n# Heading\n\n```cpp\nalpha beta gamma\n```\n";
    const auto reflowed = reflow_markdown_paragraphs(source, {7, 7}, 20);
    ASSERT_TRUE(reflowed.has_value());
    EXPECT_EQ(reflowed->replaced, (MarkdownByteRange{0, 35}));
    EXPECT_EQ(reflowed->replacement, "Alpha beta gamma\ndelta epsilon zeta");
    EXPECT_EQ(reflowed->selection, (MarkdownByteRange{0, reflowed->replacement.size()}));

    constexpr std::string_view crlf_source = "One two\r\nthree four five\r\n";
    const auto crlf = reflow_markdown_paragraphs(crlf_source, {0, crlf_source.size()}, 20);
    ASSERT_TRUE(crlf.has_value());
    EXPECT_EQ(crlf->replacement, "One two three four\r\nfive\r");

    constexpr std::string_view unicode_source = "caf\xC3\xA9 alpha\nbeta gamma delta\n";
    const auto unicode = reflow_markdown_paragraphs(unicode_source, {0, unicode_source.size()}, 20);
    ASSERT_TRUE(unicode.has_value());
    EXPECT_EQ(unicode->replacement, "caf\xC3\xA9 alpha beta\ngamma delta");

    EXPECT_FALSE(reflow_markdown_paragraphs("Keep  \nthis hard break\n", {0, 0}, 20));
    EXPECT_FALSE(reflow_markdown_paragraphs("Use `inline code` exactly\n", {0, 0}, 20));
    EXPECT_FALSE(reflow_markdown_paragraphs("Heading\n---\n", {0, 0}, 20));
    EXPECT_FALSE(reflow_markdown_paragraphs("[reference]: https://example.test\n", {0, 0}, 20));
}

TEST(MarkdownTransformations, RejectsEmptyAndInvalidRanges)
{
    EXPECT_FALSE(toggle_markdown_inline_style("text", {2, 2}, MarkdownInlineStyle::Italic));
    EXPECT_FALSE(toggle_markdown_inline_style("text", {3, 2}, MarkdownInlineStyle::Italic));
    EXPECT_FALSE(toggle_markdown_heading("text", {0, 4}, 0));
    EXPECT_FALSE(toggle_markdown_heading("text", {0, 5}, 1));
}

} // namespace
