#include "ck/edit/markdown_editor.hpp"

#include "ck/about_dialog.hpp"
#include "ck/hotkeys.hpp"
#include "ck/launcher.hpp"
#include "ck/ui/clock_view.hpp"

#define Uses_TFindDialog
#define Uses_TReplaceDialog

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <chrono>
#include <thread>
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
#ifndef CK_EDIT_VERSION
#define CK_EDIT_VERSION "0.0.0"
#endif
        constexpr int kInfoColumnWidth = 20;

        class MarkdownWindowFrame : public TFrame
        {
        public:
            using TFrame::TFrame;

            virtual void draw() override
            {
                TFrame::draw();

                auto *window = dynamic_cast<MarkdownEditWindow *>(owner);
                if (!window)
                    return;

                auto *editor = window->editor();
                if (!editor || !editor->isMarkdownMode())
                    return;

                const int connectorColumn = kInfoColumnWidth;
                if (connectorColumn <= 0 || connectorColumn >= size.x)
                    return;

                const bool active = (state & sfActive) != 0;
                const bool dragging = (state & sfDragging) != 0;
                const bool useDoubleLines = active && !dragging;

                constexpr char kTopDouble = static_cast<char>(0xD1);
                constexpr char kBottomDouble = static_cast<char>(0xCF);
                constexpr char kTopSingle = static_cast<char>(0xC2);
                constexpr char kBottomSingle = static_cast<char>(0xC1);

                const char topChar = useDoubleLines ? kTopDouble : kTopSingle;
                const char bottomChar = useDoubleLines ? kBottomDouble : kBottomSingle;
                const char verticalChar = useDoubleLines ? static_cast<char>(0xBA) : static_cast<char>(0xB3);

                ushort colorIndex;
                if (dragging)
                    colorIndex = 0x0505;
                else if (!active)
                    colorIndex = 0x0101;
                else
                    colorIndex = 0x0503;

                TAttrPair frameColors = getColor(colorIndex);
                TColorAttr frameAttr = frameColors[0];

                TDrawBuffer buffer;
                buffer.moveChar(0, topChar, frameAttr, 1);
                writeLine(connectorColumn, 0, 1, 1, buffer);
                buffer.moveChar(0, bottomChar, frameAttr, 1);
                writeLine(connectorColumn, size.y - 1, 1, 1, buffer);

                if (size.y > 2)
                {
                    TDrawBuffer column;
                    column.moveChar(0, verticalChar, frameAttr, 1);
                    for (int y = 1; y < size.y - 1; ++y)
                        writeLine(connectorColumn, y, 1, 1, column);
                }
            }

            virtual void setState(ushort aState, Boolean enable) override
            {
                bool wasDragging = (state & sfDragging) != 0;
                bool wasActive = (state & sfActive) != 0;
                TFrame::setState(aState, enable);
                bool isDragging = (state & sfDragging) != 0;
                bool isActive = (state & sfActive) != 0;
                if (wasDragging != isDragging || wasActive != isActive)
                {
                    if (auto *window = dynamic_cast<MarkdownEditWindow *>(owner))
                        window->refreshDivider();
                }
            }
        };

        ushort execDialog(TDialog *d, void *data = nullptr);
        ushort runFindDialog(TFindDialogRec &rec);
        ushort runReplaceDialog(TReplaceDialogRec &rec);

        ushort runEditorDialog(int dialog, ...)
        {
            va_list args;
            switch (dialog)
            {
            case edOutOfMemory:
                return messageBox("Not enough memory for this operation.", mfError | mfOKButton);
            case edReadError:
            case edWriteError:
            case edCreateError:
            {
                va_start(args, dialog);
                const char *file = va_arg(args, const char *);
                va_end(args);
                std::ostringstream text;
                switch (dialog)
                {
                case edReadError:
                    text << "Error reading file ";
                    break;
                case edWriteError:
                    text << "Error writing file ";
                    break;
                default:
                    text << "Error creating file ";
                    break;
                }
                if (file && *file)
                    text << file;
                text << '.';
                return messageBox(text.str().c_str(), mfError | mfOKButton);
            }
            case edSaveModify:
            {
                va_start(args, dialog);
                const char *file = va_arg(args, const char *);
                va_end(args);
                std::ostringstream text;
                if (file && *file)
                    text << file << " has been modified. Save?";
                else
                    text << "Document has been modified. Save?";
                return messageBox(text.str().c_str(), mfConfirmation | mfYesNoCancel);
            }
            case edSaveUntitled:
                return messageBox("Save untitled document?", mfConfirmation | mfYesNoCancel);
            case edSaveAs:
            {
                va_start(args, dialog);
                char *file = va_arg(args, char *);
                va_end(args);
                return execDialog(new TFileDialog("*.md", "Save file as", "~N~ame", fdOKButton, 101), file);
            }
            case edFind:
            {
                va_start(args, dialog);
                auto *rec = va_arg(args, TFindDialogRec *);
                va_end(args);
                if (!rec)
                    return cmCancel;
                return runFindDialog(*rec);
            }
            case edReplace:
            {
                va_start(args, dialog);
                auto *rec = va_arg(args, TReplaceDialogRec *);
                va_end(args);
                if (!rec)
                    return cmCancel;
                return runReplaceDialog(*rec);
            }
            case edSearchFailed:
                return messageBox("Search string not found.", mfError | mfOKButton);
            case edReplacePrompt:
            {
                va_start(args, dialog);
                (void)va_arg(args, TPoint *);
                va_end(args);
                return messageBox("Replace this occurrence?", mfYesNoCancel | mfInformation);
            }
            default:
                return cmCancel;
            }
            return cmCancel;
        }

        void delay(unsigned milliseconds)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
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

        constexpr int kFindHistoryId = 10;
        constexpr int kReplaceHistoryId = 11;

        ushort encodeFindOptions(ushort options)
        {
            ushort value = 0;
            if (options & efCaseSensitive)
                value |= 0x0001;
            if (options & efWholeWordsOnly)
                value |= 0x0002;
            return value;
        }

        ushort decodeFindOptions(ushort value)
        {
            ushort options = 0;
            if (value & 0x0001)
                options |= efCaseSensitive;
            if (value & 0x0002)
                options |= efWholeWordsOnly;
            return options;
        }

        ushort encodeReplaceOptions(ushort options)
        {
            ushort value = 0;
            if (options & efCaseSensitive)
                value |= 0x0001;
            if (options & efWholeWordsOnly)
                value |= 0x0002;
            if (options & efPromptOnReplace)
                value |= 0x0004;
            if (options & efReplaceAll)
                value |= 0x0008;
            return value;
        }

        ushort decodeReplaceOptions(ushort value)
        {
            ushort options = 0;
            if (value & 0x0001)
                options |= efCaseSensitive;
            if (value & 0x0002)
                options |= efWholeWordsOnly;
            if (value & 0x0004)
                options |= efPromptOnReplace;
            if (value & 0x0008)
                options |= efReplaceAll;
            return options;
        }

        ushort runFindDialog(TFindDialogRec &rec)
        {
            auto *dialog = new TDialog(TRect(0, 0, 38, 12), "Find");
            dialog->options |= ofCentered;

            auto *findInput = new TInputLine(TRect(3, 3, 32, 4), maxFindStrLen);
            dialog->insert(findInput);
            dialog->insert(new TLabel(TRect(2, 2, 15, 3), "~T~ext to find", findInput));
            dialog->insert(new THistory(TRect(32, 3, 35, 4), findInput, kFindHistoryId));

            auto *optionBoxes = new TCheckBoxes(
                TRect(3, 5, 35, 7),
                new TSItem("~C~ase sensitive",
                           new TSItem("~W~hole words only", nullptr)));
            dialog->insert(optionBoxes);

            dialog->insert(new TButton(TRect(14, 9, 24, 11), "O~K~", cmOK, bfDefault));
            dialog->insert(new TButton(TRect(26, 9, 36, 11), "Cancel", cmCancel, bfNormal));

            findInput->setData(rec.find);
            ushort optionMask = encodeFindOptions(rec.options);
            optionBoxes->setData(&optionMask);

            dialog->selectNext(False);

            TView *validated = TProgram::application->validView(dialog);
            if (!validated)
                return cmCancel;
            dialog = static_cast<TDialog *>(validated);

            ushort result = TProgram::deskTop->execView(dialog);
            if (result != cmCancel)
            {
                findInput->getData(rec.find);
                optionBoxes->getData(&optionMask);
                rec.options = decodeFindOptions(optionMask);
            }
            TObject::destroy(dialog);
            return result;
        }

        ushort runReplaceDialog(TReplaceDialogRec &rec)
        {
            auto *dialog = new TDialog(TRect(0, 0, 40, 16), "Replace");
            dialog->options |= ofCentered;

            auto *findInput = new TInputLine(TRect(3, 3, 34, 4), maxFindStrLen);
            dialog->insert(findInput);
            dialog->insert(new TLabel(TRect(2, 2, 15, 3), "~T~ext to find", findInput));
            dialog->insert(new THistory(TRect(34, 3, 37, 4), findInput, kFindHistoryId));

            auto *replaceInput = new TInputLine(TRect(3, 6, 34, 7), maxReplaceStrLen);
            dialog->insert(replaceInput);
            dialog->insert(new TLabel(TRect(2, 5, 12, 6), "~N~ew text", replaceInput));
            dialog->insert(new THistory(TRect(34, 6, 37, 7), replaceInput, kReplaceHistoryId));

            auto *optionBoxes = new TCheckBoxes(
                TRect(3, 8, 37, 12),
                new TSItem("~C~ase sensitive",
                           new TSItem("~W~hole words only",
                                      new TSItem("~P~rompt on replace",
                                                 new TSItem("~R~eplace all", nullptr)))));
            dialog->insert(optionBoxes);

            dialog->insert(new TButton(TRect(17, 13, 27, 15), "O~K~", cmOK, bfDefault));
            dialog->insert(new TButton(TRect(28, 13, 38, 15), "Cancel", cmCancel, bfNormal));

            findInput->setData(rec.find);
            replaceInput->setData(rec.replace);
            ushort optionMask = encodeReplaceOptions(rec.options);
            optionBoxes->setData(&optionMask);

            dialog->selectNext(False);

            TView *validated = TProgram::application->validView(dialog);
            if (!validated)
                return cmCancel;
            dialog = static_cast<TDialog *>(validated);

            ushort result = TProgram::deskTop->execView(dialog);
            if (result != cmCancel)
            {
                findInput->getData(rec.find);
                replaceInput->getData(rec.replace);
                optionBoxes->getData(&optionMask);
                rec.options = decodeReplaceOptions(optionMask);
            }
            TObject::destroy(dialog);
            return result;
        }

        bool operator==(const MarkdownStatusContext &lhs, const MarkdownStatusContext &rhs) noexcept
        {
            return lhs.hasEditor == rhs.hasEditor && lhs.markdownMode == rhs.markdownMode &&
                   lhs.hasFileName == rhs.hasFileName && lhs.isUntitled == rhs.isUntitled &&
                   lhs.isModified == rhs.isModified && lhs.hasCursorLine == rhs.hasCursorLine &&
                   lhs.lineKind == rhs.lineKind && lhs.headingLevel == rhs.headingLevel &&
                   lhs.isTaskItem == rhs.isTaskItem && lhs.isOrderedItem == rhs.isOrderedItem &&
                   lhs.isBulletItem == rhs.isBulletItem && lhs.isTableRow == rhs.isTableRow &&
                   lhs.isTableHeader == rhs.isTableHeader && lhs.isTableSeparator == rhs.isTableSeparator &&
                   lhs.tableColumn == rhs.tableColumn && lhs.tableAlignment == rhs.tableAlignment &&
                   lhs.tableHasAlignment == rhs.tableHasAlignment && lhs.spanKind == rhs.spanKind &&
                   lhs.hasSpan == rhs.hasSpan;
        }

        bool operator!=(const MarkdownStatusContext &lhs, const MarkdownStatusContext &rhs) noexcept
        {
            return !(lhs == rhs);
        }

        std::vector<ushort> buildBaseCommands(const MarkdownStatusContext &context)
        {
            std::vector<ushort> commands;
            if (!context.hasEditor)
            {
                commands.push_back(cmOpen);
                if (ck::launcher::launchedFromCkLauncher())
                    commands.push_back(cmReturnToLauncher);
                return commands;
            }

            commands.push_back(cmSave);
            if (context.isUntitled)
                commands.push_back(cmSaveAs);
            commands.push_back(cmToggleWrap);
            commands.push_back(cmToggleMarkdownMode);
            if (ck::launcher::launchedFromCkLauncher())
                commands.push_back(cmReturnToLauncher);
            return commands;
        }

        std::vector<ushort> buildContextCommands(const MarkdownStatusContext &context)
        {
            constexpr std::size_t kMaxContextCommands = 12;
            std::vector<ushort> commands;
            if (!context.hasEditor || !context.markdownMode)
                return commands;

            auto addCommand = [&](ushort command)
            {
                if (commands.size() >= kMaxContextCommands)
                    return;
                if (std::find(commands.begin(), commands.end(), command) != commands.end())
                    return;
                commands.push_back(command);
            };

            if (context.isTableRow || context.isTableHeader)
            {
                addCommand(cmTableInsertRowAbove);
                addCommand(cmTableInsertRowBelow);
                addCommand(cmTableDeleteRow);
                addCommand(cmTableInsertColumnBefore);
                addCommand(cmTableInsertColumnAfter);
                addCommand(cmTableDeleteColumn);
                addCommand(cmTableDeleteTable);
                addCommand(cmTableAlignDefault);
                addCommand(cmTableAlignLeft);
                addCommand(cmTableAlignCenter);
                addCommand(cmTableAlignRight);
                addCommand(cmTableAlignNumber);
            }
            else if (context.isTableSeparator)
            {
                addCommand(cmTableAlignDefault);
                addCommand(cmTableAlignLeft);
                addCommand(cmTableAlignCenter);
                addCommand(cmTableAlignRight);
                addCommand(cmTableAlignNumber);
                addCommand(cmTableInsertColumnBefore);
                addCommand(cmTableInsertColumnAfter);
                addCommand(cmTableDeleteColumn);
                addCommand(cmTableDeleteTable);
            }

            switch (context.spanKind)
            {
            case MarkdownSpanKind::Bold:
                addCommand(cmBold);
                addCommand(cmItalic);
                addCommand(cmRemoveFormatting);
                break;
            case MarkdownSpanKind::Italic:
                addCommand(cmItalic);
                addCommand(cmBold);
                addCommand(cmRemoveFormatting);
                break;
            case MarkdownSpanKind::BoldItalic:
                addCommand(cmBoldItalic);
                addCommand(cmBold);
                addCommand(cmItalic);
                addCommand(cmRemoveFormatting);
                break;
            case MarkdownSpanKind::Strikethrough:
                addCommand(cmStrikethrough);
                addCommand(cmRemoveFormatting);
                break;
            case MarkdownSpanKind::Code:
                addCommand(cmInlineCode);
                addCommand(cmRemoveFormatting);
                break;
            case MarkdownSpanKind::Link:
                addCommand(cmInsertLink);
                addCommand(cmInsertReferenceLink);
                addCommand(cmAutoLinkSelection);
                addCommand(cmRemoveFormatting);
                break;
            case MarkdownSpanKind::Image:
                addCommand(cmInsertImage);
                addCommand(cmRemoveFormatting);
                break;
            case MarkdownSpanKind::InlineHtml:
                addCommand(cmEscapeSelection);
                addCommand(cmRemoveFormatting);
                break;
            default:
                break;
            }

            switch (context.lineKind)
            {
            case MarkdownLineKind::Heading:
                addCommand(cmHeading1);
                addCommand(cmHeading2);
                addCommand(cmHeading3);
                addCommand(cmHeading4);
                addCommand(cmHeading5);
                addCommand(cmHeading6);
                addCommand(cmClearHeading);
                addCommand(cmMakeParagraph);
                addCommand(cmInsertLineBreak);
                break;
            case MarkdownLineKind::BlockQuote:
                addCommand(cmToggleBlockQuote);
                addCommand(cmIncreaseIndent);
                addCommand(cmDecreaseIndent);
                addCommand(cmMakeParagraph);
                break;
            case MarkdownLineKind::BulletListItem:
                addCommand(cmToggleBulletList);
                addCommand(cmToggleNumberedList);
                addCommand(cmConvertTaskList);
                addCommand(cmIncreaseIndent);
                addCommand(cmDecreaseIndent);
                addCommand(cmToggleSmartList);
                if (context.isTaskItem)
                    addCommand(cmToggleTaskCheckbox);
                break;
            case MarkdownLineKind::OrderedListItem:
                addCommand(cmToggleNumberedList);
                addCommand(cmToggleBulletList);
                addCommand(cmConvertTaskList);
                addCommand(cmIncreaseIndent);
                addCommand(cmDecreaseIndent);
                addCommand(cmToggleSmartList);
                if (context.isTaskItem)
                    addCommand(cmToggleTaskCheckbox);
                break;
            case MarkdownLineKind::TaskListItem:
                addCommand(cmToggleTaskCheckbox);
                addCommand(cmConvertTaskList);
                addCommand(cmToggleBulletList);
                addCommand(cmToggleNumberedList);
                addCommand(cmIncreaseIndent);
                addCommand(cmDecreaseIndent);
                addCommand(cmToggleSmartList);
                break;
            case MarkdownLineKind::CodeFenceStart:
            case MarkdownLineKind::CodeFenceEnd:
            case MarkdownLineKind::FencedCode:
            case MarkdownLineKind::IndentedCode:
                addCommand(cmCodeBlock);
                addCommand(cmInlineCode);
                addCommand(cmEscapeSelection);
                break;
            case MarkdownLineKind::HorizontalRule:
                addCommand(cmInsertHorizontalRule);
                addCommand(cmMakeParagraph);
                addCommand(cmInsertTable);
                break;
            case MarkdownLineKind::Html:
                addCommand(cmEscapeSelection);
                addCommand(cmInlineCode);
                addCommand(cmInsertLink);
                addCommand(cmInsertImage);
                break;
            case MarkdownLineKind::Paragraph:
            case MarkdownLineKind::Blank:
            case MarkdownLineKind::Unknown:
                addCommand(cmBold);
                addCommand(cmItalic);
                addCommand(cmInlineCode);
                addCommand(cmInsertLink);
                addCommand(cmInsertImage);
                addCommand(cmInsertFootnote);
                addCommand(cmInsertTable);
                addCommand(cmInsertLineBreak);
                addCommand(cmReflowParagraphs);
                addCommand(cmFormatDocument);
                addCommand(cmInsertHorizontalRule);
                break;
            default:
                break;
            }

            return commands;
        }

        class MarkdownStatusLine : public TStatusLine
        {
        public:
            MarkdownStatusLine(TRect r)
                : TStatusLine(r, *new TStatusDef(0, 0xFFFF, nullptr))
            {
                setContext(MarkdownStatusContext{});
            }

            ~MarkdownStatusLine() override
            {
                disposeItemList(items);
                items = nullptr;
                if (defs)
                    defs->items = nullptr;
            }

            void setContext(const MarkdownStatusContext &context)
            {
                if (lastContext && *lastContext == context)
                    return;
                lastContext = context;
                rebuildItems(context);
            }

            void showTemporaryMessage(const std::string &message)
            {
                temporaryMessage = message;
                showingTemporaryMessage = true;
                drawView();
            }

            void clearTemporaryMessage()
            {
                if (!showingTemporaryMessage)
                    return;
                showingTemporaryMessage = false;
                temporaryMessage.clear();
                drawView();
            }

            bool hasTemporaryMessage() const noexcept { return showingTemporaryMessage; }

            const char *hint(ushort helpCtx) override
            {
                if (showingTemporaryMessage)
                    return temporaryMessage.c_str();
                return TStatusLine::hint(helpCtx);
            }

        private:
            std::optional<MarkdownStatusContext> lastContext;
            std::string temporaryMessage;
            bool showingTemporaryMessage = false;

            void rebuildItems(const MarkdownStatusContext &context)
            {
                disposeItemList(items);
                items = nullptr;
                if (defs)
                    defs->items = nullptr;

                std::vector<ushort> commands = buildBaseCommands(context);
                std::vector<ushort> contextCommands = buildContextCommands(context);
                commands.insert(commands.end(), contextCommands.begin(), contextCommands.end());

                TStatusItem *head = nullptr;
                TStatusItem **tail = &head;
                for (ushort command : commands)
                {
                    std::string label = ck::hotkeys::commandLabel(command);
                    if (label.empty())
                        continue;
                    auto *item = new TStatusItem(label.c_str(), kbNoKey, command);
                    ck::hotkeys::configureStatusItem(*item, label);
                    *tail = item;
                    tail = &item->next;
                }

                items = head;
                if (defs)
                    defs->items = items;
                drawView();
            }

            static void disposeItemList(TStatusItem *item)
            {
                while (item)
                {
                    TStatusItem *next = item->next;
                    delete item;
                    item = next;
                }
            }
        };

        TSubMenu &makeFileMenu()
        {
            TSubMenu &menu = *new TSubMenu("~F~ile", kbNoKey) +
                             *new TMenuItem("~O~pen", cmOpen, kbNoKey, hcNoContext) +
                             *new TMenuItem("~N~ew", cmNew, kbNoKey, hcNoContext) +
                             *new TMenuItem("~S~ave", cmSave, kbNoKey, hcNoContext) +
                             *new TMenuItem("S~a~ve as...", cmSaveAs, kbNoKey) +
                             *new TMenuItem("~C~lose", cmClose, kbNoKey, hcNoContext) +
                             newLine() +
                             *new TMenuItem("~C~hange dir...", cmChangeDir, kbNoKey);
            if (ck::launcher::launchedFromCkLauncher())
                menu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher, kbNoKey, hcNoContext);
            menu + *new TMenuItem("E~x~it", cmQuit, kbNoKey, hcNoContext);
            return menu;
        }

        TSubMenu &makeHeadingsMenu()
        {
            return *new TSubMenu("~H~eadings", kbNoKey) +
                   *new TMenuItem("Heading ~1", cmHeading1, kbNoKey) +
                   *new TMenuItem("Heading ~2", cmHeading2, kbNoKey) +
                   *new TMenuItem("Heading ~3", cmHeading3, kbNoKey) +
                   *new TMenuItem("Heading ~4", cmHeading4, kbNoKey) +
                   *new TMenuItem("Heading ~5", cmHeading5, kbNoKey) +
                   *new TMenuItem("Heading ~6", cmHeading6, kbNoKey) +
                   newLine() +
                   *new TMenuItem("C~l~ear Heading", cmClearHeading, kbNoKey);
        }

        TSubMenu &makeTextStyleMenu()
        {
            return *new TSubMenu("Te~x~t Style", kbNoKey) +
                   *new TMenuItem("~B~old", cmBold, kbNoKey, hcNoContext) +
                   *new TMenuItem("~I~talic", cmItalic, kbNoKey, hcNoContext) +
                   *new TMenuItem("Bold + Italic", cmBoldItalic, kbNoKey) +
                   *new TMenuItem("~S~trikethrough", cmStrikethrough, kbNoKey) +
                   *new TMenuItem("Remove Formatting", cmRemoveFormatting, kbNoKey);
        }

        TSubMenu &makeBlocksMenu()
        {
            return *new TSubMenu("~B~locks", kbNoKey) +
                   *new TMenuItem("Make Paragraph", cmMakeParagraph, kbNoKey) +
                   *new TMenuItem("Toggle Blockquote", cmToggleBlockQuote, kbNoKey) +
                   newLine() +
                   *new TMenuItem("Increase Indent", cmIncreaseIndent, kbNoKey) +
                   *new TMenuItem("Decrease Indent", cmDecreaseIndent, kbNoKey);
        }

        constexpr const char *kSmartListMenuBaseLabel = "Auto List Continuation";
        TMenuItem *gSmartListMenuItem = nullptr;
        bool gSmartListMenuChecked = true;

        void clearSmartListMenuItem()
        {
            gSmartListMenuItem = nullptr;
        }

        void updateSmartListMenuItemLabel(bool enabled)
        {
            gSmartListMenuChecked = enabled;
            if (!gSmartListMenuItem)
                return;
            std::string label = std::string(enabled ? "[x] " : "[ ] ") + kSmartListMenuBaseLabel;
            delete[] const_cast<char *>(gSmartListMenuItem->name);
            gSmartListMenuItem->name = newStr(label.c_str());
        }

        TSubMenu &makeListsMenu()
        {
            auto *smartListItem = new TMenuItem(kSmartListMenuBaseLabel, cmToggleSmartList, kbNoKey);
            gSmartListMenuItem = smartListItem;
            updateSmartListMenuItemLabel(gSmartListMenuChecked);

            return *new TSubMenu("~L~ists", kbNoKey) +
                   *new TMenuItem("Bulleted List", cmToggleBulletList, kbNoKey) +
                   *new TMenuItem("Numbered List", cmToggleNumberedList, kbNoKey) +
                   *new TMenuItem("Task List", cmConvertTaskList, kbNoKey) +
                   *new TMenuItem("Toggle Checkbox", cmToggleTaskCheckbox, kbNoKey) +
                   *new TMenuItem("Definition List", cmDefinitionList, kbNoKey) +
                   newLine() +
                   *smartListItem;
        }

        TSubMenu &makeInsertMenu()
        {
            return *new TSubMenu("~I~nsert", kbNoKey) +
                   *new TMenuItem("Insert/Edit Link...", cmInsertLink, kbNoKey) +
                   *new TMenuItem("Reference Link...", cmInsertReferenceLink, kbNoKey) +
                   *new TMenuItem("Auto-link Selection", cmAutoLinkSelection, kbNoKey) +
                   newLine() +
                   *new TMenuItem("Line Break", cmInsertLineBreak, kbNoKey) +
                   *new TMenuItem("Horizontal Rule", cmInsertHorizontalRule, kbNoKey) +
                   *new TMenuItem("Escape Selection", cmEscapeSelection, kbNoKey) +
                   *new TMenuItem("Footnote", cmInsertFootnote, kbNoKey) +
                   newLine() +
                   *new TMenuItem("Inline Code", cmInlineCode, kbNoKey, hcNoContext) +
                   *new TMenuItem("Code Block...", cmCodeBlock, kbNoKey) +
                   *new TMenuItem("Insert Image...", cmInsertImage, kbNoKey);
        }

        TSubMenu &makeDocumentMenu()
        {
            return *new TSubMenu("Doc~u~ment", kbNoKey) +
                   *new TMenuItem("Reflow Paragraphs", cmReflowParagraphs, kbNoKey) +
                   *new TMenuItem("Format Document", cmFormatDocument, kbNoKey);
        }

        TSubMenu &makeTableMenu()
        {
            return *new TSubMenu("Ta~b~le", kbNoKey) +
                   *new TMenuItem("Insert ~T~able...", cmInsertTable, kbNoKey) +
                   newLine() +
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
                   *new TMenuItem("Delete table", cmTableDeleteTable, kbNoKey);
        }

        TSubMenu &makeViewMenu()
        {
            return *new TSubMenu("~V~iew", kbNoKey) +
                   *new TMenuItem("Toggle ~w~rap", cmToggleWrap, kbNoKey, hcNoContext) +
                   *new TMenuItem("Toggle ~M~arkdown mode", cmToggleMarkdownMode, kbNoKey, hcNoContext);
        }

        TSubMenu &makeWindowMenu()
        {
            return *new TSubMenu("~W~indows", kbNoKey) +
                   *new TMenuItem("~R~esize/Move", cmResize, kbNoKey, hcNoContext) +
                   *new TMenuItem("~Z~oom", cmZoom, kbNoKey, hcNoContext) +
                   *new TMenuItem("~N~ext", cmNext, kbNoKey, hcNoContext) +
                   *new TMenuItem("~C~lose", cmClose, kbNoKey, hcNoContext) +
                   *new TMenuItem("~T~ile", cmTile, kbNoKey) +
                   *new TMenuItem("C~a~scade", cmCascade, kbNoKey);
        }

        TSubMenu &makeHelpMenu()
        {
            return *new TSubMenu("~H~elp", kbNoKey) +
                   *new TMenuItem("~A~bout", cmAbout, kbNoKey, hcNoContext);
        }

        TSubMenu &makeEditMenu(bool markdownMode)
        {
            TSubMenu &edit = *new TSubMenu("~E~dit", kbNoKey) +
                             *new TMenuItem("~U~ndo", cmUndo, kbNoKey, hcNoContext) +
                             newLine() +
                             *new TMenuItem("Cu~t~", cmCut, kbNoKey, hcNoContext) +
                             *new TMenuItem("~C~opy", cmCopy, kbNoKey, hcNoContext) +
                             *new TMenuItem("~P~aste", cmPaste, kbNoKey, hcNoContext) +
                             newLine() +
                             *new TMenuItem("~F~ind...", cmFind, kbNoKey, hcNoContext) +
                             *new TMenuItem("~R~eplace...", cmReplace, kbNoKey, hcNoContext) +
                             *new TMenuItem("Find ~N~ext", cmSearchAgain, kbNoKey, hcNoContext);

            if (markdownMode)
            {
                edit + newLine() + makeHeadingsMenu() + makeTextStyleMenu() + makeBlocksMenu() + makeListsMenu() + makeDocumentMenu();
            }

            return edit;
        }

        class MarkdownMenuBar : public TMenuBar
        {
        public:
            explicit MarkdownMenuBar(TRect r)
                : TMenuBar(r, buildMenu(true)), markdownMode(true)
            {
            }

            void setMarkdownMode(bool mode)
            {
                if (markdownMode == mode)
                    return;
                markdownMode = mode;
                clearSmartListMenuItem();
                TMenu *newMenu = buildMenu(mode);
                delete menu;
                menu = newMenu;
                current = nullptr;
                drawView();
            }

        private:
            bool markdownMode;

            static TMenu *buildMenu(bool markdownMode)
            {
                if (markdownMode)
                {
                    TMenuItem &items = makeFileMenu() + makeEditMenu(true) + makeInsertMenu() + makeTableMenu() + makeViewMenu() + makeWindowMenu() + makeHelpMenu();
                    ck::hotkeys::configureMenuTree(items);
                    return new TMenu(items);
                }

                TMenuItem &items = makeFileMenu() + makeEditMenu(false) + makeViewMenu() + makeWindowMenu() + makeHelpMenu();
                ck::hotkeys::configureMenuTree(items);
                return new TMenu(items);
            }
        };

    } // namespace

    MarkdownInfoView::MarkdownInfoView(const TRect &bounds, MarkdownFileEditor *editor) noexcept
        : TView(bounds), editor(editor)
    {
        growMode = gfGrowHiY;
        eventMask = 0;
    }

    TPalette &MarkdownInfoView::getPalette() const
    {
        return TView::getPalette();
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

    std::string MarkdownInfoView::filterLabel(const std::string &label)
    {
        if (label == "Blank" || label == "Code" || label == "Code Fence End")
            return {};
        return label;
    }

    MarkdownInfoView::LineRenderInfo MarkdownInfoView::buildLineInfo(uint linePtr, int lineNumber, MarkdownParserState &state)
    {
        LineRenderInfo info;
        info.isActive = editor && (lineNumber == editor->documentLineNumber());
        info.lineActive = info.isActive;
        info.lineNumber = lineNumber;
        info.visualRowIndex = 0;
        info.visualRowCount = 1;

        if (!editor || linePtr >= editor->bufLen)
            return info;

        std::string incomingFenceLabel = state.fenceLabel;
        MarkdownLineInfo lineInfo = editor->analyzer().analyzeLine(editor->lineText(linePtr), state);
        info.hasLine = true;
        info.lineKind = lineInfo.kind;
        std::string baseLabel = editor->analyzer().describeLine(lineInfo);
        std::string fenceLabel = lineInfo.fenceLabel;
        if (lineInfo.inFence)
        {
            std::string derivedFenceLabel = lineInfo.language.empty() ? std::string("Code Fence")
                                                                      : std::string("Code Fence (") + lineInfo.language + ")";
            if (fenceLabel.empty())
                fenceLabel = derivedFenceLabel;
            if (incomingFenceLabel.empty())
                incomingFenceLabel = derivedFenceLabel;
        }
        if (fenceLabel.empty())
            fenceLabel = incomingFenceLabel;
        std::string groupSource = fenceLabel.empty() ? baseLabel : fenceLabel;
        info.groupLabel = filterLabel(groupSource);
        info.displayLabel = info.groupLabel;

        const bool isFencedCodeLine = lineInfo.kind == MarkdownLineKind::FencedCode;
        const bool isFenceEndLine = lineInfo.kind == MarkdownLineKind::CodeFenceEnd;
        if (isFencedCodeLine || isFenceEndLine)
        {
            if (!fenceLabel.empty())
                info.groupLabel = filterLabel(fenceLabel);
            if (info.isActive)
                info.displayLabel = baseLabel;
            else
                info.displayLabel.clear();
        }

        const bool suppressDisplayOverride = isFencedCodeLine || isFenceEndLine;

        if (info.isActive)
        {
            std::string tableLabel;
            if (lineInfo.kind == MarkdownLineKind::TableRow || lineInfo.kind == MarkdownLineKind::TableSeparator)
            {
                auto locateColumn = [&](int column)
                {
                    if (column < 0)
                        return -1;
                    if (lineInfo.tableCells.empty())
                        return -1;
                    for (std::size_t i = 0; i < lineInfo.tableCells.size(); ++i)
                    {
                        const auto &cell = lineInfo.tableCells[i];
                        auto endCol = std::max(cell.endColumn, cell.startColumn + 1);
                        if (column >= static_cast<int>(cell.startColumn) && column < static_cast<int>(endCol))
                            return static_cast<int>(i);
                    }
                    return static_cast<int>(lineInfo.tableCells.size()) - 1;
                };

                int columnIndex = locateColumn(editor->documentColumnNumber());
                if (columnIndex == -1 && editor->documentColumnNumber() > 0)
                    columnIndex = locateColumn(editor->documentColumnNumber() - 1);

                if (columnIndex >= 0)
                    tableLabel = sanitizeMultiline(
                        editor->analyzer().describeTableCell(lineInfo, static_cast<std::size_t>(columnIndex)));
            }

            const auto *span = editor->analyzer().spanAtColumn(lineInfo, editor->documentColumnNumber());
            if (!span && editor->documentColumnNumber() > 0)
                span = editor->analyzer().spanAtColumn(lineInfo, editor->documentColumnNumber() - 1);
            if (!tableLabel.empty())
            {
                if (span && span->kind != MarkdownSpanKind::PlainText)
                {
                    std::string spanLabel = sanitizeMultiline(editor->analyzer().describeSpan(*span));
                    if (!spanLabel.empty())
                    {
                        tableLabel.push_back(' ');
                        tableLabel.append(" -- ");
                        tableLabel.append(spanLabel);
                    }
                }
                info.displayLabel = tableLabel;
            }
            else if (span && !suppressDisplayOverride)
            {
                info.displayLabel = sanitizeMultiline(editor->analyzer().describeSpan(*span));
            }
        }

        return info;
    }

    MarkdownInfoView::LineRenderInfo MarkdownInfoView::buildLineInfo(uint linePtr, int lineNumber)
    {
        LineRenderInfo info;
        info.isActive = editor && (lineNumber == editor->documentLineNumber());
        info.lineActive = info.isActive;
        info.lineNumber = lineNumber;
        info.visualRowIndex = 0;
        info.visualRowCount = 1;
        if (!editor || linePtr >= editor->bufLen)
            return info;

        MarkdownParserState state = computeState(linePtr);
        return buildLineInfo(linePtr, lineNumber, state);
    }

    void MarkdownInfoView::refreshBoundaryLabels(uint topPtr, uint linePtrAfterView)
    {
        cachedLabelBeforeView.reset();
        cachedLabelAfterView.reset();

        if (!editor)
            return;

        if (topPtr > 0 && editor->bufLen > 0)
        {
            uint prevPtr = editor->lineMove(topPtr, -1);
            if (prevPtr < editor->bufLen)
            {
                MarkdownParserState prevState = computeState(prevPtr);
                std::string incomingFenceLabel = prevState.fenceLabel;
                MarkdownLineInfo prevInfo = editor->analyzer().analyzeLine(editor->lineText(prevPtr), prevState);
                std::string fenceLabel = prevInfo.fenceLabel;
                if (prevInfo.inFence)
                {
                    std::string derivedFenceLabel = prevInfo.language.empty() ? std::string("Code Fence")
                                                                              : std::string("Code Fence (") + prevInfo.language + ")";
                    if (fenceLabel.empty())
                        fenceLabel = derivedFenceLabel;
                    if (incomingFenceLabel.empty())
                        incomingFenceLabel = derivedFenceLabel;
                }
                if (fenceLabel.empty())
                    fenceLabel = incomingFenceLabel;
                std::string label = filterLabel(editor->analyzer().describeLine(prevInfo));
                if (label.empty() && !fenceLabel.empty())
                    label = filterLabel(fenceLabel);
                if (!label.empty())
                    cachedLabelBeforeView = label;
            }
        }

        if (linePtrAfterView < editor->bufLen)
        {
            MarkdownParserState afterState = computeState(linePtrAfterView);
            std::string incomingFenceLabel = afterState.fenceLabel;
            MarkdownLineInfo nextInfo = editor->analyzer().analyzeLine(editor->lineText(linePtrAfterView), afterState);
            std::string fenceLabel = nextInfo.fenceLabel;
            if (nextInfo.inFence)
            {
                std::string derivedFenceLabel = nextInfo.language.empty() ? std::string("Code Fence")
                                                                          : std::string("Code Fence (") + nextInfo.language + ")";
                if (fenceLabel.empty())
                    fenceLabel = derivedFenceLabel;
                if (incomingFenceLabel.empty())
                    incomingFenceLabel = derivedFenceLabel;
            }
            if (fenceLabel.empty())
                fenceLabel = incomingFenceLabel;
            std::string label = filterLabel(editor->analyzer().describeLine(nextInfo));
            if (label.empty() && !fenceLabel.empty())
                label = filterLabel(fenceLabel);
            if (!label.empty())
                cachedLabelAfterView = label;
        }
    }

    void MarkdownInfoView::rebuildCache()
    {
        cachedLines.assign(static_cast<std::size_t>(std::max(0, size.y)), {});
        cachedGroups.clear();
        cachedTopLineNumber = editor ? editor->delta.y : -1;
        cachedLabelBeforeView.reset();
        cachedLabelAfterView.reset();
        cacheValid = false;

        if (!editor || !editor->isMarkdownMode() || size.y <= 0)
            return;

        const int viewportRows = size.y;
        uint topPtr = editor->topLinePointer();
        MarkdownParserState state = computeState(topPtr);
        uint linePtr = topPtr;
        uint linePtrAfterView = topPtr;
        int lineNumber = cachedTopLineNumber;
        int row = 0;
        while (row < viewportRows)
        {
            if (linePtr < editor->bufLen)
            {
                LineRenderInfo base = buildLineInfo(linePtr, lineNumber, state);
                MarkdownFileEditor::WrapLayout layout;
                editor->computeWrapLayout(linePtr, layout);
                int totalRows = std::max<int>(1, static_cast<int>(layout.segments.size()));
                int visibleRows = std::min(totalRows, viewportRows - row);
                int caretRowIndex = -1;
                if (base.lineActive && totalRows > 0)
                {
                    caretRowIndex = editor->wrapSegmentForColumn(layout, editor->documentColumnNumber());
                    caretRowIndex = std::clamp(caretRowIndex, 0, totalRows - 1);
                }

                LineGroupCache group;
                group.lineNumber = lineNumber;
                group.firstRow = row;
                group.visibleRows = visibleRows;
                group.totalRows = totalRows;
                group.activeRow = caretRowIndex;
                cachedGroups.push_back(group);

                for (int i = 0; i < visibleRows && row < viewportRows; ++i, ++row)
                {
                    LineRenderInfo rowInfo = base;
                    rowInfo.visualRowIndex = i;
                    rowInfo.visualRowCount = totalRows;
                    rowInfo.lineActive = base.lineActive;
                    bool isCursorRow = (caretRowIndex >= 0 && i == caretRowIndex);
                    rowInfo.isActive = isCursorRow;
                    if (base.lineActive)
                        rowInfo.displayLabel = isCursorRow ? base.displayLabel : std::string{};
                    else
                        rowInfo.displayLabel = (i == 0) ? base.displayLabel : std::string{};
                    cachedLines[static_cast<std::size_t>(row)] = std::move(rowInfo);
                }

                linePtrAfterView = editor->nextLine(linePtr);
                if (linePtrAfterView <= linePtr)
                    linePtrAfterView = editor->bufLen;
                linePtr = linePtrAfterView;
            }
            else
            {
                LineRenderInfo info;
                info.lineNumber = lineNumber;
                info.isActive = (lineNumber == editor->documentLineNumber());
                info.lineActive = info.isActive;
                info.visualRowIndex = 0;
                info.visualRowCount = 1;
                cachedLines[static_cast<std::size_t>(row)] = std::move(info);
                ++row;
                linePtrAfterView = editor->bufLen;
            }
            ++lineNumber;
        }

        refreshBoundaryLabels(topPtr, linePtrAfterView);
        cacheValid = true;
    }

    void MarkdownInfoView::draw()
    {
        const auto normalPair = getColor(0x0301);
        const auto activePair = getColor(0x0604);
        const auto normalAttr = normalPair[0];
        const auto activeAttr = activePair[0];

        if (!editor || !editor->isMarkdownMode())
        {
            cacheValid = false;
            cachedLines.clear();
            cachedGroups.clear();
            cachedLabelBeforeView.reset();
            cachedLabelAfterView.reset();
            cachedTopLineNumber = -1;

            TDrawBuffer buffer;
            for (int y = 0; y < size.y; ++y)
            {
                buffer.moveChar(0, ' ', normalAttr, size.x);
                if (y == 0)
                    buffer.moveCStr(1, "Plain Text", normalPair);
                if (size.x > 0)
                    buffer.moveStr(size.x - 1, "│", normalAttr);
                writeLine(0, y, size.x, 1, buffer);
            }
            return;
        }

        rebuildCache();

        for (int row = 0; row < size.y; ++row)
            renderRow(row);
    }

    void MarkdownInfoView::renderRow(int row)
    {
        if (row < 0 || row >= size.y)
            return;
        if (cachedLines.size() != static_cast<std::size_t>(size.y))
            return;

        const auto normalPair = getColor(0x0301);
        const auto activePair = getColor(0x0604);
        const auto normalAttr = normalPair[0];
        const auto activeAttr = activePair[0];

        const LineRenderInfo &line = cachedLines[static_cast<std::size_t>(row)];
        bool isActiveRow = line.isActive;
        bool isHeadingLine = line.lineKind == MarkdownLineKind::Heading;

        auto setBoldStyle = [](TColorAttr &attr)
        {
            setStyle(attr, getStyle(attr) | slBold);
        };

        TColorAttr rowAttr = isActiveRow ? activeAttr : normalAttr;
        if (isHeadingLine)
            setBoldStyle(rowAttr);

        TDrawBuffer buffer;
        int dividerCol = size.x - 1;
        int contentWidth = std::max(0, dividerCol);
        if (contentWidth > 0)
            buffer.moveChar(0, ' ', rowAttr, contentWidth);

        auto applyHeadingStyle = [&](TAttrPair &pair)
        {
            setBoldStyle(pair[0]);
            setBoldStyle(pair[1]);
        };

        if (line.hasLine)
        {
            bool hasGroupLabel = !line.groupLabel.empty();
            bool hasPrevSame = false;
            bool hasNextSame = false;

            if (hasGroupLabel)
            {
                if (row > 0 && cachedLines[static_cast<std::size_t>(row - 1)].hasLine &&
                    cachedLines[static_cast<std::size_t>(row - 1)].groupLabel == line.groupLabel)
                    hasPrevSame = true;
                else if (row == 0 && cachedLabelBeforeView && *cachedLabelBeforeView == line.groupLabel)
                    hasPrevSame = true;

                if (row + 1 < size.y && cachedLines[static_cast<std::size_t>(row + 1)].hasLine &&
                    cachedLines[static_cast<std::size_t>(row + 1)].groupLabel == line.groupLabel)
                    hasNextSame = true;
                else if (row == size.y - 1 && cachedLabelAfterView && *cachedLabelAfterView == line.groupLabel)
                    hasNextSame = true;
            }

            if (hasPrevSame)
            {
                const char *connector = hasNextSame ? "│" : "└";
                buffer.moveStr(0, connector, rowAttr);
            }

            const bool showLabel = !line.displayLabel.empty() &&
                                   (!hasPrevSame || isActiveRow || !hasGroupLabel);
            if (showLabel)
            {
                TAttrPair labelPair = isActiveRow ? activePair : normalPair;
                if (isHeadingLine)
                    applyHeadingStyle(labelPair);
                if (!line.displayLabel.empty() && line.displayLabel != line.groupLabel)
                {
                    auto adjustForeground = [&](TColorAttr attr)
                    {
                        TColorAttr result = attr;
                        setFore(result, TColorDesired(TColorBIOS(isActiveRow ? 0x10 : 0x0E)));
                        return result;
                    };
                    labelPair[0] = adjustForeground(labelPair[0]);
                    labelPair[1] = adjustForeground(labelPair[1]);
                }
                ushort startCol = 0;
                if (hasPrevSame && isActiveRow)
                    startCol = 1;
                int availableWidth = std::max(0, contentWidth - static_cast<int>(startCol));
                if (availableWidth > 0)
                    buffer.moveCStr(startCol, line.displayLabel, labelPair, availableWidth);
            }
            else if (!hasPrevSame && hasGroupLabel)
            {
                TAttrPair labelPair = isActiveRow ? activePair : normalPair;
                if (isHeadingLine)
                    applyHeadingStyle(labelPair);
                if (contentWidth > 0)
                    buffer.moveCStr(0, line.groupLabel, labelPair, contentWidth);
            }
        }

        if (size.x > 0)
        {
            MarkdownEditWindow *window = editor ? editor->hostWindow : nullptr;
            TColorAttr dividerAttr = normalAttr;
            if (window && window->frame)
            {
                bool dragging = (window->frame->state & sfDragging) != 0;
                bool activeFrame = (window->frame->state & sfActive) != 0;

                ushort colorIndex;
                if (dragging)
                    colorIndex = 0x0505;
                else if (!activeFrame)
                    colorIndex = 0x0101;
                else
                    colorIndex = 0x0503;

                dividerAttr = window->frame->getColor(colorIndex)[0];
            }
            buffer.moveStr(dividerCol, "│", dividerAttr);
        }
        writeLine(0, row, size.x, 1, buffer);
    }

    void MarkdownInfoView::updateLines(const std::vector<int> &lineNumbers)
    {
        if (!editor || !editor->isMarkdownMode())
            return;

        if (!cacheValid || cachedTopLineNumber != editor->delta.y ||
            cachedLines.size() != static_cast<std::size_t>(size.y))
        {
            drawView();
            return;
        }

        if (lineNumbers.empty())
            return;

        uint topPtr = editor->topLinePointer();
        std::vector<int> rowsChanged;
        rowsChanged.reserve(lineNumbers.size() * 2);

        for (int lineNumber : lineNumbers)
        {
            if (lineNumber < 0 || lineNumber < cachedTopLineNumber)
                continue;

            auto groupIt = std::find_if(cachedGroups.begin(), cachedGroups.end(),
                                         [lineNumber](const LineGroupCache &group)
                                         { return group.lineNumber == lineNumber; });
            if (groupIt == cachedGroups.end())
                continue;

            uint linePtr = topPtr;
            int offset = lineNumber - cachedTopLineNumber;
            if (offset > 0)
                linePtr = editor->lineMove(topPtr, offset);
            if (linePtr > editor->bufLen)
                linePtr = editor->bufLen;

            LineRenderInfo base = buildLineInfo(linePtr, lineNumber);
            MarkdownFileEditor::WrapLayout layout;
            editor->computeWrapLayout(linePtr, layout);
            int newTotalRows = std::max<int>(1, static_cast<int>(layout.segments.size()));
            int newVisibleRows = std::min(newTotalRows, size.y - groupIt->firstRow);
            if (newVisibleRows <= 0)
                continue;

            if (newTotalRows != groupIt->totalRows || newVisibleRows != groupIt->visibleRows)
            {
                drawView();
                return;
            }

            int caretRowIndex = -1;
            if (base.lineActive && newTotalRows > 0)
            {
                caretRowIndex = editor->wrapSegmentForColumn(layout, editor->documentColumnNumber());
                caretRowIndex = std::clamp(caretRowIndex, 0, newTotalRows - 1);
            }
            groupIt->activeRow = caretRowIndex;
            groupIt->visibleRows = newVisibleRows;
            groupIt->totalRows = newTotalRows;

            for (int i = 0; i < groupIt->visibleRows; ++i)
            {
                LineRenderInfo rowInfo = base;
                rowInfo.visualRowIndex = i;
                rowInfo.visualRowCount = newTotalRows;
                rowInfo.lineActive = base.lineActive;
                bool isCursorRow = (caretRowIndex >= 0 && i == caretRowIndex);
                rowInfo.isActive = isCursorRow;
                if (base.lineActive)
                    rowInfo.displayLabel = isCursorRow ? base.displayLabel : std::string{};
                else
                    rowInfo.displayLabel = (i == 0) ? base.displayLabel : std::string{};
                int rowIndex = groupIt->firstRow + i;
                if (rowIndex >= size.y)
                    break;
                cachedLines[static_cast<std::size_t>(rowIndex)] = std::move(rowInfo);
                rowsChanged.push_back(rowIndex);
            }
        }

        if (rowsChanged.empty())
            return;

        bool touchesFirst = false;
        bool touchesLast = false;
        for (int row : rowsChanged)
        {
            touchesFirst = touchesFirst || (row == 0);
            touchesLast = touchesLast || (row == size.y - 1);
        }

        if (touchesFirst || touchesLast)
        {
            uint linePtrAfterView = topPtr;
            int linesVisible = static_cast<int>(cachedGroups.size());
            for (int i = 0; i < linesVisible && linePtrAfterView < editor->bufLen; ++i)
            {
                uint next = editor->nextLine(linePtrAfterView);
                if (next <= linePtrAfterView)
                {
                    linePtrAfterView = editor->bufLen;
                    break;
                }
                linePtrAfterView = next;
            }
            refreshBoundaryLabels(topPtr, linePtrAfterView);
        }

        std::vector<int> rowsToDraw;
        rowsToDraw.reserve(rowsChanged.size() * 3);
        for (int row : rowsChanged)
        {
            for (int neighbor = row - 1; neighbor <= row + 1; ++neighbor)
            {
                if (neighbor >= 0 && neighbor < size.y)
                    rowsToDraw.push_back(neighbor);
            }
        }
        std::sort(rowsToDraw.begin(), rowsToDraw.end());
        rowsToDraw.erase(std::unique(rowsToDraw.begin(), rowsToDraw.end()), rowsToDraw.end());

        for (int row : rowsToDraw)
            renderRow(row);
    }

    TFrame *MarkdownEditWindow::initFrame(TRect bounds)
    {
        return new MarkdownWindowFrame(bounds);
    }

    MarkdownEditWindow::MarkdownEditWindow(const TRect &bounds, TStringView fileName, int aNumber) noexcept
        : TWindowInit(&MarkdownEditWindow::initFrame), TWindow(bounds, nullptr, aNumber)
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
        fileEditor->setHostWindow(this);
        updateLayoutForMode();
        updateWindowTitle();
    }

    void MarkdownEditWindow::updateLayoutForMode()
    {
        if (!fileEditor || !hScrollBar)
            return;

        const bool markdown = fileEditor->isMarkdownMode();

        if (infoView)
        {
            if (markdown)
            {
                TRect infoRect(1, 1, 1 + kInfoColumnWidth, size.y - 1);
                infoView->show();
                infoView->locate(infoRect);
            }
            else
            {
                infoView->hide();
            }
        }

        TRect editorRect = markdown ? TRect(1 + kInfoColumnWidth, 1, size.x - 1, size.y - 1)
                                    : TRect(1, 1, size.x - 1, size.y - 1);
        fileEditor->locate(editorRect);

        TRect hScrollRect = markdown ? TRect(1 + kInfoColumnWidth, size.y - 1, size.x - 2, size.y)
                                     : TRect(1, size.y - 1, size.x - 2, size.y);
        hScrollBar->locate(hScrollRect);

        if (infoView && markdown)
            infoView->drawView();
        fileEditor->drawView();
        hScrollBar->drawView();

        if (frame)
            frame->drawView();

        if (auto *app = dynamic_cast<MarkdownEditorApp *>(TProgram::application))
            app->refreshUiMode();
    }

    void MarkdownEditWindow::applyWindowTitle(const std::string &titleText)
    {
        if (title)
        {
            delete[] const_cast<char *>(title);
            title = nullptr;
        }
        title = newStr(titleText.c_str());
        if (frame)
            frame->drawView();
    }

    void MarkdownEditWindow::updateWindowTitle()
    {
        if (!fileEditor)
            return;

        std::string displayName;
        if (fileEditor->fileName[0] != '\0')
        {
            std::filesystem::path path(fileEditor->fileName);
            displayName = path.filename().string();
            if (displayName.empty())
                displayName = path.string();
        }
        else
        {
            displayName = "Untitled";
        }

        applyWindowTitle(displayName);
    }

    bool MarkdownEditWindow::saveDocument(bool forceSaveAs)
    {
        if (!fileEditor)
            return false;

        std::string previousName = fileEditor->fileName;
        bool saved = forceSaveAs ? static_cast<bool>(fileEditor->saveAs()) 
                                 : static_cast<bool>(fileEditor->save());
        if (!saved)
            return false;

        std::string newName = fileEditor->fileName;
        if (previousName != newName && !newName.empty())
            fileEditor->setMarkdownMode(MarkdownFileEditor::isMarkdownFileName(newName));

        updateWindowTitle();

        std::string savedPath = newName.empty() ? std::string("Untitled") : newName;

        if (auto *app = dynamic_cast<MarkdownEditorApp *>(TProgram::application))
            app->showDocumentSavedMessage(savedPath);

        return true;
    }

    void MarkdownEditWindow::handleEvent(TEvent &event)
    {
        TWindow::handleEvent(event);
        if (event.what == evBroadcast && event.message.command == cmUpdateTitle)
        {
            updateWindowTitle();
            clearEvent(event);
        }
    }

    void MarkdownEditWindow::refreshDivider()
    {
        if (!fileEditor || !fileEditor->isMarkdownMode())
            return;
        if (infoView && (infoView->state & sfVisible))
            infoView->drawView();
    }

    void MarkdownEditWindow::setState(ushort aState, Boolean enable)
    {
        TWindow::setState(aState, enable);
        if ((aState & sfActive) != 0 && enable)
        {
            if (auto *app = dynamic_cast<MarkdownEditorApp *>(TProgram::application))
                app->refreshUiMode();
        }
    }

    MarkdownEditorApp::MarkdownEditorApp(int argc, char **argv)
        : TProgInit(&MarkdownEditorApp::initStatusLine, &MarkdownEditorApp::initMenuBar, &TApplication::initDeskTop),
          TApplication()
    {
        auto clockBounds = ck::ui::clockBoundsFrom(getExtent());
        auto *clock = new ck::ui::ClockView(clockBounds);
        clock->growMode = gfGrowLoX | gfGrowHiX;
        insert(clock);

        TEditor::editorDialog = runEditorDialog;

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
        refreshUiMode();
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
        ck::ui::showAboutDialog(appName(), CK_EDIT_VERSION, appAboutDescription());
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

    void MarkdownEditorApp::showDocumentSavedMessage(const std::string &path)
    {
        if (!statusLine)
            return;
        auto *line = dynamic_cast<MarkdownStatusLine *>(statusLine);
        if (!line)
            return;

        std::string message = "Document saved: " + path;
        line->showTemporaryMessage(message);

        const uint32_t token = statusMessageCounter.fetch_add(1, std::memory_order_relaxed) + 1;
        activeStatusMessageToken.store(token, std::memory_order_release);
        pendingStatusMessageClear.store(0, std::memory_order_release);

        std::thread([this, token]()
                    {
        delay(3000);
        pendingStatusMessageClear.store(token, std::memory_order_release); }).detach();
    }

    void MarkdownEditorApp::clearStatusMessage()
    {
        if (!statusLine)
            return;
        if (auto *line = dynamic_cast<MarkdownStatusLine *>(statusLine))
            line->clearTemporaryMessage();
    }

    void MarkdownEditorApp::handleEvent(TEvent &event)
    {
        TApplication::handleEvent(event);
        if (event.what != evCommand)
        {
            refreshUiMode();
            return;
        }

        bool handled = true;
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
        case cmSave:
        case cmSaveAs:
            dispatchToEditor(event.message.command);
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
        case cmMakeParagraph:
        case cmInsertLineBreak:
        case cmFind:
        case cmReplace:
        case cmSearchAgain:
        case cmBold:
        case cmItalic:
        case cmBoldItalic:
        case cmStrikethrough:
        case cmInlineCode:
        case cmCodeBlock:
        case cmRemoveFormatting:
        case cmToggleBlockQuote:
        case cmToggleBulletList:
        case cmToggleNumberedList:
        case cmConvertTaskList:
        case cmToggleTaskCheckbox:
        case cmIncreaseIndent:
        case cmDecreaseIndent:
        case cmDefinitionList:
        case cmInsertLink:
        case cmInsertReferenceLink:
        case cmAutoLinkSelection:
        case cmInsertImage:
        case cmInsertFootnote:
        case cmInsertHorizontalRule:
        case cmEscapeSelection:
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
        case cmReflowParagraphs:
        case cmFormatDocument:
        case cmToggleSmartList:
            dispatchToEditor(event.message.command);
            break;
        case cmReturnToLauncher:
            std::exit(ck::launcher::kReturnToLauncherExitCode);
            break;
        case cmAbout:
            showAbout();
            break;
        default:
            handled = false;
            break;
        }
        if (handled)
            clearEvent(event);
        refreshUiMode();
    }

    static Boolean windowIsTileable(TView *view, void *)
    {
        return Boolean((view->options & ofTileable) != 0);
    }

    void MarkdownEditorApp::idle()
    {
        TApplication::idle();

        if (deskTop && deskTop->firstThat(windowIsTileable, nullptr) != nullptr)
        {
            enableCommand(cmTile);
            enableCommand(cmCascade);
        }
        else
        {
            disableCommand(cmTile);
            disableCommand(cmCascade);
        }

        uint32_t token = pendingStatusMessageClear.load(std::memory_order_acquire);
        if (token == 0)
            return;

        uint32_t active = activeStatusMessageToken.load(std::memory_order_acquire);
        if (token == active)
        {
            clearStatusMessage();
            pendingStatusMessageClear.store(0, std::memory_order_release);
            activeStatusMessageToken.store(0, std::memory_order_release);
        }
        else
        {
            uint32_t expected = token;
            pendingStatusMessageClear.compare_exchange_strong(expected, 0, 
                                                              std::memory_order_acq_rel, 
                                                              std::memory_order_acquire);
        }
    }

    TMenuBar *MarkdownEditorApp::initMenuBar(TRect r)
    {
        r.b.y = r.a.y + 1;
        return new MarkdownMenuBar(r);
    }

    TStatusLine *MarkdownEditorApp::initStatusLine(TRect r)
    {
        r.a.y = r.b.y - 1;
        return new MarkdownStatusLine(r);
    }

    void MarkdownEditorApp::updateStatusLine(const MarkdownStatusContext &context)
    {
        if (!statusLine)
            return;
        if (auto *line = dynamic_cast<MarkdownStatusLine *>(statusLine))
            line->setContext(context);
    }

    void MarkdownEditorApp::updateMenuBarForMode(bool markdownMode)
    {
        if (!menuBar)
            return;
        if (auto *bar = dynamic_cast<MarkdownMenuBar *>(menuBar))
            bar->setMarkdownMode(markdownMode);
    }

    void MarkdownEditorApp::refreshUiMode()
    {
        MarkdownStatusContext context;
        bool markdownMode = false;
        if (deskTop && deskTop->current)
        {
            if (auto *win = dynamic_cast<MarkdownEditWindow *>(deskTop->current))
            {
                if (auto *ed = win->editor())
                {
                    ed->buildStatusContext(context);
                    markdownMode = context.markdownMode;
                }
            }
        }
        updateStatusLine(context);
        updateMenuBarForMode(markdownMode);
        updateSmartListMenuItemLabel(context.hasEditor ? context.smartListContinuation : gSmartListMenuChecked);
    }

} // namespace ck::edit
