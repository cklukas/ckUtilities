#include "ck/edit/markdown_editor.hpp"

#include "ck/about_dialog.hpp"
#include "ck/launcher.hpp"

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

const std::array<std::string_view, 7> kMarkdownExtensions = {
    ".md", ".markdown", ".mdown", ".mkd", ".mkdn", ".mdtxt", ".mdtext"};

ushort execDialog(TDialog *d, void *data = nullptr);

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
    default:
        return cmCancel;
    }
}

void delay(unsigned milliseconds)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

const ushort cmToggleWrap = 3000;
const ushort cmToggleMarkdownMode = 3001;
const ushort cmHeading1 = 3010;
const ushort cmHeading2 = 3011;
const ushort cmHeading3 = 3012;
const ushort cmHeading4 = 3013;
const ushort cmHeading5 = 3014;
const ushort cmHeading6 = 3015;
const ushort cmClearHeading = 3016;
const ushort cmMakeParagraph = 3017;
const ushort cmInsertLineBreak = 3018;
const ushort cmBold = 3020;
const ushort cmItalic = 3021;
const ushort cmBoldItalic = 3022;
const ushort cmStrikethrough = 3023;
const ushort cmInlineCode = 3024;
const ushort cmCodeBlock = 3025;
const ushort cmRemoveFormatting = 3026;
const ushort cmToggleBlockQuote = 3030;
const ushort cmToggleBulletList = 3031;
const ushort cmToggleNumberedList = 3032;
const ushort cmConvertTaskList = 3033;
const ushort cmToggleTaskCheckbox = 3034;
const ushort cmIncreaseIndent = 3035;
const ushort cmDecreaseIndent = 3036;
const ushort cmDefinitionList = 3037;
const ushort cmInsertLink = 3040;
const ushort cmInsertReferenceLink = 3041;
const ushort cmAutoLinkSelection = 3042;
const ushort cmInsertImage = 3043;
const ushort cmInsertFootnote = 3044;
const ushort cmInsertHorizontalRule = 3045;
const ushort cmEscapeSelection = 3046;
const ushort cmInsertTable = 3050;
const ushort cmTableInsertRowAbove = 3051;
const ushort cmTableInsertRowBelow = 3052;
const ushort cmTableDeleteRow = 3053;
const ushort cmTableInsertColumnBefore = 3054;
const ushort cmTableInsertColumnAfter = 3055;
const ushort cmTableDeleteColumn = 3056;
const ushort cmTableDeleteTable = 3057;
const ushort cmTableAlignDefault = 3058;
const ushort cmTableAlignLeft = 3059;
const ushort cmTableAlignCenter = 3060;
const ushort cmTableAlignRight = 3061;
const ushort cmTableAlignNumber = 3062;
const ushort cmReflowParagraphs = 3070;
const ushort cmFormatDocument = 3071;
const ushort cmToggleSmartList = 3080;
const ushort cmAbout = 3090;
const ushort cmReturnToLauncher = 3091;

