#pragma once

#include "markdown_parser.hpp"
#include "ck/app_info.hpp"

#define Uses_TWindow
#define Uses_TFrame
#define Uses_TScrollBar
#define Uses_TIndicator
#define Uses_TView
#define Uses_TEditWindow
#define Uses_TFileEditor
#define Uses_TRect
#define Uses_TMenu
#define Uses_TEvent
#define Uses_TPoint
#define Uses_TDrawBuffer
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TSubMenu
#define Uses_TStatusLine
#define Uses_TStatusItem
#define Uses_TStatusDef
#define Uses_TDeskTop
#define Uses_TFileDialog
#define Uses_TChDirDialog
#define Uses_TCommandSet
#define Uses_TApplication
#define Uses_MsgBox
#define Uses_TKeys
#define Uses_TProgram
#define Uses_TDialog
#define Uses_TObject
#include <tvision/tv.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <climits>

namespace ck::edit
{

inline constexpr std::string_view kAppId = "ck-edit";

inline std::string_view appName()
{
    return ck::appinfo::requireTool(kAppId).executable;
}

inline std::string_view appShortDescription()
{
    return ck::appinfo::requireTool(kAppId).shortDescription;
}

inline std::string_view appAboutDescription()
{
    return ck::appinfo::requireTool(kAppId).aboutDescription;
}

class MarkdownInfoView;
class MarkdownEditWindow;
class MarkdownEditorApp;

struct MarkdownStatusContext
{
    bool hasEditor = false;
    bool markdownMode = false;
    bool hasFileName = false;
    bool isUntitled = false;
    bool isModified = false;
    bool hasCursorLine = false;
    MarkdownLineKind lineKind = MarkdownLineKind::Unknown;
    int headingLevel = 0;
    bool isTaskItem = false;
    bool isOrderedItem = false;
    bool isBulletItem = false;
    bool isTableRow = false;
    bool isTableHeader = false;
    bool isTableSeparator = false;
    int tableColumn = -1;
    MarkdownTableAlignment tableAlignment = MarkdownTableAlignment::Default;
    bool tableHasAlignment = false;
    MarkdownSpanKind spanKind = MarkdownSpanKind::PlainText;
    bool hasSpan = false;
};

class MarkdownFileEditor : public TFileEditor
{
public:
    MarkdownFileEditor(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll,
                       TIndicator *indicator, TStringView fileName) noexcept;

    void setInfoView(MarkdownInfoView *view) noexcept { infoView = view; }
    void setHostWindow(MarkdownEditWindow *window) noexcept { hostWindow = window; }
    void setMarkdownMode(bool value) noexcept;
    bool isMarkdownMode() const noexcept { return markdownMode; }

    void toggleWrap();
    bool isWrapEnabled() const noexcept { return wrapEnabled; }
    void toggleMarkdownMode();

    void applyHeadingLevel(int level);
    void clearHeading();
    void makeParagraph();
    void insertLineBreak();
    void applyBold();
    void applyItalic();
    void applyBoldItalic();
    void applyStrikethrough();
    void applyInlineCode();
    void toggleCodeBlock();
    void removeFormatting();
    void applyBlockQuote();
    void removeBlockQuote();
    void toggleBlockQuote();

    void insertBulletList(int count);
    void insertNumberedList(int count);
    void insertLink();
    void insertReferenceLink();
    void autoLinkSelection();
    void insertImage();
    void insertFootnote();
    void insertHorizontalRule();
    void escapeSelection();
    void insertTable();
    void tableInsertRowAbove();
    void tableInsertRowBelow();
    void tableDeleteRow();
    void tableInsertColumnBefore();
    void tableInsertColumnAfter();
    void tableDeleteColumn();
    void tableDeleteTable();
    void tableAlignColumn(MarkdownTableAlignment alignment);
    void toggleBulletList();
    void toggleNumberedList();
    void convertToTaskList();
    void toggleTaskCheckbox();
    void increaseIndent();
    void decreaseIndent();
    void convertToDefinitionList();
    void reflowParagraphs();
    void formatDocument();
    void toggleSmartListContinuation();
    bool isSmartListContinuationEnabled() const noexcept { return smartListContinuation; }

    virtual void handleEvent(TEvent &event) override;
    virtual void draw() override;

    MarkdownAnalyzer &analyzer() noexcept { return markdownAnalyzer; }
    uint topLinePointer();
    std::string lineText(uint linePtr);
    void notifyInfoView();
    uint stateVersion() const noexcept { return cachedStateVersion; }
    void buildStatusContext(struct MarkdownStatusContext &context);

private:
    friend class MarkdownInfoView;
    MarkdownInfoView *infoView = nullptr;
    MarkdownEditWindow *hostWindow = nullptr;
    MarkdownAnalyzer markdownAnalyzer;
    bool wrapEnabled = false;
    bool markdownMode = true;
    bool smartListContinuation = true;
    uint cachedStateVersion = 0;

    MarkdownParserState statusStateCache{};
    uint statusCachePrefixPtr = UINT_MAX;
    uint statusCacheVersion = 0;

