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

enum class MarkdownListStyle
{
    Bullet,
    Ordered,
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

// Toggles checked state for task-list items on ordinary selected lines. A
// non-task list item gains an unchecked task marker; a non-list ordinary line
// becomes a bullet task. Fenced and indented code, headings, block quotes, and
// table-looking lines remain untouched. A zero-width selection targets its
// current line.
std::optional<MarkdownTransformEdit> toggle_markdown_task(
    std::string_view source,
    MarkdownByteRange selection);

// Adds one block-quote level to a mixed selection or removes one when every
// ordinary selected line is already quoted. Blank lines preserve quote
// continuity. Fenced and indented code remain untouched. A zero-width
// selection targets its current line.
std::optional<MarkdownTransformEdit> toggle_markdown_quote(
    std::string_view source,
    MarkdownByteRange selection);

// Applies one list style to every ordinary nonblank line touched by the
// selection. Calling it when every selected ordinary line already uses that
// style removes list (and task) markers, returning plain paragraphs. Ordered
// lists are numbered sequentially from one within the selection. Fenced and
// indented code, headings, block quotes, and table-looking lines remain
// untouched. A zero-width selection targets its current line.
std::optional<MarkdownTransformEdit> toggle_markdown_list(
    std::string_view source,
    MarkdownByteRange selection,
    MarkdownListStyle style);

// At a zero-width cursor at the end of an ordinary Markdown list item,
// continues the list on a fresh line. Bullet marker spacing and indentation
// are preserved, ordered items advance by one, and task items restart
// unchecked. An empty list item exits the list by removing its marker. Code
// fences and non-list lines return no edit so the caller can perform a normal
// newline insertion instead.
std::optional<MarkdownTransformEdit> continue_markdown_list(
    std::string_view source,
    MarkdownByteRange selection);

// Wraps a nonempty selection as `[label](destination)`, restoring selection
// to the label. If the selection is a complete link or its label, removes the
// link instead and ignores `destination`. A destination must be nonempty and
// contain no whitespace; parentheses are supported. This operation is local
// to the selected inline span and has no document or UI dependency.
std::optional<MarkdownTransformEdit> toggle_markdown_link(
    std::string_view source,
    MarkdownByteRange selection,
    std::string_view destination);

// Reflows each ordinary paragraph touched by the selection to `width` UTF-8
// code points (default 80). A zero-width selection targets its current
// paragraph. Fenced/indented code, headings, quotes, lists, tables,
// reference definitions, Setext headings, hard breaks, and paragraphs with
// inline code remain byte-for-byte unchanged. Reflowed paragraphs retain the
// first line's indentation and newline style and are returned as one atomic
// transaction-ready edit.
std::optional<MarkdownTransformEdit> reflow_markdown_paragraphs(
    std::string_view source,
    MarkdownByteRange selection,
    std::size_t width = 80U);

} // namespace ck::edit
