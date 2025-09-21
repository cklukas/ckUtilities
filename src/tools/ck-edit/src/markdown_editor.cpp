#include "ck/edit/markdown_editor.hpp"

#include "ck/about_dialog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ck::edit
{
namespace
{
#ifndef CK_EDIT_VERSION
#define CK_EDIT_VERSION "0.0.0"
#endif
constexpr int kInfoColumnWidth = 20;

const std::array<std::string_view, 7> kMarkdownExtensions = {
    ".md", ".markdown", ".mdown", ".mkd", ".mkdn", ".mdtxt", ".mdtext"};

const ushort cmToggleWrap = 3000;
const ushort cmToggleMarkdownMode = 3001;
const ushort cmHeading1 = 3010;
const ushort cmHeading2 = 3011;
const ushort cmHeading3 = 3012;
const ushort cmHeading4 = 3013;
const ushort cmHeading5 = 3014;
const ushort cmHeading6 = 3015;
const ushort cmClearHeading = 3016;
const ushort cmBold = 3020;
const ushort cmItalic = 3021;
const ushort cmBoldItalic = 3022;
const ushort cmRemoveFormatting = 3023;
const ushort cmBlockQuote = 3024;
const ushort cmBlockQuoteClear = 3025;
const ushort cmInsertBulletList = 3030;
const ushort cmInsertNumberedList = 3031;
const ushort cmInsertLink = 3032;
const ushort cmInsertImage = 3033;
const ushort cmInsertTable = 3035;
const ushort cmTableInsertRowAbove = 3040;
const ushort cmTableInsertRowBelow = 3041;
const ushort cmTableDeleteRow = 3042;
const ushort cmTableInsertColumnBefore = 3043;
const ushort cmTableInsertColumnAfter = 3044;
const ushort cmTableDeleteColumn = 3045;
const ushort cmTableDeleteTable = 3046;
const ushort cmTableAlignDefault = 3047;
const ushort cmTableAlignLeft = 3048;
const ushort cmTableAlignCenter = 3049;
const ushort cmTableAlignRight = 3050;
const ushort cmTableAlignNumber = 3051;
const ushort cmAbout = 3052;

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        char a = static_cast<char>(std::tolower(static_cast<unsigned char>(lhs[i])));
        char b = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[i])));
        if (a != b)
            return false;
    }
    return true;
}

bool isMarkdownFile(std::string_view path)
{
    std::filesystem::path p(path);
    auto ext = p.extension().string();
    for (auto candidate : kMarkdownExtensions)
    {
        if (equalsIgnoreCase(ext, candidate))
            return true;
    }
    return false;
}

std::string sanitizeMultiline(const std::string &text)
{
    std::string result;
    result.reserve(text.size());
    for (char ch : text)
    {
        if (ch == '\n' || ch == '\r' || ch == '\t')
            result.push_back(' ');
        else
            result.push_back(ch);
    }
    return result;
}

std::string columnLabel(int index)
{
    if (index < 0)
        return "?";
    std::string name;
    int value = index;
    while (true)
    {
        char letter = static_cast<char>('A' + (value % 26));
        name.insert(name.begin(), letter);
        if (value < 26)
            break;
        value = value / 26 - 1;
    }
    return name;
}

ushort execDialog(TDialog *d, void *data)
{
    TView *p = TProgram::application->validView(d);
    if (!p)
        return cmCancel;
    if (data)
        p->setData(data);
    ushort result = TProgram::deskTop->execView(p);
    if (result != cmCancel && data)
        p->getData(data);
    TObject::destroy(p);
    return result;
}

} // namespace

MarkdownFileEditor::MarkdownFileEditor(const TRect &bounds, TScrollBar *hScroll,
                                       TScrollBar *vScroll, TIndicator *indicator,
                                       TStringView fileName) noexcept
    : TFileEditor(bounds, hScroll, vScroll, indicator, fileName)
{
    if (!fileName.empty())
        markdownMode = isMarkdownFile(std::string(fileName));
    else
        markdownMode = true;
}

int MarkdownFileEditor::TableContext::columnCount() const noexcept
{
    if (!separatorInfo.tableAlignments.empty())
        return static_cast<int>(separatorInfo.tableAlignments.size());
    if (!headerInfo.tableCells.empty())
        return static_cast<int>(headerInfo.tableCells.size());
    for (const auto &info : bodyInfos)
    {
        if (!info.tableCells.empty())
            return static_cast<int>(info.tableCells.size());
    }
    return 0;
}

void MarkdownFileEditor::toggleWrap()
{
    wrapEnabled = !wrapEnabled;
    if (wrapEnabled)
        delta.x = 0;
    if (hScrollBar)
    {
        if (wrapEnabled)
            hScrollBar->hide();
        else
            hScrollBar->show();
    }
    notifyInfoView();
    drawView();
}

void MarkdownFileEditor::toggleMarkdownMode()
{
    markdownMode = !markdownMode;
    notifyInfoView();
}

void MarkdownFileEditor::applyHeadingLevel(int level)
{
    if (level < 1)
    {
        clearHeading();
        return;
    }
    lock();
    uint lineStartPtr = lineStart(curPtr);
    uint lineEndPtr = lineEnd(lineStartPtr);
    std::string line = readRange(lineStartPtr, lineEndPtr);
    std::size_t index = 0;
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t'))
        ++index;
    std::string indent = line.substr(0, index);
    std::size_t markerEnd = index;
    while (markerEnd < line.size() && line[markerEnd] == '#')
        ++markerEnd;
    if (markerEnd < line.size() && line[markerEnd] == ' ')
        ++markerEnd;
    std::string content = line.substr(markerEnd);
    std::string replacement = indent + std::string(level, '#');
    if (!content.empty())
        replacement.append(" ").append(content);
    replaceRange(lineStartPtr, lineEndPtr, replacement);
    unlock();
    onContentModified();
}

void MarkdownFileEditor::clearHeading()
{
    lock();
    uint lineStartPtr = lineStart(curPtr);
    uint lineEndPtr = lineEnd(lineStartPtr);
    std::string line = readRange(lineStartPtr, lineEndPtr);
    std::size_t index = 0;
    while (index < line.size() && (line[index] == ' ' || line[index] == '\t'))
        ++index;
    std::size_t markerEnd = index;
    while (markerEnd < line.size() && line[markerEnd] == '#')
        ++markerEnd;
    if (markerEnd < line.size() && line[markerEnd] == ' ')
        ++markerEnd;
    std::string replacement = line.substr(0, index) + line.substr(markerEnd);
    replaceRange(lineStartPtr, lineEndPtr, replacement);
    unlock();
    onContentModified();
}

bool MarkdownFileEditor::ensureSelection()
{
    if (hasSelection())
        return true;
    uint start = prevWord(curPtr);
    uint end = nextWord(curPtr);
    if (start == end)
        return false;
    setSelect(start, end, True);
    return true;
}

void MarkdownFileEditor::wrapSelectionWith(const std::string &prefix, const std::string &suffix)
{
    if (!ensureSelection())
        return;
    lock();
    uint start = std::min(selStart, selEnd);
    uint end = std::max(selStart, selEnd);
    setCurPtr(start, 0);
    insertText(prefix.c_str(), prefix.size(), False);
    setCurPtr(end + prefix.size(), 0);
    insertText(suffix.c_str(), suffix.size(), False);
    setCurPtr(end + prefix.size() + suffix.size(), 0);
    unlock();
    onContentModified();
}