    void onContentModified();
    void wrapSelectionWith(const std::string &prefix, const std::string &suffix);
    void removeFormattingAround(uint start, uint end);
    bool ensureSelection();
    std::string readRange(uint start, uint end);
    void replaceRange(uint start, uint end, const std::string &text);
    void indentRangeWith(const std::string &prefix);
    void unindentBlockQuote();
    void insertListItems(int count, bool ordered);
    void insertRichInline(const std::string &prefix, const std::string &suffix, const std::string &placeholder);
    int promptForCount(const char *title);
    std::string promptForText(const char *title, const char *label, const std::string &initial = {});
    int promptForNumeric(const char *title, const char *label, int defaultValue, int minValue, int maxValue);
    bool continueListOnEnter(TEvent &event);

    struct BlockSelection
    {
        uint start = 0;
        uint end = 0;
        std::vector<std::string> lines;
        bool trailingNewline = false;
    };

    struct LinePattern
    {
        std::string indent;
        std::string blockquote;
        std::size_t markerStart = 0;
        std::size_t markerEnd = 0;
        bool hasBullet = false;
        bool hasOrdered = false;
        bool hasTask = false;
        char bulletChar = '-';
    };

    BlockSelection captureSelectedLines();
    void applyBlockSelection(const BlockSelection &selection, const std::vector<std::string> &lines, bool trailingNewline);
    static std::string trimLeft(std::string_view text);
    static std::string trim(std::string_view text);
    static bool lineIsWhitespace(const std::string &line);
    LinePattern analyzeLinePattern(const std::string &line) const;
    std::string generateUniqueReferenceId(const std::string &prefix);
    std::string generateUniqueFootnoteId();
    void appendDefinition(const std::string &definition);

    struct TableContext
    {
        enum class ActiveRow
        {
            None,
            Header,
            Separator,
            Body
        };

        bool valid = false;
        uint headerPtr = UINT_MAX;
        uint separatorPtr = UINT_MAX;
        std::vector<uint> bodyPtrs;
        MarkdownLineInfo headerInfo;
        MarkdownLineInfo separatorInfo;
        std::vector<MarkdownLineInfo> bodyInfos;
        ActiveRow activeRow = ActiveRow::None;
        uint activePtr = UINT_MAX;
        MarkdownLineInfo activeInfo;
        int activeColumn = -1;

        int columnCount() const noexcept;
    };

    bool locateTableContext(TableContext &context);
    std::string makeTableRow(const std::vector<std::string> &cells) const;
    std::string makeTableAlignmentRow(int columnCount, const std::vector<MarkdownTableAlignment> &alignments) const;
    std::string alignmentMarker(MarkdownTableAlignment alignment) const;
    void insertTableRow(TableContext &context, bool below);
    void insertTableColumn(TableContext &context, bool after);
    void alignTableColumn(TableContext &context, MarkdownTableAlignment alignment);
};

class MarkdownInfoView : public TView
{
public:
    explicit MarkdownInfoView(const TRect &bounds, MarkdownFileEditor *editor) noexcept;

    virtual void draw() override;
    virtual TPalette &getPalette() const override;

    void invalidateState() noexcept { cachedPrefixPtr = UINT_MAX; }
    void setEditor(MarkdownFileEditor *ed) noexcept { editor = ed; }

private:
    MarkdownFileEditor *editor;
    MarkdownParserState cachedState{};
    uint cachedPrefixPtr = UINT_MAX;
    uint cachedVersion = 0;

    MarkdownParserState computeState(uint topPtr);
    void renderLine(TDrawBuffer &buffer, int row, uint linePtr, int lineNumber,
                    const MarkdownParserState &stateSnapshot);
};

class MarkdownEditWindow : public TWindow
{
public:
    MarkdownEditWindow(const TRect &bounds, TStringView fileName, int aNumber) noexcept;
    MarkdownFileEditor *editor() noexcept { return fileEditor; }
    void updateLayoutForMode();
    void updateWindowTitle();
    bool saveDocument(bool forceSaveAs);

    virtual void draw() override;
    virtual void setState(ushort aState, Boolean enable) override;
    virtual void handleEvent(TEvent &event) override;

private:
    MarkdownFileEditor *fileEditor = nullptr;
    MarkdownInfoView *infoView = nullptr;
    TScrollBar *hScrollBar = nullptr;
    TScrollBar *vScrollBar = nullptr;
    TIndicator *indicator = nullptr;

    void applyWindowTitle(const std::string &titleText);
};

class MarkdownEditorApp : public TApplication
{
public:
    MarkdownEditorApp(int argc, char **argv);

    static TMenuBar *initMenuBar(TRect);
    static TStatusLine *initStatusLine(TRect);

    virtual void handleEvent(TEvent &event) override;
    virtual void idle() override;

    void updateStatusLine(const MarkdownStatusContext &context);
    void updateMenuBarForMode(bool markdownMode);
    void refreshUiMode();
    void showDocumentSavedMessage(const std::string &path);

private:
    MarkdownEditWindow *openEditor(const char *fileName, Boolean visible);
    void fileOpen();
    void fileNew();
    void changeDir();
    void showAbout();
    void dispatchToEditor(ushort command);
    void clearStatusMessage();

    std::atomic<uint32_t> statusMessageCounter = 0;
    std::atomic<uint32_t> activeStatusMessageToken = 0;
    std::atomic<uint32_t> pendingStatusMessageClear = 0;
};

} // namespace ck::edit