const std::unordered_map<ushort, MarkdownFileEditor::InlineCommandSpec> kInlineCommandSpecs = {
    {cmBold, {"Bold", "**", "**", "", false, true,
              MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
    {cmItalic, {"Italic", "*", "*", "", false, true,
                MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
    {cmBoldItalic, {"Bold + Italic", "***", "***", "", false, true,
                    MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
    {cmStrikethrough, {"Strikethrough", "~~", "~~", "", false, true,
                       MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
    {cmInlineCode, {"Inline Code", "`", "`", "", false, true,
                    MarkdownFileEditor::InlineCommandSpec::CursorPlacement::AfterPrefix}},
};

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

struct CommandHotkey
{
    ushort command;
    TKey key;
    const char *label;
};

const std::array<CommandHotkey, 53> kCommandHotkeys = {{
    {cmOpen, TKey(kbF3), "~F3~ Open"},
    {cmSave, TKey(kbF2), "~F2~ Save"},
    {cmSaveAs, TKey(kbShiftF12), "~Shift-F12~ Save As"},
    {cmReturnToLauncher, TKey(kbCtrlL), "~Ctrl-L~ Return"},
    {cmToggleWrap, TKey(kbCtrlW), "~Ctrl-W~ Wrap"},
    {cmToggleMarkdownMode, TKey(kbCtrlM), "~Ctrl-M~ Markdown"},
    {cmBold, TKey(kbCtrlB), "~Ctrl-B~ Bold"},
    {cmItalic, TKey(kbCtrlI), "~Ctrl-I~ Italic"},
    {cmBoldItalic, TKey('B', kbCtrlShift | kbShift), "~Ctrl+Shift+B~ Bold+Italic"},
    {cmStrikethrough, TKey('S', kbCtrlShift | kbShift), "~Ctrl+Shift+S~ Strike"},
    {cmInlineCode, TKey(kbCtrlK), "~Ctrl-K~ Inline Code"},
    {cmCodeBlock, TKey('K', kbCtrlShift | kbShift), "~Ctrl+Shift+K~ Code Block"},
    {cmRemoveFormatting, TKey('0', kbCtrlShift | kbShift), "~Ctrl+Shift+0~ Clear Format"},
    {cmMakeParagraph, TKey('P', kbCtrlShift | kbShift), "~Ctrl+Shift+P~ Paragraph"},
    {cmToggleBlockQuote, TKey('Q', kbCtrlShift | kbShift), "~Ctrl+Shift+Q~ Block Quote"},
    {cmToggleBulletList, TKey('U', kbCtrlShift | kbShift), "~Ctrl+Shift+U~ Bullet List"},
    {cmToggleNumberedList, TKey('O', kbCtrlShift | kbShift), "~Ctrl+Shift+O~ Numbered"},
    {cmConvertTaskList, TKey('T', kbCtrlShift | kbShift), "~Ctrl+Shift+T~ Task List"},
    {cmToggleTaskCheckbox, TKey('X', kbCtrlShift | kbShift), "~Ctrl+Shift+X~ Checkbox"},
    {cmIncreaseIndent, TKey(kbRight, kbCtrlShift | kbShift), "~Ctrl+Shift+Right~ Indent"},
    {cmDecreaseIndent, TKey(kbLeft, kbCtrlShift | kbShift), "~Ctrl+Shift+Left~ Outdent"},
    {cmDefinitionList, TKey('D', kbCtrlShift | kbShift), "~Ctrl+Shift+D~ Definition"},
    {cmToggleSmartList, TKey('A', kbCtrlShift | kbShift), "~Ctrl+Shift+A~ Auto List"},
    {cmInsertLink, TKey('L', kbCtrlShift | kbShift), "~Ctrl+Shift+L~ Link"},
    {cmInsertReferenceLink, TKey('R', kbCtrlShift | kbShift), "~Ctrl+Shift+R~ Reference"},
    {cmAutoLinkSelection, TKey('Y', kbCtrlShift | kbShift), "~Ctrl+Shift+Y~ Auto Link"},
    {cmInsertImage, TKey('I', kbCtrlShift | kbShift), "~Ctrl+Shift+I~ Image"},
    {cmInsertFootnote, TKey('N', kbCtrlShift | kbShift), "~Ctrl+Shift+N~ Footnote"},
    {cmInsertHorizontalRule, TKey('H', kbCtrlShift | kbShift), "~Ctrl+Shift+H~ Rule"},
    {cmEscapeSelection, TKey('E', kbCtrlShift | kbShift), "~Ctrl+Shift+E~ Escape"},
    {cmInsertTable, TKey('T', kbCtrlShift | kbAltShift), "~Ctrl+Alt+T~ Table"},
    {cmTableInsertRowAbove, TKey(kbUp, kbCtrlShift | kbAltShift), "~Ctrl+Alt+Up~ Row Above"},
    {cmTableInsertRowBelow, TKey(kbDown, kbCtrlShift | kbAltShift), "~Ctrl+Alt+Down~ Row Below"},
    {cmTableDeleteRow, TKey(kbDown, kbCtrlShift | kbAltShift | kbShift), "~Ctrl+Alt+Shift+Down~ Delete Row"},
    {cmTableInsertColumnBefore, TKey(kbLeft, kbCtrlShift | kbAltShift), "~Ctrl+Alt+Left~ Col Before"},
    {cmTableInsertColumnAfter, TKey(kbRight, kbCtrlShift | kbAltShift), "~Ctrl+Alt+Right~ Col After"},
    {cmTableDeleteColumn, TKey(kbRight, kbCtrlShift | kbAltShift | kbShift), "~Ctrl+Alt+Shift+Right~ Delete Col"},
    {cmTableDeleteTable, TKey('T', kbCtrlShift | kbAltShift | kbShift), "~Ctrl+Alt+Shift+T~ Delete Table"},
    {cmTableAlignDefault, TKey('D', kbCtrlShift | kbAltShift), "~Ctrl+Alt+D~ Align Default"},
    {cmTableAlignLeft, TKey('L', kbCtrlShift | kbAltShift), "~Ctrl+Alt+L~ Align Left"},
    {cmTableAlignCenter, TKey('C', kbCtrlShift | kbAltShift), "~Ctrl+Alt+C~ Align Center"},
    {cmTableAlignRight, TKey('R', kbCtrlShift | kbAltShift), "~Ctrl+Alt+R~ Align Right"},
    {cmTableAlignNumber, TKey('N', kbCtrlShift | kbAltShift), "~Ctrl+Alt+N~ Align Number"},
    {cmReflowParagraphs, TKey('P', kbCtrlShift | kbAltShift), "~Ctrl+Alt+P~ Reflow"},
    {cmFormatDocument, TKey('F', kbCtrlShift | kbAltShift), "~Ctrl+Alt+F~ Format Doc"},
    {cmHeading1, TKey('1', kbAltShift), "~Alt-1~ H1"},
    {cmHeading2, TKey('2', kbAltShift), "~Alt-2~ H2"},
    {cmHeading3, TKey('3', kbAltShift), "~Alt-3~ H3"},
    {cmHeading4, TKey('4', kbAltShift), "~Alt-4~ H4"},
    {cmHeading5, TKey('5', kbAltShift), "~Alt-5~ H5"},
    {cmHeading6, TKey('6', kbAltShift), "~Alt-6~ H6"},
    {cmClearHeading, TKey('0', kbAltShift), "~Alt-0~ Clear"},
    {cmInsertLineBreak, TKey(kbCtrlEnter), "~Ctrl+Enter~ Line Break"},
}};

const CommandHotkey *findHotkey(ushort command) noexcept
{
    for (const auto &entry : kCommandHotkeys)
    {
        if (entry.command == command)
            return &entry;
    }
    return nullptr;
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

    auto addCommand = [&](ushort command) {
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
            if (const auto *hotkey = findHotkey(command))
            {
                auto *item = new TStatusItem(hotkey->label, hotkey->key, hotkey->command);
                *tail = item;
                tail = &item->next;
            }
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
    TSubMenu &menu = *new TSubMenu("~F~ile", kbAltF) +
                     *new TMenuItem("~O~pen", cmOpen, kbF3, hcNoContext, "F3") +
                     *new TMenuItem("~N~ew", cmNew, kbCtrlN, hcNoContext, "Ctrl-N") +
                     *new TMenuItem("~S~ave", cmSave, kbF2, hcNoContext, "F2") +
                     *new TMenuItem("S~a~ve as...", cmSaveAs, kbNoKey) +
                     newLine() +
                     *new TMenuItem("~C~hange dir...", cmChangeDir, kbNoKey);
    if (ck::launcher::launchedFromCkLauncher())
        menu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher, kbCtrlL, hcNoContext, "Ctrl-L");
    menu + *new TMenuItem("E~x~it", cmQuit, kbCtrlQ, hcNoContext, "Ctrl-Q");
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
           *new TMenuItem("~B~old", cmBold, kbCtrlB, hcNoContext, "Ctrl-B") +
           *new TMenuItem("~I~talic", cmItalic, kbCtrlI, hcNoContext, "Ctrl-I") +
           *new TMenuItem("Bold + Italic", cmBoldItalic, kbNoKey) +
           *new TMenuItem("~S~trikethrough", cmStrikethrough, kbNoKey) +
           *new TMenuItem("Remove Formatting", cmRemoveFormatting, kbNoKey);
}

TSubMenu &makeCodeMediaMenu()
{
    return *new TSubMenu("~C~ode && Media", kbNoKey) +
           *new TMenuItem("Inline Code", cmInlineCode, kbCtrlK, hcNoContext, "Ctrl-K") +
           *new TMenuItem("Code Block...", cmCodeBlock, kbNoKey) +
           *new TMenuItem("Insert Image...", cmInsertImage, kbNoKey);
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

TSubMenu &makeListsMenu()
{
    return *new TSubMenu("~L~ists", kbNoKey) +
           *new TMenuItem("Bulleted List", cmToggleBulletList, kbNoKey) +
           *new TMenuItem("Numbered List", cmToggleNumberedList, kbNoKey) +
           *new TMenuItem("Task List", cmConvertTaskList, kbNoKey) +
           *new TMenuItem("Toggle Checkbox", cmToggleTaskCheckbox, kbNoKey) +
           *new TMenuItem("Definition List", cmDefinitionList, kbNoKey) +
           newLine() +
           *new TMenuItem("Auto List Continuation", cmToggleSmartList, kbNoKey);
}

TSubMenu &makeInsertMenu()
{
    return *new TSubMenu("I~n~sert", kbNoKey) +
           *new TMenuItem("Insert/Edit Link...", cmInsertLink, kbNoKey) +
           *new TMenuItem("Reference Link...", cmInsertReferenceLink, kbNoKey) +
           *new TMenuItem("Auto-link Selection", cmAutoLinkSelection, kbNoKey) +
           newLine() +
           *new TMenuItem("Line Break", cmInsertLineBreak, kbNoKey) +
           *new TMenuItem("Horizontal Rule", cmInsertHorizontalRule, kbNoKey) +
           *new TMenuItem("Escape Selection", cmEscapeSelection, kbNoKey) +
           newLine() +
           *new TMenuItem("Footnote", cmInsertFootnote, kbNoKey);
}

TSubMenu &makeDocumentMenu()
{
    return *new TSubMenu("Doc~u~ment", kbNoKey) +
           *new TMenuItem("Reflow Paragraphs", cmReflowParagraphs, kbNoKey) +
           *new TMenuItem("Format Document", cmFormatDocument, kbNoKey);
}

TSubMenu &makeTableMenu()
{
    return *new TSubMenu("Ta~b~le", kbAltB) +
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
    return *new TSubMenu("~V~iew", kbAltV) +
           *new TMenuItem("Toggle ~w~rap", cmToggleWrap, kbCtrlW, hcNoContext, "Ctrl-W") +
           *new TMenuItem("Toggle ~M~arkdown mode", cmToggleMarkdownMode, kbCtrlM, hcNoContext, "Ctrl-M");
}

TSubMenu &makeHelpMenu()
{
    return *new TSubMenu("~H~elp", kbAltH) +
           *new TMenuItem("~A~bout", cmAbout, kbNoKey);
}

TSubMenu &makeEditMenu(bool markdownMode)
{
    TSubMenu &edit = *new TSubMenu("~E~dit", kbAltE) +
                     *new TMenuItem("~U~ndo", cmUndo, kbCtrlU, hcNoContext, "Ctrl-U") +
                     newLine() +
                     *new TMenuItem("Cu~t~", cmCut, kbShiftDel, hcNoContext, "Shift-Del") +
                     *new TMenuItem("~C~opy", cmCopy, kbCtrlIns, hcNoContext, "Ctrl-Ins") +
                     *new TMenuItem("~P~aste", cmPaste, kbShiftIns, hcNoContext, "Shift-Ins") +
                     newLine() +
                     *new TMenuItem("~F~ind...", cmFind, kbCtrlF, hcNoContext, "Ctrl-F") +
                     *new TMenuItem("~R~eplace...", cmReplace, kbCtrlR, hcNoContext, "Ctrl-R") +
                     *new TMenuItem("Find ~N~ext", cmSearchAgain, kbCtrlL, hcNoContext, "Ctrl-L");

    if (markdownMode)
    {
        edit + newLine() + makeHeadingsMenu() + makeTextStyleMenu() + makeCodeMediaMenu() + makeBlocksMenu() +
            makeListsMenu() + makeInsertMenu() + makeDocumentMenu();
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
            TMenuItem &items = makeFileMenu() + makeEditMenu(true) + makeTableMenu() + makeViewMenu() + makeHelpMenu();
            return new TMenu(items);
        }

        TMenuItem &items = makeFileMenu() + makeEditMenu(false) + makeViewMenu() + makeHelpMenu();
        return new TMenu(items);
    }
};

} // namespace

MarkdownFileEditor::MarkdownFileEditor(const TRect &bounds, TScrollBar *hScroll,
                                       TScrollBar *vScroll, TIndicator *indicator,
                                       TStringView fileName) noexcept
    : TFileEditor(bounds, hScroll, vScroll, indicator, fileName)
{
    if (!fileName.empty())
        markdownMode = isMarkdownFile(std::string(fileName));
    else
        markdownMode = false;
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
                std::string inner =
                    text.substr(spec.prefix.size(), text.size() - spec.prefix.size() - spec.suffix.size());
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
        line.insert(line.begin(), { ' ', ' ' });
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
    link << '[' << selectionText << "][" << referenceId << ']';

    lock();
    if (hasSelection())
        deleteSelect();
    insertText(link.str().c_str(), link.str().size(), False);
    unlock();
    onContentModified();

    std::ostringstream def;
    def << '[' << referenceId << "]: " << url;
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
    auto isUrl = [&](const std::string &value) {
        return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0 || value.rfind("ftp://", 0) == 0;
    };
    auto isEmail = [&](const std::string &value) {
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

    auto reflowParagraph = [](const std::string &paragraph) {
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
    if (continueListOnEnter(event))
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
    statusCachePrefixPtr = UINT_MAX;
    statusCacheVersion = 0;
    if (infoView)
    {
        infoView->invalidateState();
        if (markdownMode && (infoView->state & sfVisible))
            infoView->drawView();
    }
    if (auto *app = dynamic_cast<MarkdownEditorApp *>(TProgram::application))
        app->refreshUiMode();
}

void MarkdownFileEditor::buildStatusContext(MarkdownStatusContext &context)
{
    context = {};
    context.hasEditor = true;
    context.markdownMode = markdownMode;
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
    const TColorAttr normalAttr = TColorAttr(0x70);
    const TColorAttr activeAttr = TColorAttr(0xF0);
    const TAttrPair normalPair(normalAttr, normalAttr);
    const TAttrPair activePair(activeAttr, activeAttr);
    if (!editor || !editor->isMarkdownMode())
    {
        TDrawBuffer buffer;
        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', normalAttr, size.x);
            if (y == 0)
                buffer.moveCStr(1, "Plain Text", normalPair);
            if (size.x > 0)
            {
                buffer.moveStr(size.x - 1, "│", normalAttr);
            }
            writeLine(0, y, size.x, 1, buffer);
        }
        return;
    }

    uint topPtr = editor->topLinePointer();
    MarkdownParserState stateBeforeTop = computeState(topPtr);
    MarkdownParserState state = stateBeforeTop;
    uint linePtr = topPtr;
    int lineNumber = editor->delta.y;

    struct LineRenderInfo
    {
        bool hasLine = false;
        bool isActive = false;
        std::string displayLabel;
        std::string groupLabel;
    };

    std::vector<LineRenderInfo> lines;
    lines.reserve(size.y);

    for (int row = 0; row < size.y; ++row)
    {
        LineRenderInfo info;
        info.isActive = lineNumber == editor->curPos.y;
        if (linePtr < editor->bufLen)
        {
            MarkdownLineInfo lineInfo = editor->analyzer().analyzeLine(editor->lineText(linePtr), state);
            info.groupLabel = editor->analyzer().describeLine(lineInfo);
            info.displayLabel = info.groupLabel;
            if (info.isActive)
            {
                std::string tableLabel;
                if (lineInfo.kind == MarkdownLineKind::TableRow || lineInfo.kind == MarkdownLineKind::TableSeparator)
                {
                    int columnIndex = -1;
                    if (!lineInfo.tableCells.empty())
                    {
                        for (std::size_t i = 0; i < lineInfo.tableCells.size(); ++i)
                        {
                            const auto &cell = lineInfo.tableCells[i];
                            auto endCol = std::max(cell.endColumn, cell.startColumn + 1);
                            if (editor->curPos.x >= static_cast<int>(cell.startColumn) &&
                                editor->curPos.x < static_cast<int>(endCol))
                            {
                                columnIndex = static_cast<int>(i);
                                break;
                            }
                        }
                        if (columnIndex == -1)
                            columnIndex = static_cast<int>(lineInfo.tableCells.size()) - 1;
                    }
                    if (columnIndex >= 0)
                        tableLabel = sanitizeMultiline(
                            editor->analyzer().describeTableCell(lineInfo, static_cast<std::size_t>(columnIndex)));
                }

                const auto *span = editor->analyzer().spanAtColumn(lineInfo, editor->curPos.x);
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
                    info.displayLabel = tableLabel;
                }
                else if (span)
                {
                    info.displayLabel = sanitizeMultiline(editor->analyzer().describeSpan(*span));
                }
            }
            info.hasLine = true;
            linePtr = editor->nextLine(linePtr);
        }
        lines.push_back(info);
        ++lineNumber;
    }

    std::optional<std::string> labelBeforeView;
    if (topPtr > 0 && editor->bufLen > 0)
    {
        uint prevPtr = editor->lineMove(topPtr, -1);
        if (prevPtr < editor->bufLen)
        {
            MarkdownParserState prevState = computeState(prevPtr);
            MarkdownLineInfo prevInfo = editor->analyzer().analyzeLine(editor->lineText(prevPtr), prevState);
            labelBeforeView = editor->analyzer().describeLine(prevInfo);
        }
    }

    std::optional<std::string> labelAfterView;
    if (linePtr < editor->bufLen)
    {
        MarkdownParserState afterState = state;
        MarkdownLineInfo nextInfo = editor->analyzer().analyzeLine(editor->lineText(linePtr), afterState);
        labelAfterView = editor->analyzer().describeLine(nextInfo);
    }

    for (int row = 0; row < size.y; ++row)
    {
        TDrawBuffer buffer;
        const auto &line = lines[row];
        const bool isActiveRow = line.isActive;
        const TColorAttr &rowAttr = isActiveRow ? activeAttr : normalAttr;
        buffer.moveChar(0, ' ', rowAttr, size.x);

        if (line.hasLine)
        {
            const bool hasGroupLabel = !line.groupLabel.empty();
            bool hasPrevSame = false;
            bool hasNextSame = false;

            if (hasGroupLabel)
            {
                if (row > 0 && lines[row - 1].hasLine && lines[row - 1].groupLabel == line.groupLabel)
                    hasPrevSame = true;
                else if (row == 0 && labelBeforeView && *labelBeforeView == line.groupLabel)
                    hasPrevSame = true;

                if (row + 1 < size.y && lines[row + 1].hasLine && lines[row + 1].groupLabel == line.groupLabel)
                    hasNextSame = true;
                else if (row == size.y - 1 && labelAfterView && *labelAfterView == line.groupLabel)
                    hasNextSame = true;
            }

            if (hasPrevSame)
            {
                const char *connector = hasNextSame ? "│" : "└";
                buffer.moveStr(0, connector, rowAttr);
            }
            else if (hasGroupLabel)
            {
                buffer.moveCStr(0, line.displayLabel, isActiveRow ? activePair : normalPair, size.x);
            }
        }

        if (size.x > 0)
        {
            buffer.moveStr(size.x - 1, "│", rowAttr);
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

    if (fileEditor->modified)
        displayName.insert(0, "* ");

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
        fileEditor->setMarkdownMode(isMarkdownFile(newName));

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

void MarkdownEditWindow::draw()
{
    TWindow::draw();
    if (fileEditor && fileEditor->isMarkdownMode())
    {
        TDrawBuffer buffer;
        TColorAttr frameAttr = getColor(2);
        buffer.moveStr(0, "╤", frameAttr);
        writeLine(kInfoColumnWidth, 0, 1, 1, buffer);
        buffer.moveStr(0, "╧", frameAttr);
        writeLine(kInfoColumnWidth, size.y - 1, 1, 1, buffer);
    }
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

    std::thread([this, token]() {
        delay(3000);
        pendingStatusMessageClear.store(token, std::memory_order_release);
    }).detach();
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

void MarkdownEditorApp::idle()
{
    TApplication::idle();

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
}

} // namespace ck::edit