void MarkdownFileEditor::removeFormattingAround(uint start, uint end)
{
    if (end <= start)
        return;
    std::string text = readRange(start, end);
    auto removePair = [&](const std::string &marker)
    {
        if (text.size() >= marker.size() * 2 &&
            text.rfind(marker, 0) == 0 &&
            text.rfind(marker, text.size() - marker.size()) == text.size() - marker.size())
        {
            text = text.substr(marker.size(), text.size() - 2 * marker.size());
            replaceRange(start, end, text);
            setSelect(start, start + text.size(), True);
            onContentModified();
            return true;
        }
        return false;
    };
    if (removePair("***") || removePair("___") || removePair("**") || removePair("__") ||
        removePair("*") || removePair("_") || removePair("~~"))
        return;
}

void MarkdownFileEditor::applyBold()
{
    wrapSelectionWith("**", "**");
}

void MarkdownFileEditor::applyItalic()
{
    wrapSelectionWith("*", "*");
}

void MarkdownFileEditor::applyBoldItalic()
{
    wrapSelectionWith("***", "***");
}

void MarkdownFileEditor::removeFormatting()
{
    if (!ensureSelection())
        return;
    uint start = std::min(selStart, selEnd);
    uint end = std::max(selStart, selEnd);
    removeFormattingAround(start, end);
}

void MarkdownFileEditor::applyBlockQuote()
{
    indentRangeWith("> ");
    onContentModified();
}

void MarkdownFileEditor::removeBlockQuote()
{
    unindentBlockQuote();
    onContentModified();
}

void MarkdownFileEditor::indentRangeWith(const std::string &prefix)
{
    lock();
    uint start = lineStart(hasSelection() ? std::min(selStart, selEnd) : curPtr);
    uint end = lineEnd(hasSelection() ? std::max(selStart, selEnd) : curPtr);
    uint current = start;
    while (true)
    {
        setCurPtr(current, 0);
        insertText(prefix.c_str(), prefix.size(), False);
        if (current >= end)
            break;
        uint next = nextLine(current);
        if (next <= current)
            break;
        end += prefix.size();
        current = next;
    }
    unlock();
}

void MarkdownFileEditor::unindentBlockQuote()
{
    lock();
    uint start = lineStart(hasSelection() ? std::min(selStart, selEnd) : curPtr);
    uint end = lineEnd(hasSelection() ? std::max(selStart, selEnd) : curPtr);
    uint current = start;
    while (current <= end)
    {
        std::string line = readRange(current, lineEnd(current));
        if (!line.empty())
        {
            if (line.rfind("> ", 0) == 0)
                replaceRange(current, current + 2, "");
            else if (line.rfind(">", 0) == 0)
                replaceRange(current, current + 1, "");
        }
        uint next = nextLine(current);
        if (next <= current)
            break;
        end -= std::min<uint>(end - current, 2u);
        current = next;
    }
    unlock();
}

void MarkdownFileEditor::insertListItems(int count, bool ordered)
{
    if (count <= 0)
        return;
    std::ostringstream out;
    for (int i = 0; i < count; ++i)
    {
        if (i > 0)
            out << "\n";
        if (ordered)
            out << (i + 1) << ". Item" << (i + 1);
        else
            out << "- Item" << (i + 1);
    }
    insertRichInline("", "", out.str());
}

void MarkdownFileEditor::insertBulletList(int count)
{
    insertListItems(count, false);
}

void MarkdownFileEditor::insertNumberedList(int count)
{
    insertListItems(count, true);
}

void MarkdownFileEditor::insertRichInline(const std::string &prefix, const std::string &suffix,
                                          const std::string &placeholder)
{
    lock();
    if (hasSelection())
        deleteSelect();
    insertText(prefix.c_str(), prefix.size(), False);
    insertText(placeholder.c_str(), placeholder.size(), False);
    insertText(suffix.c_str(), suffix.size(), False);
    setCurPtr(curPtr - suffix.size(), 0);
    unlock();
    onContentModified();
}

int MarkdownFileEditor::promptForCount(const char *title)
{
    char buffer[16] = "3";
    if (inputBox(title, "Number of items", buffer, sizeof(buffer)) == cmCancel)
        return 0;
    try
    {
        int value = std::stoi(buffer);
        return std::clamp(value, 0, 50);
    }
    catch (...)
    {
        return 0;
    }
}

std::string MarkdownFileEditor::promptForText(const char *title, const char *label, const std::string &initial)
{
    char buffer[256];
    std::strncpy(buffer, initial.c_str(), sizeof(buffer));
    buffer[sizeof(buffer) - 1] = '\0';
    if (inputBox(title, label, buffer, sizeof(buffer) - 1) == cmCancel)
        return {};
    return std::string(buffer);
}

int MarkdownFileEditor::promptForNumeric(const char *title, const char *label, int defaultValue, int minValue, int maxValue)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d", defaultValue);
    if (inputBox(title, label, buffer, sizeof(buffer) - 1) == cmCancel)
        return -1;
    try
    {
        int value = std::stoi(buffer);
        return std::clamp(value, minValue, maxValue);
    }
    catch (...)
    {
        return -1;
    }
}

void MarkdownFileEditor::insertLink()
{
    std::string label = promptForText("Insert Link", "Link text", hasSelection() ? readRange(std::min(selStart, selEnd), std::max(selStart, selEnd)) : "");
    if (label.empty())
        return;
    std::string url = promptForText("Insert Link", "Target URL", "https://");
    if (url.empty())
        return;
    std::ostringstream out;
    out << '[' << label << "](" << url << ')';
    insertRichInline("", "", out.str());
}

void MarkdownFileEditor::insertImage()
{
    std::string alt = promptForText("Insert Image", "Alt text", "Image");
    if (alt.empty())
        return;
    std::string url = promptForText("Insert Image", "Image URL", "https://");
    if (url.empty())
        return;
    std::ostringstream out;
    out << "![" << alt << "](" << url << ')';
    insertRichInline("", "", out.str());
}

