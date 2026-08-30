#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ck::edit
{

// Half-open byte offsets into one UTF-8 Markdown source string. Callers that
// own a revisioned document obtain these at the document boundary, where
// grapheme alignment and revision validity are enforced.
struct MarkdownByteRange
{
    std::size_t begin = 0;
    std::size_t end = 0;

    friend bool operator==(const MarkdownByteRange &, const MarkdownByteRange &) = default;
};

enum class MarkdownInlineStyle
{
    Bold,
    Italic,
    Strikethrough,
    Code,
};

// One atomic replacement plus the semantic selection to restore after it is
// committed. The transformation layer deliberately has no document, UI, or
// terminal dependency.
struct MarkdownTransformEdit
{
    MarkdownByteRange replaced;
    std::string replacement;
    MarkdownByteRange selection;
};

// Adds or removes an inline Markdown delimiter around a nonempty selected
// range. Existing matching delimiters immediately outside (or included in)
// the selection are removed; otherwise they are added. Inline-code delimiters
// grow beyond any backtick run in the selected text.
std::optional<MarkdownTransformEdit> toggle_markdown_inline_style(
    std::string_view source,
    MarkdownByteRange selection,
    MarkdownInlineStyle style);

// Applies `level` (1..6) to every ordinary nonblank line touched by the
// selection. Calling it again on lines already at that level removes their
// ATX heading marker. Fenced and indented code remain untouched. A zero-width
// selection targets its current line.
std::optional<MarkdownTransformEdit> toggle_markdown_heading(
    std::string_view source,
    MarkdownByteRange selection,
    int level);

} // namespace ck::edit
