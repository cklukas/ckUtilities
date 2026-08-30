#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "ck/edit/markdown_transformations.hpp"

namespace ck::edit
{

enum class MarkdownTableInsertPosition
{
    Before,
    After,
};

// Inserts a Markdown table at a zero-width cursor. The generated table has a
// header, a separator, and `body_rows` empty rows. Dimensions are deliberately
// bounded so a malformed dialog input cannot create an impractically large
// single document transaction.
std::optional<MarkdownTransformEdit> insert_markdown_table(
    std::string_view source,
    MarkdownByteRange cursor,
    std::size_t columns,
    std::size_t body_rows);

// Table structure operations locate one ordinary pipe table at the cursor or
// wholly selected range. They keep existing cell text and separator alignment
// markers, return one replacement for the complete table, and restore a
// selection in the affected cell. Fenced and indented code, malformed tables,
// and out-of-table ranges are rejected without a partial edit.
std::optional<MarkdownTransformEdit> insert_markdown_table_row(
    std::string_view source,
    MarkdownByteRange selection,
    MarkdownTableInsertPosition position);

std::optional<MarkdownTransformEdit> erase_markdown_table_row(
    std::string_view source,
    MarkdownByteRange selection);

std::optional<MarkdownTransformEdit> insert_markdown_table_column(
    std::string_view source,
    MarkdownByteRange selection,
    MarkdownTableInsertPosition position);

std::optional<MarkdownTransformEdit> erase_markdown_table_column(
    std::string_view source,
    MarkdownByteRange selection);

} // namespace ck::edit