bool MarkdownFileEditor::locateTableContext(TableContext &context)
{
    context = TableContext{};
    uint target = lineStart(curPtr);
    MarkdownParserState state;
    uint ptr = 0;
    TableContext working;
    while (ptr < bufLen)
    {
        uint end = lineEnd(ptr);
        std::string line = readRange(ptr, end);
        MarkdownLineInfo info = markdownAnalyzer.analyzeLine(line, state);
        bool isTableLine = info.kind == MarkdownLineKind::TableRow || info.kind == MarkdownLineKind::TableSeparator;
        if (isTableLine)
        {
            if (!working.valid)
            {
                working = TableContext{};
                working.valid = true;
            }
            if (info.kind == MarkdownLineKind::TableRow)
            {
                if (info.isTableHeader && working.headerPtr == UINT_MAX)
                {
                    working.headerPtr = ptr;
                    working.headerInfo = info;
                }
                else
                {
                    working.bodyPtrs.push_back(ptr);
                    working.bodyInfos.push_back(info);
                }
                if (ptr == target)
                {
                    working.activeRow = info.isTableHeader ? TableContext::ActiveRow::Header : TableContext::ActiveRow::Body;
                    working.activePtr = ptr;
                    working.activeInfo = info;
                }
            }
            else
            {
                working.separatorPtr = ptr;
                working.separatorInfo = info;
                if (ptr == target)
                {
                    working.activeRow = TableContext::ActiveRow::Separator;
                    working.activePtr = ptr;
                    working.activeInfo = info;
                }
            }
        }
        else if (working.valid)
        {
            if (working.activeRow != TableContext::ActiveRow::None)
            {
                context = working;
                context.valid = true;
                break;
            }
            working = TableContext{};
        }

        uint next = nextLine(ptr);
        if (next <= ptr)
            break;
        ptr = next;
    }

    if (!context.valid && working.valid && working.activeRow != TableContext::ActiveRow::None)
    {
        context = working;
        context.valid = true;
    }

    if (!context.valid)
        return false;
    if (context.headerPtr == UINT_MAX || context.separatorPtr == UINT_MAX)
        return false;
    if (context.activeRow == TableContext::ActiveRow::None)
        return false;

    int columns = context.columnCount();
    if (columns <= 0)
        return false;

    context.activeColumn = -1;
    const auto &cells = context.activeInfo.tableCells;
    if (!cells.empty())
    {
        for (std::size_t i = 0; i < cells.size(); ++i)
        {
            const auto &cell = cells[i];
            auto endColumn = std::max(cell.endColumn, cell.startColumn + 1);
            if (curPos.x >= static_cast<int>(cell.startColumn) && curPos.x < static_cast<int>(endColumn))
            {
                context.activeColumn = static_cast<int>(i);
                break;
            }
        }
        if (context.activeColumn == -1)
            context.activeColumn = static_cast<int>(cells.size()) - 1;
    }

    if (context.activeColumn < 0)
        context.activeColumn = std::clamp(curPos.x, 0, columns - 1);
    if (context.activeColumn >= columns)
        context.activeColumn = columns - 1;

    return true;
}

void MarkdownFileEditor::insertTable()
{
    int columns = promptForNumeric("Insert Table", "Number of columns", 3, 1, 12);
    if (columns < 1)
        return;
    int rows = promptForNumeric("Insert Table", "Number of body rows", 2, 0, 50);
    if (rows < 0)
        return;

    std::vector<std::string> headerCells;
    headerCells.reserve(columns);
    for (int i = 0; i < columns; ++i)
    {
        std::ostringstream cell;
        cell << "Column " << columnLabel(i);
        headerCells.push_back(cell.str());
    }

    std::vector<MarkdownTableAlignment> alignments(columns, MarkdownTableAlignment::Default);

    std::ostringstream table;
    table << makeTableRow(headerCells) << '\n';
    table << makeTableAlignmentRow(columns, alignments);
    for (int r = 0; r < rows; ++r)
    {
        std::vector<std::string> rowCells;
        rowCells.reserve(columns);
        for (int c = 0; c < columns; ++c)
        {
            std::ostringstream cell;
            cell << "Cell " << (r + 1) << '.' << columnLabel(c);
            rowCells.push_back(cell.str());
        }
        table << '\n' << makeTableRow(rowCells);
    }
    if (curPtr < bufLen && bufChar(curPtr) != '\n')
        table << '\n';

    std::string prefix;
    if (curPtr > 0 && bufChar(curPtr - 1) != '\n')
        prefix = "\n";

    insertRichInline(prefix, "", table.str());
}

void MarkdownFileEditor::tableInsertRowAbove()
{
    TableContext context;
    if (!locateTableContext(context))
    {
        messageBox("Cursor is not inside a Markdown table.", mfError | mfOKButton);
        return;
    }
    insertTableRow(context, false);
}

void MarkdownFileEditor::tableInsertRowBelow()
{
    TableContext context;
    if (!locateTableContext(context))
    {
        messageBox("Cursor is not inside a Markdown table.", mfError | mfOKButton);
        return;
    }
    insertTableRow(context, true);
}

void MarkdownFileEditor::tableDeleteRow()
{
    TableContext context;
    if (!locateTableContext(context))
    {
        messageBox("Cursor is not inside a Markdown table.", mfError | mfOKButton);
        return;
    }
    if (context.activeRow != TableContext::ActiveRow::Body)
    {
        messageBox("Select a table body row to delete.", mfError | mfOKButton);
        return;
    }

    int columns = context.columnCount();
    if (columns <= 0)
    {
        messageBox("Unable to determine the current table layout.", mfError | mfOKButton);
        return;
    }
    if (context.bodyInfos.empty())
    {
        messageBox("The table has no body rows to delete.", mfError | mfOKButton);
        return;
    }

    std::ostringstream prompt;
    prompt << "Delete table row " << context.activeInfo.tableRowIndex << '?';
    if (messageBox(prompt.str().c_str(), mfConfirmation | mfYesButton | mfNoButton) != cmYes)
        return;

    auto collectCells = [&](const MarkdownLineInfo &info) {
        std::vector<std::string> result(columns);
        for (int i = 0; i < columns && i < static_cast<int>(info.tableCells.size()); ++i)
            result[i] = info.tableCells[i].text;
        return result;
    };

    std::vector<std::string> headerCells = collectCells(context.headerInfo);
    std::vector<MarkdownTableAlignment> alignments = context.separatorInfo.tableAlignments;
    if (static_cast<int>(alignments.size()) < columns)
        alignments.resize(columns, MarkdownTableAlignment::Default);
    std::vector<std::vector<std::string>> bodyCells;
    bodyCells.reserve(context.bodyInfos.size());
    for (const auto &info : context.bodyInfos)
        bodyCells.push_back(collectCells(info));

    int bodyIndex = 0;
    for (std::size_t i = 0; i < context.bodyPtrs.size(); ++i)
    {
        if (context.bodyPtrs[i] == context.activePtr)
        {
            bodyIndex = static_cast<int>(i);
            break;
        }
    }
    if (bodyIndex >= 0 && bodyIndex < static_cast<int>(bodyCells.size()))
        bodyCells.erase(bodyCells.begin() + bodyIndex);

    std::ostringstream out;
    out << makeTableRow(headerCells) << '\n';
    out << makeTableAlignmentRow(columns, alignments);
    for (const auto &row : bodyCells)
        out << '\n' << makeTableRow(row);

    uint start = context.headerPtr;
    uint lastPtr = context.bodyPtrs.empty() ? context.separatorPtr : context.bodyPtrs.back();
    uint end = nextLine(lastPtr);
    bool hadNewline = false;
    if (end > start && end <= bufLen && bufChar(end - 1) == '\n')
        hadNewline = true;
    if (end <= lastPtr)
        end = lineEnd(lastPtr);
    if (hadNewline && (out.tellp() == 0 || out.str().back() != '\n'))
        out << '\n';

    lock();
    replaceRange(start, end, out.str());
    unlock();
    onContentModified();

    uint newPtr = start;
    int offset = bodyCells.empty() ? 1 : 2 + std::min(bodyIndex, static_cast<int>(bodyCells.size()) - 1);
    offset = std::max(offset, 1);
    for (int i = 0; i < offset; ++i)
        newPtr = lineMove(newPtr, 1);
    setCurPtr(newPtr, 0);
}

