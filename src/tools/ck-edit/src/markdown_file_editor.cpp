#include "ck/edit/markdown_editor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ck::edit
{
    namespace
    {
        const std::array<std::string_view, 7> kMarkdownExtensions = {
            ".md", ".markdown", ".mdown", ".mkd", ".mkdn", ".mdtxt", ".mdtext"};

        bool cellIsWhitespace(const TScreenCell &cell)
        {
            if (cell._ch.isWideCharTrail())
                return false;
            TStringView text = cell._ch.getText();
            if (text.empty())
                return false;
            unsigned char ch = static_cast<unsigned char>(text[0]);
            return ch == ' ';
        }

        bool cellBreaksAfter(const TScreenCell &cell)
        {
            if (cell._ch.isWideCharTrail())
                return false;
            TStringView text = cell._ch.getText();
            if (text.empty())
                return false;
            unsigned char ch = static_cast<unsigned char>(text[0]);
            return ch == '-' || ch == '/';
        }

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

        const std::unordered_map<ushort, MarkdownFileEditor::InlineCommandSpec> kInlineCommandSpecs = {
            {cmBold, {"Bold", "**", "**", "", false, true, MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
            {cmItalic, {"Italic", "*", "*", "", false, true, MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
            {cmBoldItalic, {"Bold + Italic", "***", "***", "", false, true, MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
            {cmStrikethrough, {"Strikethrough", "~~", "~~", "", false, true, MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
            {cmInlineCode, {"Inline Code", "`", "`", "", false, true, MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
        };

        std::string columnLabel(int index)
        {
            if (index < 0)
                return "?";
            std::string name;
            int value = index;
            do
            {
                name.push_back(static_cast<char>('A' + (value % 26)));
                value /= 26;
            } while (value > 0);
            std::reverse(name.begin(), name.end());
            return name;
        }
    } // namespace

    bool MarkdownFileEditor::isMarkdownFileName(std::string_view path)
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

    MarkdownFileEditor::MarkdownFileEditor(const TRect &bounds, TScrollBar *hScroll,
                                           TScrollBar *vScroll, TIndicator *indicator,
                                           TStringView fileName) noexcept
        : TFileEditor(bounds, hScroll, vScroll, indicator, fileName)
    {
        if (!fileName.empty())
            markdownMode = isMarkdownFileName(std::string(fileName));
        else
            markdownMode = false;
        refreshCursorMetrics();
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
        {
            delta.x = 0;
            wrapTopSegmentOffset = 0;
            wrapDesiredVisualColumn = -1;
            updateWrapStateAfterMovement(false);
        }
        else
        {
            wrapTopSegmentOffset = 0;
            wrapDesiredVisualColumn = -1;
        }
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

    void MarkdownFileEditor::setMarkdownMode(bool value) noexcept
    {
        if (markdownMode == value)
            return;
        markdownMode = value;
        if (hostWindow)
            hostWindow->updateLayoutForMode();
        else
            notifyInfoView();
    }

    void MarkdownFileEditor::toggleMarkdownMode()
    {
        setMarkdownMode(!markdownMode);
    }

    void MarkdownFileEditor::applyHeadingLevel(int level)
    {
        if (level < 1)
        {
            clearHeading();
            return;
        }
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        for (auto &line : lines)
        {
            std::size_t index = 0;
            while (index < line.size() && (line[index] == ' ' || line[index] == '\t'))
                ++index;
            std::size_t markerEnd = index;
            while (markerEnd < line.size() && line[markerEnd] == '#')
                ++markerEnd;
            if (markerEnd < line.size() && line[markerEnd] == ' ')
                ++markerEnd;

            int existingLevel = static_cast<int>(markerEnd - index);
            if (existingLevel > 0 && markerEnd > index && line[markerEnd - 1] == ' ')
                --existingLevel;

            std::string content = trimLeft(std::string_view(line).substr(markerEnd));
            std::string indent = line.substr(0, index);
            if (existingLevel == level && existingLevel > 0)
            {
                line = indent + content;
            }
            else
            {
                std::string replacement = indent + std::string(level, '#');
                if (!content.empty())
                {
                    replacement.push_back(' ');
                    replacement.append(content);
                }
                else
                {
                    replacement.push_back(' ');
                }
                line = replacement;
            }
        }
        applyBlockSelection(block, lines, block.trailingNewline);
    }

    void MarkdownFileEditor::clearHeading()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        bool modified = false;
        for (auto &line : lines)
        {
            std::size_t index = 0;
            while (index < line.size() && (line[index] == ' ' || line[index] == '\t'))
                ++index;
            std::size_t markerEnd = index;
            while (markerEnd < line.size() && line[markerEnd] == '#')
                ++markerEnd;
            if (markerEnd > index)
            {
                if (markerEnd < line.size() && line[markerEnd] == ' ')
                    ++markerEnd;
                std::string content = trimLeft(std::string_view(line).substr(markerEnd));
                line = line.substr(0, index) + content;
                modified = true;
            }
        }
        if (modified)
            applyBlockSelection(block, lines, block.trailingNewline);
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

    MarkdownFileEditor::BlockSelection MarkdownFileEditor::captureSelectedLines()
    {
        BlockSelection selection;
        uint selectionStart = hasSelection() ? std::min(selStart, selEnd) : curPtr;
        uint selectionEnd = hasSelection() ? std::max(selStart, selEnd) : curPtr;
        selection.start = lineStart(selectionStart);
        uint lastLineStart = lineStart(selectionEnd);
        uint afterEnd = nextLine(lastLineStart);
        if (afterEnd <= lastLineStart)
            afterEnd = lineEnd(lastLineStart);
        selection.end = afterEnd;
        std::string text = readRange(selection.start, selection.end);
        selection.trailingNewline = !text.empty() && text.back() == '\n';
        std::size_t pos = 0;
        while (pos < text.size())
        {
            std::size_t next = text.find('\n', pos);
            if (next == std::string::npos)
            {
                selection.lines.push_back(text.substr(pos));
                break;
            }
            selection.lines.push_back(text.substr(pos, next - pos));
            pos = next + 1;
        }
        if (selection.lines.empty())
            selection.lines.emplace_back();
        return selection;
    }

    void MarkdownFileEditor::applyBlockSelection(const BlockSelection &selection, const std::vector<std::string> &lines, bool trailingNewline)
    {
        std::string result;
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            if (i > 0)
                result.push_back('\n');
            result.append(lines[i]);
        }
        if (trailingNewline)
            result.push_back('\n');

        lock();
        replaceRange(selection.start, selection.end, result);
        unlock();
        onContentModified();
    }

    std::string MarkdownFileEditor::trimLeft(std::string_view text)
    {
        std::size_t start = 0;
        while (start < text.size() && (text[start] == ' ' || text[start] == '\t'))
            ++start;
        return std::string(text.substr(start));
    }

    std::string MarkdownFileEditor::trim(std::string_view text)
    {
        std::size_t start = 0;
        std::size_t end = text.size();
        while (start < end && (text[start] == ' ' || text[start] == '\t'))
            ++start;
        while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t'))
            --end;
        return std::string(text.substr(start, end - start));
    }

    bool MarkdownFileEditor::lineIsWhitespace(const std::string &line)
    {
        for (char ch : line)
        {
            if (ch != ' ' && ch != '\t' && ch != '\r')
                return false;
        }
        return true;
    }

    MarkdownFileEditor::LinePattern MarkdownFileEditor::analyzeLinePattern(const std::string &line) const
    {
        LinePattern pattern;
        std::size_t pos = 0;
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
            ++pos;
        pattern.indent = line.substr(0, pos);
        std::size_t blockStart = pos;
        while (pos < line.size() && line[pos] == '>')
        {
            ++pos;
            if (pos < line.size() && line[pos] == ' ')
                ++pos;
        }
        pattern.blockquote = line.substr(blockStart, pos - blockStart);
        pattern.markerStart = pos;
        std::size_t markerEnd = pos;
        if (pos < line.size())
        {
            char ch = line[pos];
            if (ch == '-' || ch == '*' || ch == '+')
            {
                pattern.hasBullet = true;
                pattern.bulletChar = ch;
                markerEnd = pos + 1;
                while (markerEnd < line.size() && (line[markerEnd] == ' ' || line[markerEnd] == '\t'))
                    ++markerEnd;
                if (markerEnd + 2 < line.size() && line[markerEnd] == '[' && line[markerEnd + 2] == ']')
                {
                    pattern.hasTask = true;
                    markerEnd += 3;
                    if (markerEnd < line.size() && (line[markerEnd] == ' ' || line[markerEnd] == '\t'))
                        ++markerEnd;
                }
            }
            else if (std::isdigit(static_cast<unsigned char>(ch)))
            {
                std::size_t digitsEnd = pos;
                while (digitsEnd < line.size() && std::isdigit(static_cast<unsigned char>(line[digitsEnd])))
                    ++digitsEnd;
                if (digitsEnd > pos && digitsEnd < line.size() && line[digitsEnd] == '.')
                {
                    markerEnd = digitsEnd + 1;
                    while (markerEnd < line.size() && (line[markerEnd] == ' ' || line[markerEnd] == '\t'))
                        ++markerEnd;
                    pattern.hasOrdered = true;
                }
            }
        }
        pattern.markerEnd = markerEnd;
        return pattern;
    }

    std::string MarkdownFileEditor::generateUniqueReferenceId(const std::string &prefix)
    {
        std::set<std::string> ids;
        std::string text = readRange(0, bufLen);
        std::size_t pos = 0;
        while (pos < text.size())
        {
            std::size_t end = text.find('\n', pos);
            std::string_view line(text.c_str() + pos, (end == std::string::npos ? text.size() - pos : end - pos));
            std::size_t start = 0;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
                ++start;
            if (start < line.size() && line[start] == '[')
            {
                std::size_t close = line.find(']', start);
                if (close != std::string::npos && close + 1 < line.size() && line[close + 1] == ':')
                {
                    ids.emplace(std::string(line.substr(start + 1, close - start - 1)));
                }
            }
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }

        if (prefix.empty())
            return "ref1";

        for (int i = 1; i < 10000; ++i)
        {
            std::string candidate = prefix + std::to_string(i);
            if (!ids.count(candidate))
                return candidate;
        }
        return prefix + "x";
    }

    std::string MarkdownFileEditor::generateUniqueFootnoteId()
    {
        std::set<std::string> ids;
        std::string text = readRange(0, bufLen);
        std::size_t pos = 0;
        while (pos < text.size())
        {
            std::size_t end = text.find('\n', pos);
            std::string_view line(text.c_str() + pos, (end == std::string::npos ? text.size() - pos : end - pos));
            std::size_t start = 0;
            while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
                ++start;
            if (start + 2 < line.size() && line[start] == '[' && line[start + 1] == '^')
            {
                std::size_t close = line.find(']', start);
                if (close != std::string::npos && close + 1 < line.size() && line[close + 1] == ':')
                    ids.emplace(std::string(line.substr(start + 2, close - start - 2)));
            }
            if (end == std::string::npos)
                break;
            pos = end + 1;
        }

        for (int i = 1; i < 10000; ++i)
        {
            std::string candidate = "fn" + std::to_string(i);
            if (!ids.count(candidate))
                return candidate;
        }
        return "fn";
    }

    void MarkdownFileEditor::appendDefinition(const std::string &definition)
    {
        lock();
        setCurPtr(bufLen, 0);
        if (bufLen > 0 && bufChar(bufLen - 1) != '\n')
            insertText("\n", 1, False);
        insertText(definition.c_str(), definition.size(), False);
        unlock();
        onContentModified();
    }

    void MarkdownFileEditor::applyInlineCommand(const InlineCommandSpec &spec)
    {
        uint start = hasSelection() ? std::min(selStart, selEnd) : curPtr;
        uint end = hasSelection() ? std::max(selStart, selEnd) : curPtr;
        bool hadSelection = start != end;

        if (hadSelection)
        {
            std::string text = readRange(start, end);
            if (!spec.prefix.empty() || !spec.suffix.empty())
            {
                if (text.size() >= spec.prefix.size() + spec.suffix.size() &&
                    text.rfind(spec.prefix, 0) == 0 &&
                    text.substr(text.size() - spec.suffix.size()) == spec.suffix)
                {
                    std::string inner = text.substr(spec.prefix.size(), text.size() - spec.prefix.size() - spec.suffix.size());
                    lock();
                    replaceRange(start, end, inner);
                    unlock();
                    setSelect(start, start + inner.size(), True);
                    onContentModified();
                    return;
                }
            }

            lock();
            setCurPtr(start, 0);
            if (!spec.prefix.empty())
                insertText(spec.prefix.c_str(), spec.prefix.size(), False);
            setCurPtr(end + spec.prefix.size(), 0);
            if (!spec.suffix.empty())
                insertText(spec.suffix.c_str(), spec.suffix.size(), False);
            unlock();

            if (spec.keepSelection)
            {
                uint innerStart = start + spec.prefix.size();
                uint innerEnd = innerStart + (end - start);
                setSelect(innerStart, innerEnd, True);
            }
            else
            {
                uint caretPos = end + spec.prefix.size();
                setCurPtr(caretPos, 0);
            }

            onContentModified();
            return;
        }

        lock();
        setCurPtr(start, 0);
        if (!spec.prefix.empty())
            insertText(spec.prefix.c_str(), spec.prefix.size(), False);
        if (!spec.placeholder.empty())
            insertText(spec.placeholder.c_str(), spec.placeholder.size(), False);
        if (!spec.suffix.empty())
            insertText(spec.suffix.c_str(), spec.suffix.size(), False);
        unlock();

        uint afterPrefix = start + spec.prefix.size();
        uint afterPlaceholder = afterPrefix + spec.placeholder.size();
        uint afterSuffix = afterPlaceholder + spec.suffix.size();

        uint caretPos = afterPrefix;
        switch (spec.cursorPlacement)
        {
        case InlineCommandSpec::CursorPlacement::AfterPrefix:
            caretPos = afterPrefix;
            break;
        case InlineCommandSpec::CursorPlacement::AfterPlaceholder:
            caretPos = afterPlaceholder;
            break;
        case InlineCommandSpec::CursorPlacement::AfterSuffix:
            caretPos = afterSuffix;
            break;
        }

        setCurPtr(caretPos, 0);
        if (spec.selectPlaceholder && afterPlaceholder > afterPrefix)
            setSelect(afterPrefix, afterPlaceholder, True);

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

        std::size_t leadingTicks = 0;
        while (leadingTicks < text.size() && text[leadingTicks] == '`')
            ++leadingTicks;
        std::size_t trailingTicks = 0;
        while (trailingTicks < text.size() && text[text.size() - 1 - trailingTicks] == '`')
            ++trailingTicks;
        if (leadingTicks > 0 && leadingTicks == trailingTicks && leadingTicks * 2 <= text.size())
        {
            std::string inner = text.substr(leadingTicks, text.size() - 2 * leadingTicks);
            replaceRange(start, end, inner);
            setSelect(start, start + inner.size(), True);
            onContentModified();
        }
    }

    void MarkdownFileEditor::applyBold()
    {
        applyInlineCommand(kInlineCommandSpecs.at(cmBold));
    }

    void MarkdownFileEditor::applyItalic()
    {
        applyInlineCommand(kInlineCommandSpecs.at(cmItalic));
    }

    void MarkdownFileEditor::applyBoldItalic()
    {
        applyInlineCommand(kInlineCommandSpecs.at(cmBoldItalic));
    }

    void MarkdownFileEditor::applyStrikethrough()
    {
        applyInlineCommand(kInlineCommandSpecs.at(cmStrikethrough));
    }

    void MarkdownFileEditor::applyInlineCode()
    {
        const auto &spec = kInlineCommandSpecs.at(cmInlineCode);

        if (!hasSelection())
        {
            applyInlineCommand(spec);
            return;
        }

        uint start = std::min(selStart, selEnd);
        uint end = std::max(selStart, selEnd);
        if (start == end)
        {
            applyInlineCommand(spec);
            return;
        }

        std::string text = readRange(start, end);
        std::size_t leading = 0;
        while (leading < text.size() && text[leading] == '`')
            ++leading;
        std::size_t trailing = 0;
        while (trailing < text.size() && text[text.size() - 1 - trailing] == '`')
            ++trailing;
        if (leading > 0 && leading == trailing && leading * 2 <= text.size())
        {
            std::string inner = text.substr(leading, text.size() - 2 * leading);
            lock();
            replaceRange(start, end, inner);
            unlock();
            setSelect(start, start + inner.size(), True);
            onContentModified();
            return;
        }

        std::size_t longest = 0;
        std::size_t current = 0;
        for (char ch : text)
        {
            if (ch == '`')
            {
                ++current;
                longest = std::max(longest, current);
            }
            else
            {
                current = 0;
            }
        }
        std::string fence(longest + 1, '`');
        lock();
        setCurPtr(start, 0);
        insertText(fence.c_str(), fence.size(), False);
        setCurPtr(end + fence.size(), 0);
        insertText(fence.c_str(), fence.size(), False);
        unlock();
        uint innerStart = start + static_cast<uint>(fence.size());
        uint innerEnd = innerStart + (end - start);
        setSelect(innerStart, innerEnd, True);
        onContentModified();
    }

    void MarkdownFileEditor::toggleCodeBlock()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        auto trimmed = [&](const std::string &line) { return trim(std::string_view(line)); };

        int first = 0;
        while (first < static_cast<int>(lines.size()) && trimmed(lines[first]).empty())
            ++first;
        int last = static_cast<int>(lines.size()) - 1;
        while (last >= first && trimmed(lines[last]).empty())
            --last;

        bool hasFence = false;
        if (first < last)
        {
            std::string firstLine = trimmed(lines[first]);
            std::string lastLine = trimmed(lines[last]);
            if (firstLine.rfind("```", 0) == 0 && lastLine.rfind("```", 0) == 0)
                hasFence = true;
        }

        if (hasFence)
        {
            lines.erase(lines.begin() + first);
            for (int i = static_cast<int>(lines.size()) - 1; i >= 0; --i)
            {
                if (trimmed(lines[i]).rfind("```", 0) == 0)
                {
                    lines.erase(lines.begin() + i);
                    break;
                }
            }
            applyBlockSelection(block, lines, true);
            return;
        }

        std::string language = trim(promptForText("Code Block", "Language (optional)", ""));
        std::string fence = "```";
        if (!language.empty())
            fence += language;

        std::vector<std::string> result;
        result.push_back(fence);
        result.insert(result.end(), lines.begin(), lines.end());
        result.push_back("```");
        applyBlockSelection(block, result, true);
    }

    void MarkdownFileEditor::makeParagraph()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        for (auto &line : lines)
        {
            LinePattern pattern = analyzeLinePattern(line);
            std::string content = trimLeft(std::string_view(line).substr(pattern.markerEnd));
            line = pattern.indent + content;
        }

        auto isBlank = [&](const std::string &line) { return trimLeft(line).empty(); };
        while (!lines.empty() && isBlank(lines.front()))
            lines.erase(lines.begin());
        while (!lines.empty() && isBlank(lines.back()))
            lines.pop_back();
        if (lines.empty())
            lines.emplace_back();

        bool needBefore = false;
        if (block.start > 0)
        {
            uint prevStart = lineMove(block.start, -1);
            if (prevStart < block.start)
            {
                std::string prevLine = readRange(prevStart, lineEnd(prevStart));
                while (!prevLine.empty() && (prevLine.back() == '\n' || prevLine.back() == '\r'))
                    prevLine.pop_back();
                if (!lineIsWhitespace(prevLine))
                    needBefore = true;
            }
        }

        bool needAfter = false;
        if (block.end < bufLen)
        {
            uint nextStart = block.end;
            std::string nextLine = readRange(nextStart, lineEnd(nextStart));
            while (!nextLine.empty() && (nextLine.back() == '\n' || nextLine.back() == '\r'))
                nextLine.pop_back();
            if (!lineIsWhitespace(nextLine))
                needAfter = true;
        }

        if (needBefore && (lines.empty() || !isBlank(lines.front())))
            lines.insert(lines.begin(), std::string());
        if (needAfter && (lines.empty() || !isBlank(lines.back())))
            lines.push_back(std::string());

        applyBlockSelection(block, lines, true);
    }

    void MarkdownFileEditor::insertLineBreak()
    {
        lock();
        insertText("  \n", 3, False);
        unlock();
        onContentModified();
    }

    void MarkdownFileEditor::toggleBlockQuote()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        bool allQuoted = true;
        for (const auto &line : lines)
        {
            if (trimLeft(line).empty())
                continue;
            LinePattern pattern = analyzeLinePattern(line);
            if (pattern.blockquote.empty())
            {
                allQuoted = false;
                break;
            }
        }

        for (auto &line : lines)
        {
            LinePattern pattern = analyzeLinePattern(line);
            if (allQuoted)
            {
                if (!pattern.blockquote.empty())
                {
                    std::size_t removeStart = pattern.indent.size();
                    std::size_t removeEnd = removeStart + pattern.blockquote.size();
                    line = line.substr(0, removeStart) + line.substr(removeEnd);
                }
            }
            else
            {
                if (pattern.blockquote.empty())
                    line = pattern.indent + "> " + line.substr(pattern.indent.size());
            }
        }
        applyBlockSelection(block, lines, block.trailingNewline);
    }

    void MarkdownFileEditor::toggleBulletList()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        for (auto &line : lines)
        {
            if (trimLeft(line).empty())
                continue;
            LinePattern pattern = analyzeLinePattern(line);
            std::string content = trimLeft(std::string_view(line).substr(pattern.markerEnd));
            line = pattern.indent + pattern.blockquote + "- " + content;
        }
        applyBlockSelection(block, lines, block.trailingNewline);
    }

    void MarkdownFileEditor::toggleNumberedList()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        for (auto &line : lines)
        {
            if (trimLeft(line).empty())
                continue;
            LinePattern pattern = analyzeLinePattern(line);
            std::string content = trimLeft(std::string_view(line).substr(pattern.markerEnd));
            line = pattern.indent + pattern.blockquote + "1. " + content;
        }
        applyBlockSelection(block, lines, block.trailingNewline);
    }

    void MarkdownFileEditor::convertToTaskList()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        for (auto &line : lines)
        {
            if (trimLeft(line).empty())
                continue;
            LinePattern pattern = analyzeLinePattern(line);
            bool checked = false;
            std::size_t bracket = line.find('[', pattern.markerStart);
            if (bracket != std::string::npos && bracket + 2 < line.size())
            {
                char mark = line[bracket + 1];
                if (mark == 'x' || mark == 'X')
                    checked = true;
            }
            std::string content = trimLeft(std::string_view(line).substr(pattern.markerEnd));
            line = pattern.indent + pattern.blockquote + "- [" + std::string(1, checked ? 'x' : ' ') + "] " + content;
        }
        applyBlockSelection(block, lines, block.trailingNewline);
    }

    void MarkdownFileEditor::toggleTaskCheckbox()
    {
        uint lineStartPtr = lineStart(curPtr);
        uint lineEndPtr = lineEnd(lineStartPtr);
        std::string line = readRange(lineStartPtr, lineEndPtr);
        bool hadNewline = false;
        if (!line.empty() && line.back() == '\n')
        {
            hadNewline = true;
            line.pop_back();
        }

        LinePattern pattern = analyzeLinePattern(line);
        std::size_t bracket = line.find('[', pattern.markerStart);
        if (bracket == std::string::npos || bracket + 2 >= line.size())
            return;
        if (line[bracket + 2] != ']')
            return;

        char current = line[bracket + 1];
        if (current == 'x' || current == 'X')
            line[bracket + 1] = ' ';
        else if (current == ' ')
            line[bracket + 1] = 'x';
        else
            return;

        if (hadNewline)
            line.push_back('\n');

        lock();
        replaceRange(lineStartPtr, lineEndPtr, line);
        unlock();
        onContentModified();
    }

    void MarkdownFileEditor::increaseIndent()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        for (auto &line : lines)
            line.insert(line.begin(), {' ', ' '});
        applyBlockSelection(block, lines, block.trailingNewline);
    }

    void MarkdownFileEditor::decreaseIndent()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> lines = block.lines;
        for (auto &line : lines)
        {
            if (!line.empty() && line[0] == '\t')
                line.erase(line.begin());
            else if (line.size() >= 2 && line[0] == ' ' && line[1] == ' ')
                line.erase(line.begin(), line.begin() + 2);
            else if (!line.empty() && line[0] == ' ')
                line.erase(line.begin());
        }
        applyBlockSelection(block, lines, block.trailingNewline);
    }

    void MarkdownFileEditor::convertToDefinitionList()
    {
        BlockSelection block = captureSelectedLines();
        std::vector<std::string> result;
        result.reserve(block.lines.size() * 2);
        for (const auto &line : block.lines)
        {
            std::string trimmedLine = trim(std::string_view(line));
            if (trimmedLine.empty())
            {
                result.push_back("");
                continue;
            }
            std::size_t colon = trimmedLine.find(':');
            if (colon == std::string::npos)
            {
                result.push_back(trimmedLine);
                continue;
            }
            std::size_t indentLen = 0;
            while (indentLen < line.size() && (line[indentLen] == ' ' || line[indentLen] == '\t'))
                ++indentLen;
            std::string indent = line.substr(0, indentLen);
            std::string term = trim(std::string_view(trimmedLine).substr(0, colon));
            std::string definition = trim(std::string_view(trimmedLine).substr(colon + 1));
            result.push_back(indent + term);
            result.push_back(indent + ": " + definition);
        }
        applyBlockSelection(block, result, block.trailingNewline);
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
                out << '\n';
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
        out << "[" << label << "](" << url << ")";
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
        out << "![" << alt << "](" << url << ")";
        insertRichInline("", "", out.str());
    }

    void MarkdownFileEditor::insertReferenceLink()
    {
        std::string selectionText;
        if (hasSelection())
            selectionText = readRange(std::min(selStart, selEnd), std::max(selStart, selEnd));
        if (selectionText.empty())
        {
            selectionText = promptForText("Reference Link", "Link text", "");
            if (selectionText.empty())
                return;
        }

        std::string url = promptForText("Reference Link", "Target URL", "https://");
        if (url.empty())
            return;

        std::string defaultId = generateUniqueReferenceId("ref");
        std::string referenceId = promptForText("Reference Link", "Reference ID", defaultId);
        if (referenceId.empty())
            return;

        std::string title = promptForText("Reference Link", "Title (optional)", "");

        std::ostringstream link;
        link << "[" << selectionText << "][" << referenceId << "]";

        lock();
        if (hasSelection())
            deleteSelect();
        insertText(link.str().c_str(), link.str().size(), False);
        unlock();
        onContentModified();

        std::ostringstream def;
        def << "[" << referenceId << "]: " << url;
        if (!title.empty())
            def << " \"" << title << "\"";
        def << '\n';
        appendDefinition(def.str());
    }

    void MarkdownFileEditor::autoLinkSelection()
    {
        if (!ensureSelection())
            return;
        uint start = std::min(selStart, selEnd);
        uint end = std::max(selStart, selEnd);
        std::string text = readRange(start, end);
        auto isUrl = [&](const std::string &value)
        {
            return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0 || value.rfind("ftp://", 0) == 0;
        };
        auto isEmail = [&](const std::string &value)
        {
            auto at = value.find('@');
            return at != std::string::npos && value.find('.', at) != std::string::npos;
        };

        if (text.size() >= 2 && text.front() == '<' && text.back() == '>')
        {
            std::string inner = text.substr(1, text.size() - 2);
            if (isUrl(inner) || isEmail(inner))
            {
                lock();
                replaceRange(start, end, inner);
                unlock();
                setSelect(start, start + inner.size(), True);
                onContentModified();
            }
            return;
        }

        if (!isUrl(text) && !isEmail(text))
            return;

        std::string wrapped = '<' + text + '>';
        lock();
        replaceRange(start, end, wrapped);
        unlock();
        setSelect(start, start + wrapped.size(), True);
        onContentModified();
    }

    void MarkdownFileEditor::insertFootnote()
    {
        std::string note = promptForText("Footnote", "Footnote text", "");
        if (note.empty())
            return;

        std::string id = generateUniqueFootnoteId();
        std::string marker = "[^" + id + "]";

        lock();
        if (hasSelection())
            deleteSelect();
        insertText(marker.c_str(), marker.size(), False);
        unlock();
        onContentModified();

        std::ostringstream definition;
        definition << "[^" << id << "]: " << note << '\n';
        appendDefinition(definition.str());
    }

    void MarkdownFileEditor::insertHorizontalRule()
    {
        std::string insertion;
        if (curPtr > 0 && bufChar(curPtr - 1) != '\n')
            insertion.push_back('\n');
        insertion.append("---\n");
        if (curPtr >= bufLen || bufChar(curPtr) != '\n')
            insertion.push_back('\n');

        lock();
        insertText(insertion.c_str(), insertion.size(), False);
        unlock();
        onContentModified();
    }

    void MarkdownFileEditor::escapeSelection()
    {
        if (!ensureSelection())
            return;
        uint start = std::min(selStart, selEnd);
        uint end = std::max(selStart, selEnd);
        std::string text = readRange(start, end);
        std::string escaped;
        escaped.reserve(text.size() * 2);
        const std::string specials = "\\`*_{}[]()#+-.!";
        for (char ch : text)
        {
            if (ch == '\\' || specials.find(ch) != std::string::npos)
                escaped.push_back('\\');
            escaped.push_back(ch);
        }

        lock();
        replaceRange(start, end, escaped);
        unlock();
        setSelect(start, start + escaped.size(), True);
        onContentModified();
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
            table << '\n'
                  << makeTableRow(rowCells);
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

        auto collectCells = [&](const MarkdownLineInfo &info)
        {
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
            out << '\n'
                << makeTableRow(row);

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

        auto collectCells = [&](const MarkdownLineInfo &info)
        {
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
            out << '\n'
                << makeTableRow(row);

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

    void MarkdownFileEditor::reflowParagraphs()
    {
        if (!hasSelection())
            return;
        uint start = std::min(selStart, selEnd);
        uint end = std::max(selStart, selEnd);
        std::string text = readRange(start, end);
        if (text.empty())
            return;

        std::vector<std::string> paragraphs;
        std::vector<std::string> separators;
        std::size_t pos = 0;
        while (pos < text.size())
        {
            std::size_t next = text.find("\n\n", pos);
            if (next == std::string::npos)
            {
                paragraphs.push_back(text.substr(pos));
                separators.emplace_back();
                break;
            }
            paragraphs.push_back(text.substr(pos, next - pos));
            std::size_t sepEnd = next;
            while (sepEnd < text.size() && text[sepEnd] == '\n')
                ++sepEnd;
            separators.push_back(text.substr(next, sepEnd - next));
            pos = sepEnd;
        }
        if (paragraphs.empty())
        {
            paragraphs.push_back(text);
            separators.emplace_back();
        }

        auto reflowParagraph = [](const std::string &paragraph)
        {
            std::istringstream stream(paragraph);
            std::string word;
            std::string output;
            int lineLength = 0;
            while (stream >> word)
            {
                if (lineLength == 0)
                {
                    output += word;
                    lineLength = static_cast<int>(word.size());
                }
                else if (lineLength + 1 + static_cast<int>(word.size()) > 80)
                {
                    output.push_back('\n');
                    output += word;
                    lineLength = static_cast<int>(word.size());
                }
                else
                {
                    output.push_back(' ');
                    output += word;
                    lineLength += 1 + static_cast<int>(word.size());
                }
            }
            return output;
        };

        std::string result;
        for (std::size_t i = 0; i < paragraphs.size(); ++i)
        {
            std::string reflowed = reflowParagraph(paragraphs[i]);
            if (!result.empty() && result.back() != '\n' && !reflowed.empty())
                result.push_back('\n');
            result += reflowed;
            result += separators[i];
        }

        lock();
        replaceRange(start, end, result);
        unlock();
        setSelect(start, start + result.size(), True);
        onContentModified();
    }

    void MarkdownFileEditor::formatDocument()
    {
        std::string text = readRange(0, bufLen);
        std::istringstream input(text);
        std::ostringstream output;
        std::string line;
        bool previousBlank = false;

        while (std::getline(input, line))
        {
            std::size_t endPos = line.size();
            std::size_t trailingSpaces = 0;
            while (endPos > 0 && (line[endPos - 1] == ' ' || line[endPos - 1] == '\t'))
            {
                ++trailingSpaces;
                --endPos;
            }
            std::string trimmed = line.substr(0, endPos);
            if (trailingSpaces >= 2)
                trimmed.append("  ");

            bool isBlank = trimLeft(trimmed).empty();
            if (isBlank)
            {
                if (!previousBlank)
                {
                    output << '\n';
                    previousBlank = true;
                }
                continue;
            }

            if (previousBlank && output.tellp() > 0 && output.str().back() != '\n')
                output << '\n';
            previousBlank = false;
            output << trimmed << '\n';
        }

        std::string formatted = output.str();
        if (!formatted.empty() && formatted.back() != '\n')
            formatted.push_back('\n');

        lock();
        replaceRange(0, bufLen, formatted);
        unlock();
        onContentModified();
    }

    void MarkdownFileEditor::toggleSmartListContinuation()
    {
        smartListContinuation = !smartListContinuation;
        if (auto *app = dynamic_cast<MarkdownEditorApp *>(TProgram::application))
            app->refreshUiMode();
    }

    bool MarkdownFileEditor::continueListOnEnter(TEvent &event)
    {
        if (!smartListContinuation)
            return false;
        if (hasSelection())
            return false;
        if (event.what != evKeyDown || event.keyDown.keyCode != kbEnter)
            return false;

        uint lineStartPtr = lineStart(curPtr);
        uint lineEndPtr = lineEnd(lineStartPtr);
        std::string line = readRange(lineStartPtr, lineEndPtr);
        bool hadNewline = false;
        if (!line.empty() && line.back() == '\n')
        {
            hadNewline = true;
            line.pop_back();
        }

        LinePattern pattern = analyzeLinePattern(line);
        if (pattern.hasBullet && pattern.markerStart < line.size())
        {
            std::string_view markerAndRest(line.c_str() + pattern.markerStart, line.size() - pattern.markerStart);
            if (markerAndRest.size() > 1)
            {
                char nextChar = markerAndRest[1];
                if (nextChar != ' ' && nextChar != '\t' && nextChar != '[')
                    pattern.hasBullet = false;
            }
        }
        if (!(pattern.hasBullet || pattern.hasOrdered || pattern.hasTask))
            return false;

        std::size_t contentStart = pattern.markerEnd;
        std::string content = line.substr(contentStart);
        bool emptyItem = trimLeft(content).empty() && curPtr >= lineStartPtr + contentStart;

        if (emptyItem)
        {
            lock();
            replaceRange(lineStartPtr + pattern.indent.size() + pattern.blockquote.size(),
                         lineStartPtr + pattern.markerEnd, "");
            unlock();
            onContentModified();
            return false;
        }

        std::string marker;
        if (pattern.hasTask)
            marker = "- [ ] ";
        else if (pattern.hasBullet)
            marker = std::string(1, pattern.bulletChar) + " ";
        else
            marker = "1. ";

        std::string prefix = pattern.indent + pattern.blockquote + marker;

        TFileEditor::handleEvent(event);
        event.what = evNothing;
        insertText(prefix.c_str(), prefix.size(), False);
        onContentModified();
        return true;
    }

    void MarkdownFileEditor::insertTableRow(TableContext &context, bool below)
    {
        int columns = context.columnCount();
        if (columns <= 0)
        {
            messageBox("Unable to determine the current table layout.", mfError | mfOKButton);
            return;
        }

        auto collectCells = [&](const MarkdownLineInfo &info)
        {
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
            out << '\n'
                << makeTableRow(row);

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

        auto collectCells = [&](const MarkdownLineInfo &info)
        {
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
            out << '\n'
                << makeTableRow(row);

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

        auto collectCells = [&](const MarkdownLineInfo &info)
        {
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
            out << '\n'
                << makeTableRow(row);

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

    void MarkdownFileEditor::queueInfoLine(int lineNumber)
    {
        if (infoViewNeedsFullRefresh || !markdownMode || lineNumber < 0)
            return;

        enqueuePendingInfoLine(lineNumber);

        if (bufLen == 0)
            return;

        uint linePtr = pointerForLine(lineNumber);
        if (linePtr >= bufLen)
            return;

        std::string prefix = readRange(0, linePtr);
        MarkdownParserState state = analyzer().computeStateBefore(prefix);
        std::string text = lineText(linePtr);
        MarkdownLineInfo info = analyzer().analyzeLine(text, state);

        if (!info.fenceOpens)
            return;

        MarkdownParserState cascadeState = state;
        uint currentPtr = nextLine(linePtr);
        int currentLine = lineNumber + 1;
        constexpr int kMaxFencePropagation = 4096;
        int processed = 0;
        while (cascadeState.inFence && currentLine <= lineNumber + kMaxFencePropagation)
        {
            if (currentPtr >= bufLen)
                break;

            enqueuePendingInfoLine(currentLine);

            std::string currentText = lineText(currentPtr);
            MarkdownLineInfo currentInfo = analyzer().analyzeLine(currentText, cascadeState);

            uint nextPtr = nextLine(currentPtr);
            if (nextPtr <= currentPtr)
                break;

            currentPtr = nextPtr;
            ++currentLine;
            ++processed;
            if (processed >= kMaxFencePropagation)
                break;
        }
    }

    void MarkdownFileEditor::queueInfoLineRange(int firstLine, int lastLine)
    {
        if (infoViewNeedsFullRefresh || !markdownMode)
            return;
        if (lastLine < firstLine)
        {
            int temp = firstLine;
            firstLine = lastLine;
            lastLine = temp;
        }
        constexpr int kMaxIncrementalRange = 256;
        if (lastLine - firstLine >= kMaxIncrementalRange)
        {
            requestInfoViewFullRefresh();
            return;
        }
        for (int line = firstLine; line <= lastLine; ++line)
            queueInfoLine(line);
    }

    void MarkdownFileEditor::requestInfoViewFullRefresh()
    {
        infoViewNeedsFullRefresh = true;
        pendingInfoLines.clear();
    }

    void MarkdownFileEditor::clearInfoViewQueue()
    {
        pendingInfoLines.clear();
        infoViewNeedsFullRefresh = false;
    }

    void MarkdownFileEditor::resetLineNumberCache()
    {
        lineNumberCachePtr = lineStart(curPtr);
        lineNumberCacheNumber = cursorLineNumber;
        lineNumberCacheValid = true;
    }

    int MarkdownFileEditor::lineNumberForPointer(uint pointer)
    {
        if (bufLen == 0)
            return 0;

        if (pointer >= bufLen)
        {
            if (bufLen == 0)
                return 0;
            int lastLine = lineNumberForPointer(bufLen - 1);
            return bufChar(bufLen - 1) == '\n' ? lastLine + 1 : lastLine;
        }

        uint target = lineStart(pointer);

        if (!lineNumberCacheValid)
        {
            lineNumberCacheNumber = computeLineNumberForPointer(curPtr);
            lineNumberCachePtr = lineStart(curPtr);
            cursorLineNumber = lineNumberCacheNumber;
            lineNumberCacheValid = true;
        }

        uint currentPtr = lineNumberCachePtr;
        int currentNumber = lineNumberCacheNumber;

        if (target == currentPtr)
            return currentNumber;

        if (target > currentPtr)
        {
            while (currentPtr < target)
            {
                uint next = nextLine(currentPtr);
                if (next <= currentPtr)
                {
                    currentPtr = target;
                    break;
                }
                ++currentNumber;
                currentPtr = next;
            }
        }
        else
        {
            while (currentPtr > target)
            {
                uint prev = lineMove(currentPtr, -1);
                if (prev >= currentPtr)
                {
                    currentPtr = target;
                    break;
                }
                --currentNumber;
                currentPtr = prev;
            }
        }

        lineNumberCachePtr = currentPtr;
        lineNumberCacheNumber = currentNumber;
        if (pointer == curPtr)
            cursorLineNumber = currentNumber;
        return currentNumber;
    }

    uint MarkdownFileEditor::pointerForLine(int lineNumber)
    {
        if (lineNumber <= 0)
            return 0;
        if (bufLen == 0)
            return 0;
        uint ptr = 0;
        for (int i = 0; i < lineNumber && ptr < bufLen; ++i)
        {
            uint next = nextLine(ptr);
            if (next <= ptr)
                return bufLen;
            ptr = next;
        }
        return ptr;
    }

    void MarkdownFileEditor::enqueuePendingInfoLine(int lineNumber)
    {
        if (infoViewNeedsFullRefresh || !markdownMode || lineNumber < 0)
            return;
        if (std::find(pendingInfoLines.begin(), pendingInfoLines.end(), lineNumber) == pendingInfoLines.end())
            pendingInfoLines.push_back(lineNumber);
    }

    void MarkdownFileEditor::handleEvent(TEvent &event)
    {
        if (continueListOnEnter(event))
            return;

        if (handleWrapKeyEvent(event))
            return;

        if (event.what == evCommand)
        {
            switch (event.message.command)
            {
            case cmSave:
                if (hostWindow)
                    hostWindow->saveDocument(false);
                else
                    save();
                clearEvent(event);
                return;
            case cmSaveAs:
                if (hostWindow)
                    hostWindow->saveDocument(true);
                else
                    saveAs();
                clearEvent(event);
                return;
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
            case cmMakeParagraph:
                makeParagraph();
                clearEvent(event);
                return;
            case cmInsertLineBreak:
                insertLineBreak();
                clearEvent(event);
                return;
            case cmLineUp:
                if (wrapEnabled)
                {
                    Boolean centerCursor = Boolean(!cursorVisible());
                    lock();
                    moveCaretVertically(-1, 0);
                    trackCursor(centerCursor);
                    updateWrapStateAfterMovement(true);
                    unlock();
                    clearEvent(event);
                    return;
                }
                break;
            case cmLineDown:
                if (wrapEnabled)
                {
                    Boolean centerCursor = Boolean(!cursorVisible());
                    lock();
                    moveCaretVertically(1, 0);
                    trackCursor(centerCursor);
                    updateWrapStateAfterMovement(true);
                    unlock();
                    clearEvent(event);
                    return;
                }
                break;
            case cmPageUp:
                if (wrapEnabled)
                {
                    Boolean centerCursor = Boolean(!cursorVisible());
                    lock();
                    moveCaretVertically(-(size.y - 1), 0);
                    trackCursor(centerCursor);
                    updateWrapStateAfterMovement(true);
                    unlock();
                    clearEvent(event);
                    return;
                }
                break;
            case cmPageDown:
                if (wrapEnabled)
                {
                    Boolean centerCursor = Boolean(!cursorVisible());
                    lock();
                    moveCaretVertically(size.y - 1, 0);
                    trackCursor(centerCursor);
                    updateWrapStateAfterMovement(true);
                    unlock();
                    clearEvent(event);
                    return;
                }
                break;
            case cmFind:
                find();
                clearEvent(event);
                return;
            case cmReplace:
                replace();
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
            case cmStrikethrough:
                applyStrikethrough();
                clearEvent(event);
                return;
            case cmInlineCode:
                applyInlineCode();
                clearEvent(event);
                return;
            case cmCodeBlock:
                toggleCodeBlock();
                clearEvent(event);
                return;
            case cmRemoveFormatting:
                removeFormatting();
                clearEvent(event);
                return;
            case cmToggleBlockQuote:
                toggleBlockQuote();
                clearEvent(event);
                return;
            case cmToggleBulletList:
                toggleBulletList();
                clearEvent(event);
                return;
            case cmToggleNumberedList:
                toggleNumberedList();
                clearEvent(event);
                return;
            case cmConvertTaskList:
                convertToTaskList();
                clearEvent(event);
                return;
            case cmToggleTaskCheckbox:
                toggleTaskCheckbox();
                clearEvent(event);
                return;
            case cmIncreaseIndent:
                increaseIndent();
                clearEvent(event);
                return;
            case cmDecreaseIndent:
                decreaseIndent();
                clearEvent(event);
                return;
            case cmDefinitionList:
                convertToDefinitionList();
                clearEvent(event);
                return;
            case cmInsertLink:
                insertLink();
                clearEvent(event);
                return;
            case cmInsertReferenceLink:
                insertReferenceLink();
                clearEvent(event);
                return;
            case cmAutoLinkSelection:
                autoLinkSelection();
                clearEvent(event);
                return;
            case cmInsertImage:
                insertImage();
                clearEvent(event);
                return;
            case cmInsertFootnote:
                insertFootnote();
                clearEvent(event);
                return;
            case cmInsertHorizontalRule:
                insertHorizontalRule();
                clearEvent(event);
                return;
            case cmEscapeSelection:
                escapeSelection();
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
            case cmReflowParagraphs:
                reflowParagraphs();
                clearEvent(event);
                return;
            case cmFormatDocument:
                formatDocument();
                clearEvent(event);
                return;
            case cmToggleSmartList:
                toggleSmartListContinuation();
                clearEvent(event);
                return;
            default:
                break;
            }
        }

        refreshCursorMetrics();
        int prevLineNumber = cursorLineNumber;
        TPoint prevPos = curPos;
        TPoint prevDelta = delta;
        uint prevInsCount = insCount;
        uint prevDelCount = delCount;
        Boolean prevModified = modified;
        TFileEditor::handleEvent(event);
        refreshCursorMetrics();
        int currentLineNumber = cursorLineNumber;
        updateWrapStateAfterMovement(false);
        bool contentChanged = (insCount != prevInsCount) || (delCount != prevDelCount) || (modified != prevModified);

        if (contentChanged)
        {
            if (prevLineNumber >= 0)
                queueInfoLine(prevLineNumber);
            queueInfoLine(currentLineNumber);
        }
        else if (prevPos.x != curPos.x)
        {
            queueInfoLine(currentLineNumber);
        }

        if (prevLineNumber != currentLineNumber)
        {
            if (prevLineNumber >= 0)
                queueInfoLine(prevLineNumber);
            queueInfoLine(currentLineNumber);
        }

        if (prevDelta != delta)
            requestInfoViewFullRefresh();

        bool handledContentUpdate = false;
        if (contentChanged)
        {
            onContentModified();
            handledContentUpdate = true;
        }

        if (!handledContentUpdate && (prevPos != curPos || prevDelta != delta || event.what == evCommand))
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
        int wrapWidth = std::max(1, size.x);
        std::vector<TScreenCell> segmentBuffer(static_cast<std::size_t>(size.x));
        int skipSegments = wrapTopSegmentOffset;

        {
            WrapLayout caretLayout;
            computeWrapLayout(lineStart(curPtr), caretLayout);
            int caretSegment = wrapSegmentForColumn(caretLayout, cursorColumnNumber);
            updateWrapCursorVisualPosition(caretLayout, caretSegment);
        }
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
            int lineColumns = charPos(linePtr, endPtr);
            int bufferWidth = std::max(lineColumns + 1, wrapWidth);
            std::vector<TScreenCell> cells(static_cast<std::size_t>(bufferWidth));
            formatLine(cells.data(), linePtr, bufferWidth, color);

            WrapLayout layout;
            layout.lineColumns = lineColumns;
            computeWrapLayoutFromCells(cells.data(), lineColumns, wrapWidth, layout);

            if (layout.segments.empty())
                layout.segments.push_back({0, 0});

            int segmentCount = wrapSegmentCount(layout);
            if (skipSegments >= segmentCount)
            {
                skipSegments -= segmentCount;
                linePtr = nextLine(linePtr);
                continue;
            }

            int startSegment = skipSegments;
            skipSegments = 0;

            for (int seg = startSegment; seg < segmentCount && row < size.y; ++seg)
            {
                const auto &segment = layout.segments[static_cast<std::size_t>(seg)];
                int startCol = std::clamp(segment.startColumn, 0, lineColumns);
                int endCol = std::clamp(segment.endColumn, startCol, lineColumns);
                int copyLen = std::min(size.x, std::max(0, endCol - startCol));
                for (int i = 0; i < copyLen; ++i)
                    segmentBuffer[static_cast<std::size_t>(i)] = cells[static_cast<std::size_t>(startCol + i)];
                for (int i = copyLen; i < size.x; ++i)
                {
                    ::setChar(segmentBuffer[static_cast<std::size_t>(i)], ' ');
                    ::setAttr(segmentBuffer[static_cast<std::size_t>(i)], color);
                }
                writeBuf(0, row, size.x, 1, segmentBuffer.data());
                ++row;
            }
            linePtr = nextLine(linePtr);
        }
        setCursor(wrapCursorScreenPos.x, wrapCursorScreenPos.y);
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

    int MarkdownFileEditor::documentLineNumber() const noexcept
    {
        return cursorLineNumber;
    }

    int MarkdownFileEditor::documentColumnNumber() const noexcept
    {
        return cursorColumnNumber;
    }

    int MarkdownFileEditor::computeLineNumberForPointer(uint pointer)
    {
        if (bufLen == 0)
            return 0;

        if (pointer >= bufLen)
        {
            if (bufLen == 0)
                return 0;
            uint lastPtr = bufLen - 1;
            int lastLine = computeLineNumberForPointer(lastPtr);
            return bufChar(lastPtr) == '\n' ? lastLine + 1 : lastLine;
        }

        uint target = lineStart(pointer);
        uint current = 0;
        int lineNumber = 0;
        while (current < target)
        {
            uint next = nextLine(current);
            if (next <= current)
            {
                current = target;
                break;
            }
            ++lineNumber;
            current = next;
        }
        return lineNumber;
    }

    void MarkdownFileEditor::refreshCursorMetrics()
    {
        if (bufLen == 0)
        {
            cursorLineNumber = 0;
            cursorColumnNumber = 0;
            lineNumberCachePtr = 0;
            lineNumberCacheNumber = 0;
            lineNumberCacheValid = true;
            return;
        }

        cursorLineNumber = lineNumberForPointer(curPtr);
        uint linePtr = lineStart(curPtr);
        cursorColumnNumber = charPos(linePtr, curPtr);
        lineNumberCachePtr = linePtr;
        lineNumberCacheNumber = cursorLineNumber;
        lineNumberCacheValid = true;
        if (indicator)
            indicator->setValue(TPoint(cursorColumnNumber, cursorLineNumber), modified);
    }

    void MarkdownFileEditor::replaceRange(uint start, uint end, const std::string &text)
    {
        queueInfoLineRange(lineNumberForPointer(start), lineNumberForPointer(end));
        deleteRange(start, end, False);
        setCurPtr(start, 0);
        insertText(text.c_str(), text.size(), False);
    }

    std::string MarkdownFileEditor::lineText(uint linePtr)
    {
        return readRange(linePtr, lineEnd(linePtr));
    }

    void MarkdownFileEditor::buildWordWrapSegments(const TScreenCell *cells,
                                                   int lineColumns,
                                                   int wrapWidth,
                                                   std::vector<WrapSegment> &segments)
    {
        segments.clear();
        if (lineColumns <= 0)
        {
            segments.push_back({0, 0});
            return;
        }

        wrapWidth = std::max(1, wrapWidth);

        int offset = 0;
        while (offset < lineColumns)
        {
            int limit = std::min(offset + wrapWidth, lineColumns);

            int lastSpaceStart = -1;
            int lastSpaceEnd = -1;
            int currentSpaceStart = -1;
            int lastHyphenBreak = -1;

            for (int i = offset; i < limit; ++i)
            {
                const TScreenCell &cell = cells[i];
                if (cell._ch.isWideCharTrail())
                    continue;

                if (cellIsWhitespace(cell))
                {
                    if (currentSpaceStart == -1)
                        currentSpaceStart = i;
                    lastSpaceStart = currentSpaceStart;
                    lastSpaceEnd = i + 1;
                }
                else
                {
                    currentSpaceStart = -1;
                }

                if (cellBreaksAfter(cell))
                    lastHyphenBreak = i + 1;
            }

            if (currentSpaceStart != -1)
            {
                lastSpaceStart = currentSpaceStart;
                lastSpaceEnd = limit;
            }

            if (limit < lineColumns)
            {
                const TScreenCell &overflowCell = cells[limit];
                if (!overflowCell._ch.isWideCharTrail())
                {
                    if (cellIsWhitespace(overflowCell))
                    {
                        if (lastSpaceStart < offset)
                            lastSpaceStart = limit;
                        int j = limit;
                        while (j < lineColumns && cellIsWhitespace(cells[j]))
                            ++j;
                        lastSpaceEnd = j;
                    }
                    else if (cellBreaksAfter(overflowCell))
                    {
                        lastHyphenBreak = std::min(limit + 1, lineColumns);
                    }
                }
            }

            int segmentEnd = limit;
            int nextOffset = limit;

            if (limit < lineColumns)
            {
                if (lastSpaceStart > offset)
                {
                    segmentEnd = lastSpaceStart;
                    nextOffset = std::max(lastSpaceEnd, segmentEnd);
                }
                else if (lastHyphenBreak > offset)
                {
                    segmentEnd = lastHyphenBreak;
                    nextOffset = segmentEnd;
                }
            }

            if (segmentEnd <= offset)
            {
                if (limit > offset)
                {
                    segmentEnd = limit;
                    nextOffset = limit;
                }
                else
                {
                    segmentEnd = offset + 1;
                    nextOffset = segmentEnd;
                }
            }

            segments.push_back({offset, segmentEnd});

            offset = nextOffset;
            while (offset < lineColumns && cells[offset]._ch.isWideCharTrail())
                ++offset;
        }
    }

    void MarkdownFileEditor::computeWrapLayoutFromCells(const TScreenCell *cells,
                                                        int lineColumns,
                                                        int wrapWidth,
                                                        WrapLayout &layout)
    {
        layout.segments.clear();
        layout.lineColumns = std::max(0, lineColumns);

        if (!wrapEnabled || wrapWidth <= 0)
        {
            layout.segments.push_back({0, layout.lineColumns});
            return;
        }

        buildWordWrapSegments(cells, layout.lineColumns, wrapWidth, layout.segments);
        if (layout.segments.empty())
            layout.segments.push_back({0, layout.lineColumns});
    }

    void MarkdownFileEditor::computeWrapLayout(uint linePtr, WrapLayout &layout)
    {
        layout.segments.clear();
        layout.lineColumns = 0;

        if (linePtr >= bufLen)
        {
            layout.segments.push_back({0, 0});
            return;
        }

        uint endPtr = lineEnd(linePtr);
        int lineColumns = charPos(linePtr, endPtr);
        layout.lineColumns = lineColumns;

        if (!wrapEnabled)
        {
            layout.segments.push_back({0, lineColumns});
            return;
        }

        int wrapWidth = std::max(1, size.x);
        int bufferWidth = std::max(lineColumns + 1, wrapWidth);
        std::vector<TScreenCell> cells(static_cast<std::size_t>(bufferWidth));
        TAttrPair color = getColor(0x0201);
        formatLine(cells.data(), linePtr, bufferWidth, color);
        computeWrapLayoutFromCells(cells.data(), lineColumns, wrapWidth, layout);
    }

    int MarkdownFileEditor::wrapSegmentForColumn(const WrapLayout &layout, int column) const
    {
        if (layout.segments.empty())
            return 0;
        if (column <= layout.segments.front().startColumn)
            return 0;
        for (std::size_t i = 0; i < layout.segments.size(); ++i)
        {
            const auto &segment = layout.segments[i];
            if (column < segment.endColumn || segment.endColumn <= segment.startColumn)
                return static_cast<int>(i);
        }
        return static_cast<int>(layout.segments.size() - 1);
    }

    int MarkdownFileEditor::documentLineCount() const
    {
        if (bufLen == 0)
            return 1;
        auto *self = const_cast<MarkdownFileEditor *>(this);
        int lastLine = self->lineNumberForPointer(bufLen - 1);
        bool hasTrailingNewline = self->bufChar(bufLen - 1) == '\n';
        return lastLine + 1 + (hasTrailingNewline ? 1 : 0);
    }

    int MarkdownFileEditor::wrapSegmentCount(const WrapLayout &layout) const
    {
        return std::max<int>(1, static_cast<int>(layout.segments.size()));
    }

    MarkdownFileEditor::WrapSegment MarkdownFileEditor::segmentAt(const WrapLayout &layout, int index) const
    {
        if (layout.segments.empty())
            return {0, layout.lineColumns};
        index = std::clamp(index, 0, static_cast<int>(layout.segments.size()) - 1);
        return layout.segments[static_cast<std::size_t>(index)];
    }

    void MarkdownFileEditor::normalizeWrapTop(int &docLine, int &segmentOffset)
    {
        if (!wrapEnabled)
        {
            docLine = std::clamp(docLine, 0, documentLineCount() - 1);
            segmentOffset = 0;
            return;
        }

        int totalLines = std::max(1, documentLineCount());
        docLine = std::clamp(docLine, 0, totalLines - 1);

        while (true)
        {
            uint linePtr = pointerForLine(docLine);
            WrapLayout layout;
            computeWrapLayout(linePtr, layout);
            int segmentCount = wrapSegmentCount(layout);

            if (segmentOffset < 0)
            {
                if (docLine == 0)
                {
                    segmentOffset = 0;
                    break;
                }
                --docLine;
                uint prevPtr = pointerForLine(docLine);
                WrapLayout prevLayout;
                computeWrapLayout(prevPtr, prevLayout);
                segmentOffset += wrapSegmentCount(prevLayout);
                continue;
            }

            if (segmentOffset >= segmentCount)
            {
                segmentOffset -= segmentCount;
                if (docLine >= totalLines - 1)
                {
                    segmentOffset = std::max(0, segmentCount - 1);
                    break;
                }
                ++docLine;
                continue;
            }

            break;
        }
    }

    int MarkdownFileEditor::computeWrapCaretRow(int docLine,
                                               int segmentOffset,
                                               uint caretLinePtr,
                                               const WrapLayout &caretLayout,
                                               int caretSegment) const
    {
        auto *self = const_cast<MarkdownFileEditor *>(this);
        int row = -segmentOffset;
        int lineNumber = docLine;
        int caretLineNumber = cursorLineNumber;
        uint linePtr = self->pointerForLine(docLine);

        if (caretLineNumber >= lineNumber)
        {
            while (lineNumber < caretLineNumber)
            {
                WrapLayout layout;
                self->computeWrapLayout(linePtr, layout);
                row += wrapSegmentCount(layout);
                linePtr = self->nextLine(linePtr);
                ++lineNumber;
            }
            row += caretSegment;
        }
        else
        {
            while (lineNumber > caretLineNumber)
            {
                --lineNumber;
                linePtr = self->pointerForLine(lineNumber);
                WrapLayout layout;
                self->computeWrapLayout(linePtr, layout);
                row -= wrapSegmentCount(layout);
            }
            row += caretSegment;
        }

        return row;
    }

    int MarkdownFileEditor::currentWrapLocalColumn(const WrapLayout &layout, int segmentIndex) const
    {
        if (layout.segments.empty())
            return cursorColumnNumber;
        WrapSegment segment = segmentAt(layout, segmentIndex);
        return std::max(0, cursorColumnNumber - segment.startColumn);
    }

    void MarkdownFileEditor::ensureWrapViewport(const WrapLayout &caretLayout, int caretSegment)
    {
        int docLine = delta.y;
        int segmentOffset = wrapTopSegmentOffset;
        normalizeWrapTop(docLine, segmentOffset);

        uint caretLinePtr = lineStart(curPtr);
        int caretRow = computeWrapCaretRow(docLine, segmentOffset, caretLinePtr, caretLayout, caretSegment);

        int viewHeight = std::max(1, size.y);
        while (caretRow < 0)
        {
            segmentOffset += caretRow;
            normalizeWrapTop(docLine, segmentOffset);
            caretRow = computeWrapCaretRow(docLine, segmentOffset, caretLinePtr, caretLayout, caretSegment);
        }

        while (caretRow >= viewHeight)
        {
            segmentOffset += caretRow - (viewHeight - 1);
            normalizeWrapTop(docLine, segmentOffset);
            caretRow = computeWrapCaretRow(docLine, segmentOffset, caretLinePtr, caretLayout, caretSegment);
        }

        bool docLineChanged = docLine != delta.y;
        bool offsetChanged = segmentOffset != wrapTopSegmentOffset;
        wrapTopSegmentOffset = segmentOffset;
        if (docLineChanged)
            scrollTo(delta.x, docLine);
        else if (offsetChanged)
            update(ufView);
    }

    void MarkdownFileEditor::updateWrapCursorVisualPosition(const WrapLayout &caretLayout, int caretSegment)
    {
        if (!wrapEnabled)
        {
            wrapCursorScreenPos = TPoint(curPos.x - delta.x, curPos.y - delta.y);
            return;
        }

        int docLine = delta.y;
        int segmentOffset = wrapTopSegmentOffset;
        normalizeWrapTop(docLine, segmentOffset);

        uint caretLinePtr = lineStart(curPtr);
        int caretRow = computeWrapCaretRow(docLine, segmentOffset, caretLinePtr, caretLayout, caretSegment);
        caretRow = std::clamp(caretRow, 0, std::max(0, size.y - 1));

        int column = cursorColumnNumber;
        if (!caretLayout.segments.empty())
        {
            WrapSegment segment = segmentAt(caretLayout, caretSegment);
            column = std::max(0, cursorColumnNumber - segment.startColumn);
        }
        column = std::clamp(column, 0, std::max(0, size.x - 1));
        wrapCursorScreenPos = TPoint(column, caretRow);
    }

    void MarkdownFileEditor::updateWrapStateAfterMovement(bool preserveDesiredColumn)
    {
        if (!wrapEnabled)
            return;

        uint caretLinePtr = lineStart(curPtr);
        WrapLayout caretLayout;
        computeWrapLayout(caretLinePtr, caretLayout);
        int caretSegment = wrapSegmentForColumn(caretLayout, cursorColumnNumber);

        if (!preserveDesiredColumn)
            wrapDesiredVisualColumn = currentWrapLocalColumn(caretLayout, caretSegment);

        ensureWrapViewport(caretLayout, caretSegment);
        updateWrapCursorVisualPosition(caretLayout, caretSegment);
    }

    bool MarkdownFileEditor::handleWrapKeyEvent(TEvent &event)
    {
        if (!wrapEnabled || event.what != evKeyDown)
            return false;

        const ushort keyCode = event.keyDown.keyCode;
        int lines = 0;
        if (keyCode == kbUp)
            lines = -1;
        else if (keyCode == kbDown)
            lines = 1;
        else if (keyCode == kbPgUp)
            lines = -(size.y - 1);
        else if (keyCode == kbPgDn)
            lines = size.y - 1;
        else
            return false;

        uchar selectMode = 0;
        if (selecting == True || (event.keyDown.controlKeyState & kbShift) != 0)
            selectMode = smExtend;

        Boolean centerCursor = Boolean(!cursorVisible());

        lock();
        moveCaretVertically(lines, selectMode);
        trackCursor(centerCursor);
        updateWrapStateAfterMovement(true);
        unlock();

        clearEvent(event);
        return true;
    }

    void MarkdownFileEditor::moveCaretVertically(int lines, uchar selectMode)
    {
        if (lines == 0)
            return;

        uint linePtr = lineStart(curPtr);
        WrapLayout layout;
        computeWrapLayout(linePtr, layout);
        int segmentIndex = wrapSegmentForColumn(layout, cursorColumnNumber);
        int desiredColumn = wrapDesiredVisualColumn >= 0 ? wrapDesiredVisualColumn
                                                        : currentWrapLocalColumn(layout, segmentIndex);
        wrapDesiredVisualColumn = desiredColumn;

        int remaining = lines;
        while (remaining != 0)
        {
            int direction = remaining > 0 ? 1 : -1;
            if (!moveCaretOneStep(direction, selectMode, desiredColumn))
                break;
            remaining -= direction;
        }
    }

    bool MarkdownFileEditor::moveCaretOneStep(int direction, uchar selectMode, int desiredColumn)
    {
        uint linePtr = lineStart(curPtr);
        WrapLayout layout;
        computeWrapLayout(linePtr, layout);
        int segmentIndex = wrapSegmentForColumn(layout, cursorColumnNumber);

        if (direction > 0)
        {
            if (moveCaretDownSegment(linePtr, layout, segmentIndex, selectMode, desiredColumn))
                return true;
            return moveCaretToNextDocumentLine(linePtr, selectMode, desiredColumn);
        }

        if (moveCaretUpSegment(linePtr, layout, segmentIndex, selectMode, desiredColumn))
            return true;
        return moveCaretToPreviousDocumentLine(linePtr, selectMode, desiredColumn);
    }

    bool MarkdownFileEditor::moveCaretDownSegment(uint linePtr,
                                                  const WrapLayout &layout,
                                                  int segmentIndex,
                                                  uchar selectMode,
                                                  int desiredColumn)
    {
        int segmentCount = wrapSegmentCount(layout);
        if (segmentIndex + 1 >= segmentCount)
            return false;

        WrapSegment segment = segmentAt(layout, segmentIndex + 1);
        int segmentWidth = std::max(0, segment.endColumn - segment.startColumn);
        int localColumn = std::clamp(desiredColumn, 0, segmentWidth);
        int targetColumn = segment.startColumn + localColumn;
        uint newPtr = charPtr(linePtr, targetColumn);
        setCurPtr(newPtr, selectMode);
        return true;
    }

    bool MarkdownFileEditor::moveCaretToNextDocumentLine(uint linePtr,
                                                        uchar selectMode,
                                                        int desiredColumn)
    {
        uint nextPtr = nextLine(linePtr);
        if (nextPtr == linePtr || nextPtr >= bufLen)
        {
            setCurPtr(bufLen, selectMode);
            return false;
        }

        WrapLayout nextLayout;
        computeWrapLayout(nextPtr, nextLayout);
        int lineColumns = std::max(0, nextLayout.lineColumns);
        int targetColumn = std::clamp(desiredColumn, 0, lineColumns);
        uint newPtr = charPtr(nextPtr, targetColumn);
        setCurPtr(newPtr, selectMode);
        return true;
    }

    bool MarkdownFileEditor::moveCaretUpSegment(uint linePtr,
                                                const WrapLayout &layout,
                                                int segmentIndex,
                                                uchar selectMode,
                                                int desiredColumn)
    {
        if (segmentIndex <= 0)
            return false;

        WrapSegment segment = segmentAt(layout, segmentIndex - 1);
        int segmentWidth = std::max(0, segment.endColumn - segment.startColumn);
        int localColumn = std::clamp(desiredColumn, 0, segmentWidth);
        int targetColumn = segment.startColumn + localColumn;
        uint newPtr = charPtr(linePtr, targetColumn);
        setCurPtr(newPtr, selectMode);
        return true;
    }

    bool MarkdownFileEditor::moveCaretToPreviousDocumentLine(uint linePtr,
                                                             uchar selectMode,
                                                             int desiredColumn)
    {
        uint prevPtr = prevLine(linePtr);
        if (prevPtr == linePtr)
        {
            setCurPtr(0, selectMode);
            return false;
        }

        WrapLayout prevLayout;
        computeWrapLayout(prevPtr, prevLayout);
        int lastSegmentIndex = wrapSegmentCount(prevLayout) - 1;
        WrapSegment segment = segmentAt(prevLayout, lastSegmentIndex);
        int segmentWidth = std::max(0, segment.endColumn - segment.startColumn);
        int localColumn = std::clamp(desiredColumn, 0, segmentWidth);
        int targetColumn = segment.startColumn + localColumn;
        uint newPtr = charPtr(prevPtr, targetColumn);
        setCurPtr(newPtr, selectMode);
        return true;
    }

    void MarkdownFileEditor::notifyInfoView()
    {
        refreshCursorMetrics();
        ++cachedStateVersion;
        statusCachePrefixPtr = UINT_MAX;
        statusCacheVersion = 0;
        resetLineNumberCache();
        if (infoView)
        {
            infoView->invalidateState();
            if (markdownMode && (infoView->state & sfVisible))
            {
                if (infoViewNeedsFullRefresh || pendingInfoLines.empty())
                    infoView->drawView();
                else
                    infoView->updateLines(pendingInfoLines);
            }
        }
        clearInfoViewQueue();
        if (auto *app = dynamic_cast<MarkdownEditorApp *>(TProgram::application))
            app->refreshUiMode();
    }

    void MarkdownFileEditor::buildStatusContext(MarkdownStatusContext &context)
    {
        context = {};
        context.hasEditor = true;
        context.markdownMode = markdownMode;
        context.smartListContinuation = smartListContinuation;
        context.hasFileName = fileName[0] != '\0';
        context.isUntitled = !context.hasFileName;
        context.isModified = modified;

        if (!markdownMode)
            return;

        if (bufLen == 0)
        {
            context.lineKind = MarkdownLineKind::Blank;
            context.hasCursorLine = false;
            context.spanKind = MarkdownSpanKind::PlainText;
            return;
        }

        uint linePtr = lineStart(curPtr);
        if (linePtr >= bufLen)
        {
            context.lineKind = MarkdownLineKind::Blank;
            context.hasCursorLine = false;
            context.spanKind = MarkdownSpanKind::PlainText;
            return;
        }

        context.hasCursorLine = true;

        MarkdownParserState state;
        if (statusCacheVersion == cachedStateVersion && statusCachePrefixPtr != UINT_MAX && statusCachePrefixPtr <= linePtr)
        {
            state = statusStateCache;
            uint ptr = statusCachePrefixPtr;
            while (ptr < linePtr && ptr < bufLen)
            {
                uint end = lineEnd(ptr);
                std::string line = readRange(ptr, end);
                analyzer().analyzeLine(line, state);
                uint next = nextLine(ptr);
                if (next <= ptr)
                    break;
                ptr = next;
            }
        }
        else
        {
            state = MarkdownParserState{};
            uint ptr = 0;
            while (ptr < linePtr && ptr < bufLen)
            {
                uint end = lineEnd(ptr);
                std::string line = readRange(ptr, end);
                analyzer().analyzeLine(line, state);
                uint next = nextLine(ptr);
                if (next <= ptr)
                    break;
                ptr = next;
            }
        }
        statusStateCache = state;
        statusCachePrefixPtr = linePtr;
        statusCacheVersion = cachedStateVersion;

        MarkdownLineInfo info = analyzer().analyzeLine(lineText(linePtr), state);
        context.lineKind = info.kind;
        context.headingLevel = info.headingLevel;
        context.isTaskItem = info.isTask || info.kind == MarkdownLineKind::TaskListItem;
        context.isOrderedItem = info.kind == MarkdownLineKind::OrderedListItem;
        context.isBulletItem = info.kind == MarkdownLineKind::BulletListItem;
        context.isTableHeader = info.isTableHeader;
        context.isTableSeparator = info.kind == MarkdownLineKind::TableSeparator;
        context.isTableRow = info.kind == MarkdownLineKind::TableRow;

        if (context.isTableRow || context.isTableSeparator)
        {
            int columnIndex = -1;
            if (!info.tableCells.empty())
            {
                for (std::size_t i = 0; i < info.tableCells.size(); ++i)
                {
                    const auto &cell = info.tableCells[i];
                    std::size_t endCol = std::max(cell.endColumn, cell.startColumn + 1);
                    if (curPos.x >= static_cast<int>(cell.startColumn) && curPos.x < static_cast<int>(endCol))
                    {
                        columnIndex = static_cast<int>(i);
                        break;
                    }
                }
                if (columnIndex == -1)
                    columnIndex = static_cast<int>(info.tableCells.size()) - 1;
            }
            if (columnIndex < 0 && !info.tableAlignments.empty())
                columnIndex = std::clamp(curPos.x, 0, static_cast<int>(info.tableAlignments.size()) - 1);
            context.tableColumn = columnIndex;
            if (columnIndex >= 0 && columnIndex < static_cast<int>(info.tableAlignments.size()))
            {
                context.tableAlignment = info.tableAlignments[columnIndex];
                context.tableHasAlignment = true;
            }
            else if (context.isTableSeparator && !info.tableAlignments.empty())
            {
                context.tableHasAlignment = true;
                if (columnIndex >= 0 && columnIndex < static_cast<int>(info.tableAlignments.size()))
                    context.tableAlignment = info.tableAlignments[columnIndex];
                else
                    context.tableAlignment = MarkdownTableAlignment::Default;
            }
        }

        const MarkdownSpan *span = analyzer().spanAtColumn(info, curPos.x);
        if (span)
        {
            context.spanKind = span->kind;
            context.hasSpan = span->kind != MarkdownSpanKind::PlainText;
        }
        else
        {
            context.spanKind = MarkdownSpanKind::PlainText;
            context.hasSpan = false;
        }
    }

    void MarkdownFileEditor::onContentModified()
    {
        refreshCursorMetrics();
        queueInfoLine(cursorLineNumber);
        notifyInfoView();
        if (hostWindow)
            hostWindow->updateWindowTitle();
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

} // namespace ck::edit
