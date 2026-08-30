#include "ck/edit/markdown_tables.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{

void require(bool value, const char *message)
{
    if (value)
        return;
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

std::string apply(std::string source, const ck::edit::MarkdownTransformEdit &edit)
{
    source.replace(edit.replaced.begin, edit.replaced.end - edit.replaced.begin, edit.replacement);
    return source;
}

} // namespace

int main()
{
    using ck::edit::MarkdownByteRange;
    using ck::edit::MarkdownTableInsertPosition;

    const auto inserted = ck::edit::insert_markdown_table("", {0, 0}, 2, 1);
    require(inserted.has_value() && inserted->replacement == "| Header 1 | Header 2 |\n| --- | --- |\n|  |  |\n",
            "Table insertion must create a header, separator, and requested body row.");
    require(inserted->selection == MarkdownByteRange{2, 10},
            "Table insertion must select the first header label for immediate replacement.");
    require(!ck::edit::insert_markdown_table("text", {0, 1}, 2, 1) &&
                !ck::edit::insert_markdown_table("```\ncode\n```\n", {5, 5}, 2, 1),
            "Table insertion must reject a text selection and fenced-code cursor.");

    const std::string prose = "Before\nafter";
    const auto between_prose_lines = ck::edit::insert_markdown_table(prose, {6, 6}, 1, 0);
    require(between_prose_lines.has_value() && apply(prose, *between_prose_lines) ==
                                                    "Before\n| Header 1 |\n| --- |\nafter",
            "Table insertion at the end of an ordinary line must use its existing newline as a block boundary.");
    const auto inside_prose = ck::edit::insert_markdown_table("before after", {6, 6}, 1, 0);
    require(inside_prose.has_value() && apply("before after", *inside_prose) ==
                                            "before\n| Header 1 |\n| --- |\n after",
            "Table insertion inside ordinary prose must create complete Markdown block boundaries.");
    const std::string prose_crlf = "Before\r\nafter";
    const auto between_crlf_lines = ck::edit::insert_markdown_table(prose_crlf, {6, 6}, 1, 0);
    require(between_crlf_lines.has_value() && apply(prose_crlf, *between_crlf_lines) ==
                                                  "Before\r\n| Header 1 |\r\n| --- |\r\nafter",
            "Table insertion must preserve a CRLF block boundary.");
    const std::string terminated_prose = "Before\n";
    const auto at_lf_eof = ck::edit::insert_markdown_table(
        terminated_prose, {terminated_prose.size(), terminated_prose.size()}, 1, 0);
    require(at_lf_eof.has_value() && apply(terminated_prose, *at_lf_eof) ==
                                        "Before\n| Header 1 |\n| --- |\n",
            "Table insertion must accept end-of-file after a terminating LF as the final empty line.");
    const std::string terminated_crlf_prose = "Before\r\n";
    const auto at_crlf_eof = ck::edit::insert_markdown_table(
        terminated_crlf_prose, {terminated_crlf_prose.size(), terminated_crlf_prose.size()}, 1, 0);
    require(at_crlf_eof.has_value() && apply(terminated_crlf_prose, *at_crlf_eof) ==
                                          "Before\r\n| Header 1 |\r\n| --- |\r\n",
            "Table insertion must accept end-of-file after a terminating CRLF as the final empty line.");
    const std::string terminated_table = "| A |\n| --- |\n";
    const auto after_table = ck::edit::insert_markdown_table(
        terminated_table, {terminated_table.size(), terminated_table.size()}, 1, 0);
    require(after_table.has_value() && apply(terminated_table, *after_table) ==
                                        "| A |\n| --- |\n\n| Header 1 |\n| --- |\n",
            "Table insertion after a terminated table must add a blank separator rather than merge table blocks.");

    const std::string table = "| Name | Count |\n| :--- | ---: |\n| one\\|two | 2 |\n";
    const auto add_row = ck::edit::insert_markdown_table_row(
        table, {table.find("2"), table.find("2")}, MarkdownTableInsertPosition::After);
    require(add_row.has_value() &&
                apply(table, *add_row) == "| Name | Count |\n| :--- | ---: |\n| one\\|two | 2 |\n|  |  |\n",
            "Adding a row must retain escaped pipes and separator alignment markers.");

    const std::string with_extra_row = apply(table, *add_row);
    const auto erase_row = ck::edit::erase_markdown_table_row(
        with_extra_row, {with_extra_row.rfind("|  |"), with_extra_row.rfind("|  |")});
    require(erase_row.has_value() && apply(with_extra_row, *erase_row) == table,
            "Deleting a body row must restore the preceding valid table atomically.");

    const auto add_column = ck::edit::insert_markdown_table_column(
        table, {table.find("Count"), table.find("Count")}, MarkdownTableInsertPosition::After);
    require(add_column.has_value() &&
                apply(table, *add_column) ==
                    "| Name | Count | Column 3 |\n| :--- | ---: | --- |\n| one\\|two | 2 |  |\n",
            "Adding a column must add a selected header, default separator, and each body cell together.");

    const std::string with_extra_column = apply(table, *add_column);
    const auto erase_column = ck::edit::erase_markdown_table_column(
        with_extra_column, {with_extra_column.find("Column 3"), with_extra_column.find("Column 3")});
    require(erase_column.has_value() && apply(with_extra_column, *erase_column) == table,
            "Deleting the active column must restore all affected rows together.");

    const std::string crlf = "| A | B |\r\n| --- | --- |\r\n| 1 | 2 |\r\n";
    const auto crlf_row = ck::edit::insert_markdown_table_row(
        crlf, {crlf.find("1"), crlf.find("1")}, MarkdownTableInsertPosition::Before);
    require(crlf_row.has_value() && apply(crlf, *crlf_row) ==
                "| A | B |\r\n| --- | --- |\r\n|  |  |\r\n| 1 | 2 |\r\n",
            "Table operations must retain CRLF source newlines.");

    require(!ck::edit::erase_markdown_table_column("| A |\n| --- |\n", {2, 2}) &&
                !ck::edit::erase_markdown_table_row("plain text\n", {1, 1}) &&
                !ck::edit::insert_markdown_table(table, {table.find("Name"), table.find("Name")}, 1, 0),
            "Table operations must reject the final column and non-table text rather than making a partial edit.");
    return EXIT_SUCCESS;
}