void MarkdownFileEditor::tableInsertColumnBefore()
{
    TableContext context;
    if (!locateTableContext(context))
    {
        messageBox("Cursor is not inside a Markdown table.", mfError | mfOKButton);
        return;
    }
    insertTableColumn(context, false);
}

void MarkdownFileEditor::tableInsertColumnAfter()
{
    TableContext context;
    if (!locateTableContext(context))
    {
        messageBox("Cursor is not inside a Markdown table.", mfError | mfOKButton);
        return;
    }
    insertTableColumn(context, true);
}

void MarkdownFileEditor::tableDeleteColumn()
{
    TableContext context;
    if (!locateTableContext(context))
    {
        messageBox("Cursor is not inside a Markdown table.", mfError | mfOKButton);
        return;
    }

    int columns = context.columnCount();
    if (columns <= 0)
    {
        messageBox("Unable to determine the current table layout.", mfError | mfOKButton);
        return;
    }
    if (columns == 1)
    {
        messageBox("A table must have at least one column.", mfError | mfOKButton);
        return;
    }

    auto collectCells = [&](const MarkdownLineInfo &info) {
        std::vector<std::string> result(columns);
        for (int i = 0; i < columns && i < static_cast<int>(info.tableCells.size()); ++i)
            result[i] = info.tableCells[i].text;
        return result;
    };

    std::vector<std::string> headerCells = collectCells(context.headerInfo);
    std::vector<MarkdownTableAlignment> alignments = context.separatorInfo.tableAlignments;
    if (static_cast<int>(alignments.size()) < columns)
        alignments.resize(columns, MarkdownTableAlignment::Default);
    std::vector<std::vector<std::string>> bodyCells;
    bodyCells.reserve(context.bodyInfos.size());
    for (const auto &info : context.bodyInfos)
        bodyCells.push_back(collectCells(info));

    int columnIndex = std::clamp(context.activeColumn, 0, columns - 1);
    std::string columnName = columnLabel(columnIndex);
    std::string prompt = "Delete column " + columnName + '?';
    if (messageBox(prompt.c_str(), mfConfirmation | mfYesButton | mfNoButton) != cmYes)
        return;

    headerCells.erase(headerCells.begin() + columnIndex);
    if (!alignments.empty())
        alignments.erase(alignments.begin() + columnIndex);
    for (auto &row : bodyCells)
    {
        if (!row.empty() && columnIndex < static_cast<int>(row.size()))
            row.erase(row.begin() + columnIndex);
    }
    --columns;

    std::ostringstream out;
    out << makeTableRow(headerCells) << '\n';
    out << makeTableAlignmentRow(columns, alignments);
    for (const auto &row : bodyCells)
        out << '\n' << makeTableRow(row);

    uint start = context.headerPtr;
    uint lastPtr = context.bodyPtrs.empty() ? context.separatorPtr : context.bodyPtrs.back();
    uint end = nextLine(lastPtr);
    bool hadNewline = false;
    if (end > start && end <= bufLen && bufChar(end - 1) == '\n')
        hadNewline = true;
    if (end <= lastPtr)
        end = lineEnd(lastPtr);
    if (hadNewline && (out.tellp() == 0 || out.str().back() != '\n'))
        out << '\n';

    lock();
    replaceRange(start, end, out.str());
    unlock();
    onContentModified();

    int rowOffset = 0;
    if (context.activeRow == TableContext::ActiveRow::Header)
        rowOffset = 0;
    else if (context.activeRow == TableContext::ActiveRow::Separator)
        rowOffset = 1;
    else if (context.activeRow == TableContext::ActiveRow::Body)
    {
        rowOffset = 2;
        for (std::size_t i = 0; i < context.bodyPtrs.size(); ++i)
        {
            if (context.bodyPtrs[i] == context.activePtr)
            {
                rowOffset += static_cast<int>(i);
                break;
            }
        }
    }
    uint newPtr = start;
    for (int i = 0; i < rowOffset; ++i)
        newPtr = lineMove(newPtr, 1);
    setCurPtr(newPtr, 0);
}

void MarkdownFileEditor::tableDeleteTable()
{
    TableContext context;
    if (!locateTableContext(context))
    {
        messageBox("Cursor is not inside a Markdown table.", mfError | mfOKButton);
        return;
    }

    if (messageBox("Delete the entire table?", mfConfirmation | mfYesButton | mfNoButton) != cmYes)
        return;

    uint start = context.headerPtr;
    uint lastPtr = context.bodyPtrs.empty() ? context.separatorPtr : context.bodyPtrs.back();
    uint end = nextLine(lastPtr);
    if (end <= lastPtr)
        end = lineEnd(lastPtr);

    lock();
    replaceRange(start, end, "");
    unlock();
    onContentModified();
}

void MarkdownFileEditor::tableAlignColumn(MarkdownTableAlignment alignment)
{
    TableContext context;
    if (!locateTableContext(context))
    {
        messageBox("Cursor is not inside a Markdown table.", mfError | mfOKButton);
        return;
    }
    alignTableColumn(context, alignment);
}

void MarkdownFileEditor::insertTableRow(TableContext &context, bool below)
{
    int columns = context.columnCount();
    if (columns <= 0)
    {
        messageBox("Unable to determine the current table layout.", mfError | mfOKButton);
        return;
    }

    auto collectCells = [&](const MarkdownLineInfo &info) {
        std::vector<std::string> result(columns);
        for (int i = 0; i < columns && i < static_cast<int>(info.tableCells.size()); ++i)
            result[i] = info.tableCells[i].text;
        return result;
    };

    std::vector<std::string> headerCells = collectCells(context.headerInfo);
    std::vector<MarkdownTableAlignment> alignments = context.separatorInfo.tableAlignments;
    if (static_cast<int>(alignments.size()) < columns)
        alignments.resize(columns, MarkdownTableAlignment::Default);
    std::vector<std::vector<std::string>> bodyCells;
    bodyCells.reserve(context.bodyInfos.size());
    for (const auto &info : context.bodyInfos)
        bodyCells.push_back(collectCells(info));

    int insertIndex = 0;
    if (context.activeRow == TableContext::ActiveRow::Body)
    {
        int bodyIndex = 0;
        for (std::size_t i = 0; i < context.bodyPtrs.size(); ++i)
        {
            if (context.bodyPtrs[i] == context.activePtr)
            {
                bodyIndex = static_cast<int>(i);
                break;
            }
        }
        insertIndex = below ? bodyIndex + 1 : bodyIndex;
    }
    else if (context.activeRow == TableContext::ActiveRow::Header || context.activeRow == TableContext::ActiveRow::Separator)
    {
        if (!below)
        {
            messageBox("Cannot insert a row above the header.", mfError | mfOKButton);
            return;
        }
        insertIndex = 0;
    }

    insertIndex = std::clamp(insertIndex, 0, static_cast<int>(bodyCells.size()));
    bodyCells.insert(bodyCells.begin() + insertIndex, std::vector<std::string>(columns, ""));

    std::ostringstream out;
    out << makeTableRow(headerCells) << '\n';
    out << makeTableAlignmentRow(columns, alignments);
    for (const auto &row : bodyCells)
        out << '\n' << makeTableRow(row);

    uint start = context.headerPtr;
    uint lastPtr = context.bodyPtrs.empty() ? context.separatorPtr : context.bodyPtrs.back();
    uint end = nextLine(lastPtr);
    bool hadNewline = false;
    if (end > start && end <= bufLen && bufChar(end - 1) == '\n')
        hadNewline = true;
    if (end <= lastPtr)
        end = lineEnd(lastPtr);
    if (hadNewline && (out.tellp() == 0 || out.str().back() != '\n'))
        out << '\n';

    lock();
    replaceRange(start, end, out.str());
    unlock();
    onContentModified();

    uint newPtr = start;
    int offset = 2 + insertIndex;
    for (int i = 0; i < offset; ++i)
        newPtr = lineMove(newPtr, 1);
    setCurPtr(newPtr, 0);
}

void MarkdownFileEditor::insertTableColumn(TableContext &context, bool after)
{
    int columns = context.columnCount();
    if (columns <= 0)
    {
        messageBox("Unable to determine the current table layout.", mfError | mfOKButton);
        return;
    }

    auto collectCells = [&](const MarkdownLineInfo &info) {
        std::vector<std::string> result(columns);
        for (int i = 0; i < columns && i < static_cast<int>(info.tableCells.size()); ++i)
            result[i] = info.tableCells[i].text;
        return result;
    };

    std::vector<std::string> headerCells = collectCells(context.headerInfo);
    std::vector<MarkdownTableAlignment> alignments = context.separatorInfo.tableAlignments;
    if (static_cast<int>(alignments.size()) < columns)
        alignments.resize(columns, MarkdownTableAlignment::Default);
    std::vector<std::vector<std::string>> bodyCells;
    bodyCells.reserve(context.bodyInfos.size());
    for (const auto &info : context.bodyInfos)
        bodyCells.push_back(collectCells(info));

    int insertIndex = context.activeColumn + (after ? 1 : 0);
    insertIndex = std::clamp(insertIndex, 0, columns);

    std::string headerLabel = "Column " + columnLabel(insertIndex);
    headerCells.insert(headerCells.begin() + insertIndex, headerLabel);
    alignments.insert(alignments.begin() + insertIndex, MarkdownTableAlignment::Default);
    for (auto &row : bodyCells)
        row.insert(row.begin() + insertIndex, "");

    ++columns;

    std::ostringstream out;
    out << makeTableRow(headerCells) << '\n';
    out << makeTableAlignmentRow(columns, alignments);
    for (const auto &row : bodyCells)
        out << '\n' << makeTableRow(row);

    uint start = context.headerPtr;
    uint lastPtr = context.bodyPtrs.empty() ? context.separatorPtr : context.bodyPtrs.back();
    uint end = nextLine(lastPtr);
    bool hadNewline = false;
    if (end > start && end <= bufLen && bufChar(end - 1) == '\n')
        hadNewline = true;
    if (end <= lastPtr)
        end = lineEnd(lastPtr);
    if (hadNewline && (out.tellp() == 0 || out.str().back() != '\n'))
        out << '\n';

    lock();
    replaceRange(start, end, out.str());
    unlock();
    onContentModified();

    int rowOffset = 0;
    if (context.activeRow == TableContext::ActiveRow::Header)
        rowOffset = 0;
    else if (context.activeRow == TableContext::ActiveRow::Separator)
        rowOffset = 1;
    else if (context.activeRow == TableContext::ActiveRow::Body)
    {
        rowOffset = 2;
        for (std::size_t i = 0; i < context.bodyPtrs.size(); ++i)
        {
            if (context.bodyPtrs[i] == context.activePtr)
            {
                rowOffset += static_cast<int>(i);
                break;
            }
        }
    }

    uint newPtr = start;
    for (int i = 0; i < rowOffset; ++i)
        newPtr = lineMove(newPtr, 1);
    setCurPtr(newPtr, 0);
}

void MarkdownFileEditor::alignTableColumn(TableContext &context, MarkdownTableAlignment alignment)
{
    int columns = context.columnCount();
    if (columns <= 0)
    {
        messageBox("Unable to determine the current table layout.", mfError | mfOKButton);
        return;
    }

    auto collectCells = [&](const MarkdownLineInfo &info) {
        std::vector<std::string> result(columns);
        for (int i = 0; i < columns && i < static_cast<int>(info.tableCells.size()); ++i)
            result[i] = info.tableCells[i].text;
        return result;
    };

    std::vector<std::string> headerCells = collectCells(context.headerInfo);
    std::vector<MarkdownTableAlignment> alignments = context.separatorInfo.tableAlignments;
    if (static_cast<int>(alignments.size()) < columns)
        alignments.resize(columns, MarkdownTableAlignment::Default);
    std::vector<std::vector<std::string>> bodyCells;
    bodyCells.reserve(context.bodyInfos.size());
    for (const auto &info : context.bodyInfos)
        bodyCells.push_back(collectCells(info));

    int targetColumn = std::clamp(context.activeColumn, 0, columns - 1);
    alignments[targetColumn] = alignment;

    std::ostringstream out;
    out << makeTableRow(headerCells) << '\n';
    out << makeTableAlignmentRow(columns, alignments);
    for (const auto &row : bodyCells)
        out << '\n' << makeTableRow(row);

    uint start = context.headerPtr;
    uint lastPtr = context.bodyPtrs.empty() ? context.separatorPtr : context.bodyPtrs.back();
    uint end = nextLine(lastPtr);
    bool hadNewline = false;
    if (end > start && end <= bufLen && bufChar(end - 1) == '\n')
        hadNewline = true;
    if (end <= lastPtr)
        end = lineEnd(lastPtr);
    if (hadNewline && (out.tellp() == 0 || out.str().back() != '\n'))
        out << '\n';

    lock();
    replaceRange(start, end, out.str());
    unlock();
    onContentModified();

    uint newPtr = start;
    int rowOffset = 0;
    if (context.activeRow == TableContext::ActiveRow::Separator)
        rowOffset = 1;
    else if (context.activeRow == TableContext::ActiveRow::Body)
    {
        rowOffset = 2;
        for (std::size_t i = 0; i < context.bodyPtrs.size(); ++i)
        {
            if (context.bodyPtrs[i] == context.activePtr)
            {
                rowOffset += static_cast<int>(i);
                break;
            }
        }
    }
    for (int i = 0; i < rowOffset; ++i)
        newPtr = lineMove(newPtr, 1);
    setCurPtr(newPtr, 0);
}

void MarkdownFileEditor::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmToggleWrap:
            toggleWrap();
            clearEvent(event);
            return;
        case cmToggleMarkdownMode:
            toggleMarkdownMode();
            clearEvent(event);
            return;
        case cmHeading1:
        case cmHeading2:
        case cmHeading3:
        case cmHeading4:
        case cmHeading5:
        case cmHeading6:
            applyHeadingLevel(static_cast<int>(event.message.command - cmHeading1 + 1));
            clearEvent(event);
            return;
        case cmClearHeading:
            clearHeading();
            clearEvent(event);
            return;
        case cmBold:
            applyBold();
            clearEvent(event);
            return;
        case cmItalic:
            applyItalic();
            clearEvent(event);
            return;
        case cmBoldItalic:
            applyBoldItalic();
            clearEvent(event);
            return;
        case cmRemoveFormatting:
            removeFormatting();
            clearEvent(event);
            return;
        case cmBlockQuote:
            applyBlockQuote();
            clearEvent(event);
            return;
        case cmBlockQuoteClear:
            removeBlockQuote();
            clearEvent(event);
            return;
        case cmInsertBulletList:
        {
            int count = promptForCount("Bullet List");
            insertBulletList(count);
            clearEvent(event);
            return;
        }
        case cmInsertNumberedList:
        {
            int count = promptForCount("Numbered List");
            insertNumberedList(count);
            clearEvent(event);
            return;
        }
        case cmInsertLink:
            insertLink();
            clearEvent(event);
            return;
        case cmInsertImage:
            insertImage();
            clearEvent(event);
            return;
        case cmInsertTable:
            insertTable();
            clearEvent(event);
            return;
        case cmTableInsertRowAbove:
            tableInsertRowAbove();
            clearEvent(event);
            return;
        case cmTableInsertRowBelow:
            tableInsertRowBelow();
            clearEvent(event);
            return;
        case cmTableDeleteRow:
            tableDeleteRow();
            clearEvent(event);
            return;
        case cmTableInsertColumnBefore:
            tableInsertColumnBefore();
            clearEvent(event);
            return;
        case cmTableInsertColumnAfter:
            tableInsertColumnAfter();
            clearEvent(event);
            return;
        case cmTableDeleteColumn:
            tableDeleteColumn();
            clearEvent(event);
            return;
        case cmTableDeleteTable:
            tableDeleteTable();
            clearEvent(event);
            return;
        case cmTableAlignDefault:
            tableAlignColumn(MarkdownTableAlignment::Default);
            clearEvent(event);
            return;
        case cmTableAlignLeft:
            tableAlignColumn(MarkdownTableAlignment::Left);
            clearEvent(event);
            return;
        case cmTableAlignCenter:
            tableAlignColumn(MarkdownTableAlignment::Center);
            clearEvent(event);
            return;
        case cmTableAlignRight:
            tableAlignColumn(MarkdownTableAlignment::Right);
            clearEvent(event);
            return;
        case cmTableAlignNumber:
            tableAlignColumn(MarkdownTableAlignment::Number);
            clearEvent(event);
            return;
        default:
            break;
        }
    }

    TPoint prevPos = curPos;
    TPoint prevDelta = delta;
    TFileEditor::handleEvent(event);
    if (prevPos != curPos || prevDelta != delta)
        notifyInfoView();
}

void MarkdownFileEditor::draw()
{
    if (!wrapEnabled)
    {
        TFileEditor::draw();
        notifyInfoView();
        return;
    }

    TAttrPair color = getColor(0x0201);
    uint linePtr = topLinePointer();
    int row = 0;
    while (row < size.y)
    {
        if (linePtr >= bufLen)
        {
            TDrawBuffer blank;
            blank.moveChar(0, ' ', color, size.x);
            writeLine(0, row, size.x, 1, blank);
            ++row;
            continue;
        }

        uint endPtr = lineEnd(linePtr);
        int lineLen = charPos(linePtr, endPtr);
        int bufferWidth = std::max(lineLen + 1, size.x);
        std::vector<TScreenCell> cells(bufferWidth);
        formatLine(cells.data(), linePtr, bufferWidth, color);

        if (lineLen == 0 && row < size.y)
        {
            TDrawBuffer blank;
            blank.moveChar(0, ' ', color, size.x);
            writeLine(0, row, size.x, 1, blank);
            ++row;
        }

        int offset = 0;
        while (offset < lineLen && row < size.y)
        {
            std::vector<TScreenCell> segment(size.x);
            int copyLen = std::min(size.x, lineLen - offset);
            for (int i = 0; i < copyLen; ++i)
                segment[i] = cells[offset + i];
            for (int i = copyLen; i < size.x; ++i)
            {
                ::setChar(segment[i], ' ');
                ::setAttr(segment[i], color); 
            }
            writeBuf(0, row, size.x, 1, segment.data());
            offset += copyLen;
            ++row;
        }
        linePtr = nextLine(linePtr);
    }
    notifyInfoView();
}

uint MarkdownFileEditor::topLinePointer()
{
    int diff = curPos.y - delta.y;
    uint pointer = curPtr;
    if (diff > 0)
        pointer = lineMove(pointer, -diff);
    else if (diff < 0)
        pointer = lineMove(pointer, -diff);
    return lineStart(pointer);
}

std::string MarkdownFileEditor::readRange(uint start, uint end)
{
    std::string result;
    for (uint i = start; i < end && i < bufLen; ++i)
        result.push_back(bufChar(i));
    return result;
}

void MarkdownFileEditor::replaceRange(uint start, uint end, const std::string &text)
{
    deleteRange(start, end, False);
    setCurPtr(start, 0);
    insertText(text.c_str(), text.size(), False);
}

std::string MarkdownFileEditor::lineText(uint linePtr)
{
    return readRange(linePtr, lineEnd(linePtr));
}

void MarkdownFileEditor::notifyInfoView()
{
    ++cachedStateVersion;
    if (infoView)
    {
        infoView->invalidateState();
        infoView->drawView();
    }
}

void MarkdownFileEditor::onContentModified()
{
    notifyInfoView();
}

std::string MarkdownFileEditor::makeTableRow(const std::vector<std::string> &cells) const
{
    std::ostringstream out;
    out << '|';
    if (cells.empty())
        out << '|';
    for (const auto &cell : cells)
    {
        out << ' ' << cell << ' ' << '|';
    }
    return out.str();
}

std::string MarkdownFileEditor::alignmentMarker(MarkdownTableAlignment alignment) const
{
    switch (alignment)
    {
    case MarkdownTableAlignment::Left:
        return ":---";
    case MarkdownTableAlignment::Center:
        return ":---:";
    case MarkdownTableAlignment::Right:
        return "---:";
    case MarkdownTableAlignment::Number:
        return "---::";
    default:
        return "---";
    }
}

std::string MarkdownFileEditor::makeTableAlignmentRow(int columnCount, const std::vector<MarkdownTableAlignment> &alignments) const
{
    std::ostringstream out;
    out << '|';
    for (int i = 0; i < columnCount; ++i)
    {
        MarkdownTableAlignment alignment = MarkdownTableAlignment::Default;
        if (i < static_cast<int>(alignments.size()))
            alignment = alignments[i];
        out << ' ' << alignmentMarker(alignment) << ' ' << '|';
    }
    return out.str();
}

MarkdownInfoView::MarkdownInfoView(const TRect &bounds, MarkdownFileEditor *editor) noexcept
    : TView(bounds), editor(editor)
{
    growMode = gfGrowHiY;
    eventMask = 0;
}

TPalette &MarkdownInfoView::getPalette() const
{
    static TPalette palette(cpGrayWindow, sizeof(cpGrayWindow) - 1);
    return palette;
}

MarkdownParserState MarkdownInfoView::computeState(uint topPtr)
{
    if (!editor)
        return MarkdownParserState{};
    if (cachedPrefixPtr == topPtr && cachedVersion == editor->stateVersion())
        return cachedState;
    MarkdownParserState state;
    uint ptr = 0;
    while (ptr < topPtr && ptr < editor->bufLen)
    {
        uint end = editor->lineEnd(ptr);
        std::string line = editor->readRange(ptr, end);
        editor->analyzer().analyzeLine(line, state);
        uint next = editor->nextLine(ptr);
        if (next <= ptr)
            break;
        ptr = next;
    }
    cachedState = state;
    cachedPrefixPtr = topPtr;
    cachedVersion = editor->stateVersion();
    return cachedState;
}

void MarkdownInfoView::draw()
{
    TAttrPair color = getColor(0x0301);
    TAttrPair highlight = getColor(0x0302);
    if (!editor || !editor->isMarkdownMode())
    {
        TDrawBuffer buffer;
        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', color, size.x);
            if (y == 0)
                buffer.moveStr(1, "Plain Text", color);
            writeLine(0, y, size.x, 1, buffer);
        }
        return;
    }

    uint topPtr = editor->topLinePointer();
    MarkdownParserState state = computeState(topPtr);
    uint linePtr = topPtr;
    int lineNumber = editor->delta.y;

    for (int row = 0; row < size.y; ++row)
    {
        TDrawBuffer buffer;
        buffer.moveChar(0, ' ', color, size.x);
        if (linePtr < editor->bufLen)
        {
            MarkdownLineInfo info = editor->analyzer().analyzeLine(editor->lineText(linePtr), state);
            std::string label = editor->analyzer().describeLine(info);
            if (lineNumber == editor->curPos.y)
            {
                std::string tableLabel;
                if (info.kind == MarkdownLineKind::TableRow || info.kind == MarkdownLineKind::TableSeparator)
                {
                    int columnIndex = -1;
                    if (!info.tableCells.empty())
                    {
                        for (std::size_t i = 0; i < info.tableCells.size(); ++i)
                        {
                            const auto &cell = info.tableCells[i];
                            auto endCol = std::max(cell.endColumn, cell.startColumn + 1);
                            if (editor->curPos.x >= static_cast<int>(cell.startColumn) &&
                                editor->curPos.x < static_cast<int>(endCol))
                            {
                                columnIndex = static_cast<int>(i);
                                break;
                            }
                        }
                        if (columnIndex == -1)
                            columnIndex = static_cast<int>(info.tableCells.size()) - 1;
                    }
                    if (columnIndex >= 0)
                        tableLabel = sanitizeMultiline(
                            editor->analyzer().describeTableCell(info, static_cast<std::size_t>(columnIndex)));
                }

                const auto *span = editor->analyzer().spanAtColumn(info, editor->curPos.x);
                if (!tableLabel.empty())
                {
                    if (span && span->kind != MarkdownSpanKind::PlainText)
                    {
                        std::string spanLabel = sanitizeMultiline(editor->analyzer().describeSpan(*span));
                        if (!spanLabel.empty())
                        {
                            tableLabel.push_back(' ');
                            tableLabel.append("— ");
                            tableLabel.append(spanLabel);
                        }
                    }
                    label = tableLabel;
                }
                else if (span)
                {
                    label = sanitizeMultiline(editor->analyzer().describeSpan(*span));
                }
            }
            if (lineNumber == editor->curPos.y)
                buffer.moveCStr(0, label, highlight, size.x);
            else
                buffer.moveCStr(0, label, color, size.x);
            linePtr = editor->nextLine(linePtr);
            ++lineNumber;
        }
        else
        {
            if (lineNumber == editor->curPos.y)
                buffer.moveCStr(0, "End of File", highlight, size.x);
        }
        writeLine(0, row, size.x, 1, buffer);
    }
}

MarkdownEditWindow::MarkdownEditWindow(const TRect &bounds, TStringView fileName, int aNumber) noexcept
    : TWindowInit(&TWindow::initFrame), TWindow(bounds, nullptr, aNumber)
{
    options |= ofTileable;

    indicator = new TIndicator(TRect(2, size.y - 1, 2 + kInfoColumnWidth - 2, size.y));
    insert(indicator);

    hScrollBar = new TScrollBar(TRect(1 + kInfoColumnWidth, size.y - 1, size.x - 2, size.y));
    insert(hScrollBar);

    vScrollBar = new TScrollBar(TRect(size.x - 1, 1, size.x, size.y - 1));
    insert(vScrollBar);

    TRect infoRect(1, 1, 1 + kInfoColumnWidth, size.y - 1);
    infoView = new MarkdownInfoView(infoRect, nullptr);
    insert(infoView);

    TRect editorRect(1 + kInfoColumnWidth, 1, size.x - 1, size.y - 1);
    fileEditor = new MarkdownFileEditor(editorRect, hScrollBar, vScrollBar, indicator, fileName);
    insert(fileEditor);
    infoView->setEditor(fileEditor);
    fileEditor->setInfoView(infoView);
}

MarkdownEditorApp::MarkdownEditorApp(int argc, char **argv)
    : TProgInit(&MarkdownEditorApp::initStatusLine, &MarkdownEditorApp::initMenuBar, &TApplication::initDeskTop),
      TApplication()
{
    TCommandSet ts;
    ts.enableCmd(cmSave);
    ts.enableCmd(cmSaveAs);
    ts.enableCmd(cmCut);
    ts.enableCmd(cmCopy);
    ts.enableCmd(cmPaste);
    ts.enableCmd(cmClear);
    ts.enableCmd(cmUndo);
    ts.enableCmd(cmFind);
    ts.enableCmd(cmReplace);
    ts.enableCmd(cmSearchAgain);
    disableCommands(ts);

    while (--argc > 0)
        openEditor(*++argv, True);
    cascade();
}

MarkdownEditWindow *MarkdownEditorApp::openEditor(const char *fileName, Boolean visible)
{
    TRect r = deskTop->getExtent();
    auto *win = (MarkdownEditWindow *)validView(new MarkdownEditWindow(r, fileName, wnNoNumber));
    if (!win)
        return nullptr;
    if (!visible)
        win->hide();
    deskTop->insert(win);
    return win;
}

void MarkdownEditorApp::fileOpen()
{
    char name[MAXPATH] = "*.md";
    if (execDialog(new TFileDialog("*.*", "Open file", "~N~ame", fdOpenButton, 100), name) != cmCancel)
        openEditor(name, True);
}

void MarkdownEditorApp::fileNew()
{
    openEditor(nullptr, True);
}

void MarkdownEditorApp::changeDir()
{
    execDialog(new TChDirDialog(cdNormal, 0), nullptr);
}

void MarkdownEditorApp::showAbout()
{
    ck::ui::showAboutDialog("ck-edit", CK_EDIT_VERSION,
                            "Edit text and Markdown documents with live structural hints.");
}

void MarkdownEditorApp::dispatchToEditor(ushort command)
{
    if (!deskTop->current)
        return;
    auto *win = dynamic_cast<MarkdownEditWindow *>(deskTop->current);
    if (!win)
        return;
    TEvent ev;
    ev.what = evCommand;
    ev.message.command = command;
    win->editor()->handleEvent(ev);
}

void MarkdownEditorApp::handleEvent(TEvent &event)
{
    TApplication::handleEvent(event);
    if (event.what != evCommand)
        return;

    switch (event.message.command)
    {
    case cmOpen:
        fileOpen();
        break;
    case cmNew:
        fileNew();
        break;
    case cmChangeDir:
        changeDir();
        break;
    case cmToggleWrap:
    case cmToggleMarkdownMode:
    case cmHeading1:
    case cmHeading2:
    case cmHeading3:
    case cmHeading4:
    case cmHeading5:
    case cmHeading6:
    case cmClearHeading:
    case cmBold:
    case cmItalic:
    case cmBoldItalic:
    case cmRemoveFormatting:
    case cmBlockQuote:
    case cmBlockQuoteClear:
    case cmInsertBulletList:
    case cmInsertNumberedList:
    case cmInsertLink:
    case cmInsertImage:
    case cmInsertTable:
    case cmTableInsertRowAbove:
    case cmTableInsertRowBelow:
    case cmTableDeleteRow:
    case cmTableInsertColumnBefore:
    case cmTableInsertColumnAfter:
    case cmTableDeleteColumn:
    case cmTableDeleteTable:
    case cmTableAlignDefault:
    case cmTableAlignLeft:
    case cmTableAlignCenter:
    case cmTableAlignRight:
    case cmTableAlignNumber:
        dispatchToEditor(event.message.command);
        break;
    case cmAbout:
        showAbout();
        break;
    default:
        return;
    }
    clearEvent(event);
}

TMenuBar *MarkdownEditorApp::initMenuBar(TRect r)
{
    r.b.y = r.a.y + 1;
    return new TMenuBar(r,
                        *new TSubMenu("~F~ile", kbAltF) +
                            *new TMenuItem("~O~pen", cmOpen, kbF3, hcNoContext, "F3") +
                            *new TMenuItem("~N~ew", cmNew, kbCtrlN, hcNoContext, "Ctrl-N") +
                            *new TMenuItem("~S~ave", cmSave, kbF2, hcNoContext, "F2") +
                            *new TMenuItem("S~a~ve as...", cmSaveAs, kbNoKey) +
                            newLine() +
                            *new TMenuItem("~C~hange dir...", cmChangeDir, kbNoKey) +
                            *new TMenuItem("E~x~it", cmQuit, kbCtrlQ, hcNoContext, "Ctrl-Q") +
                        *new TSubMenu("~E~dit", kbAltE) +
                            *new TMenuItem("~U~ndo", cmUndo, kbCtrlU, hcNoContext, "Ctrl-U") +
                            newLine() +
                            *new TMenuItem("Cu~t~", cmCut, kbShiftDel, hcNoContext, "Shift-Del") +
                            *new TMenuItem("~C~opy", cmCopy, kbCtrlIns, hcNoContext, "Ctrl-Ins") +
                            *new TMenuItem("~P~aste", cmPaste, kbShiftIns, hcNoContext, "Shift-Ins") +
                            newLine() +
                            *new TMenuItem("~F~ind...", cmFind, kbCtrlF, hcNoContext, "Ctrl-F") +
                            *new TMenuItem("~R~eplace...", cmReplace, kbCtrlR, hcNoContext, "Ctrl-R") +
                            *new TMenuItem("Find ~N~ext", cmSearchAgain, kbCtrlL, hcNoContext, "Ctrl-L") +
                        *new TSubMenu("~S~tyle", kbAltS) +
                            *new TMenuItem("Heading ~1", cmHeading1, kbNoKey) +
                            *new TMenuItem("Heading ~2", cmHeading2, kbNoKey) +
                            *new TMenuItem("Heading ~3", cmHeading3, kbNoKey) +
                            *new TMenuItem("Heading ~4", cmHeading4, kbNoKey) +
                            *new TMenuItem("Heading ~5", cmHeading5, kbNoKey) +
                            *new TMenuItem("Heading ~6", cmHeading6, kbNoKey) +
                            newLine() +
                            *new TMenuItem("~C~lear heading", cmClearHeading, kbNoKey) +
                            newLine() +
                            *new TMenuItem("~B~old", cmBold, kbCtrlB, hcNoContext, "Ctrl-B") +
                            *new TMenuItem("~I~talic", cmItalic, kbCtrlI, hcNoContext, "Ctrl-I") +
                            *new TMenuItem("Bold + Italic", cmBoldItalic, kbNoKey) +
                            *new TMenuItem("~R~emove formatting", cmRemoveFormatting, kbNoKey) +
                            newLine() +
                            *new TMenuItem("Block ~q~uote", cmBlockQuote, kbNoKey) +
                            *new TMenuItem("Remove blockquote", cmBlockQuoteClear, kbNoKey) +
                        *new TSubMenu("~I~nsert", kbAltI) +
                            *new TMenuItem("Bullet list...", cmInsertBulletList, kbNoKey) +
                            *new TMenuItem("Numbered list...", cmInsertNumberedList, kbNoKey) +
                            *new TMenuItem("Link...", cmInsertLink, kbNoKey) +
                            *new TMenuItem("Image...", cmInsertImage, kbNoKey) +
                            *new TMenuItem("Table...", cmInsertTable, kbNoKey) +
                        *new TSubMenu("~T~able", kbAltT) +
                            *new TMenuItem("Insert row ~a~bove", cmTableInsertRowAbove, kbNoKey) +
                            *new TMenuItem("Insert row ~b~elow", cmTableInsertRowBelow, kbNoKey) +
                            *new TMenuItem("Delete row", cmTableDeleteRow, kbNoKey) +
                            newLine() +
                            *new TMenuItem("Insert column ~b~efore", cmTableInsertColumnBefore, kbNoKey) +
                            *new TMenuItem("Insert column ~a~fter", cmTableInsertColumnAfter, kbNoKey) +
                            *new TMenuItem("Delete column", cmTableDeleteColumn, kbNoKey) +
                            newLine() +
                            *new TMenuItem("Align ~d~efault", cmTableAlignDefault, kbNoKey) +
                            *new TMenuItem("Align ~l~eft", cmTableAlignLeft, kbNoKey) +
                            *new TMenuItem("Align ~c~enter", cmTableAlignCenter, kbNoKey) +
                            *new TMenuItem("Align ~r~ight", cmTableAlignRight, kbNoKey) +
                            *new TMenuItem("Align ~n~umber", cmTableAlignNumber, kbNoKey) +
                            newLine() +
                            *new TMenuItem("Delete table", cmTableDeleteTable, kbNoKey) +
                        *new TSubMenu("~V~iew", kbAltV) +
                            *new TMenuItem("Toggle ~w~rap", cmToggleWrap, kbCtrlW, hcNoContext, "Ctrl-W") +
                            *new TMenuItem("Toggle ~M~arkdown mode", cmToggleMarkdownMode, kbCtrlM, hcNoContext, "Ctrl-M") +
                        *new TSubMenu("~H~elp", kbAltH) +
                            *new TMenuItem("~A~bout", cmAbout, kbNoKey));
}

TStatusLine *MarkdownEditorApp::initStatusLine(TRect r)
{
    r.a.y = r.b.y - 1;
    return new TStatusLine(r,
                           *new TStatusDef(0, 0xFFFF) +
                               *new TStatusItem("~F2~ Save", kbF2, cmSave) +
                               *new TStatusItem("~F3~ Open", kbF3, cmOpen) +
                               *new TStatusItem("~Ctrl-W~ Wrap", kbCtrlW, cmToggleWrap) +
                               *new TStatusItem("~Ctrl-M~ Markdown", kbCtrlM, cmToggleMarkdownMode) +
                               *new TStatusItem("~Ctrl-B~ Bold", kbCtrlB, cmBold) +
                               *new TStatusItem("~Ctrl-I~ Italic", kbCtrlI, cmItalic));
}

} // namespace ck::edit

