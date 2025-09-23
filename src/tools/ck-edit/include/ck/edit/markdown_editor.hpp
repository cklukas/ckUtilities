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
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_THistory
#define Uses_TCheckBoxes
#define Uses_TSItem
#define Uses_TButton
#define Uses_TFindDialogRec
#define Uses_TReplaceDialogRec
#include <tvision/tv.h>

#include <atomic>
#include <memory>
#include <optional>
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
    bool smartListContinuation = true;
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

inline constexpr ushort cmToggleWrap = 3000;
inline constexpr ushort cmToggleMarkdownMode = 3001;
inline constexpr ushort cmHeading1 = 3010;
inline constexpr ushort cmHeading2 = 3011;
inline constexpr ushort cmHeading3 = 3012;
inline constexpr ushort cmHeading4 = 3013;
inline constexpr ushort cmHeading5 = 3014;
inline constexpr ushort cmHeading6 = 3015;
inline constexpr ushort cmClearHeading = 3016;
inline constexpr ushort cmMakeParagraph = 3017;
inline constexpr ushort cmInsertLineBreak = 3018;
inline constexpr ushort cmBold = 3020;
inline constexpr ushort cmItalic = 3021;
inline constexpr ushort cmBoldItalic = 3022;
inline constexpr ushort cmStrikethrough = 3023;
inline constexpr ushort cmInlineCode = 3024;
inline constexpr ushort cmCodeBlock = 3025;
inline constexpr ushort cmRemoveFormatting = 3026;
inline constexpr ushort cmToggleBlockQuote = 3030;
inline constexpr ushort cmToggleBulletList = 3031;
inline constexpr ushort cmToggleNumberedList = 3032;
inline constexpr ushort cmConvertTaskList = 3033;
inline constexpr ushort cmToggleTaskCheckbox = 3034;
inline constexpr ushort cmIncreaseIndent = 3035;
inline constexpr ushort cmDecreaseIndent = 3036;
inline constexpr ushort cmDefinitionList = 3037;
inline constexpr ushort cmInsertLink = 3040;
inline constexpr ushort cmInsertReferenceLink = 3041;
inline constexpr ushort cmAutoLinkSelection = 3042;
inline constexpr ushort cmInsertImage = 3043;
inline constexpr ushort cmInsertFootnote = 3044;
inline constexpr ushort cmInsertHorizontalRule = 3045;
inline constexpr ushort cmEscapeSelection = 3046;
inline constexpr ushort cmInsertTable = 3050;
inline constexpr ushort cmTableInsertRowAbove = 3051;
inline constexpr ushort cmTableInsertRowBelow = 3052;
inline constexpr ushort cmTableDeleteRow = 3053;
inline constexpr ushort cmTableInsertColumnBefore = 3054;
inline constexpr ushort cmTableInsertColumnAfter = 3055;
inline constexpr ushort cmTableDeleteColumn = 3056;
inline constexpr ushort cmTableDeleteTable = 3057;
inline constexpr ushort cmTableAlignDefault = 3058;
inline constexpr ushort cmTableAlignLeft = 3059;
inline constexpr ushort cmTableAlignCenter = 3060;
inline constexpr ushort cmTableAlignRight = 3061;
inline constexpr ushort cmTableAlignNumber = 3062;
inline constexpr ushort cmReflowParagraphs = 3070;
inline constexpr ushort cmFormatDocument = 3071;
inline constexpr ushort cmToggleSmartList = 3080;
inline constexpr ushort cmAbout = 3090;
inline constexpr ushort cmReturnToLauncher = 3091;

class MarkdownFileEditor : public TFileEditor
{
public:
    MarkdownFileEditor(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll,
                       TIndicator *indicator, TStringView fileName) noexcept;

    static bool isMarkdownFileName(std::string_view path);

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
    struct InlineCommandSpec
    {
        enum class CursorPlacement
        {
            AfterPrefix,
            AfterPlaceholder,
            AfterSuffix
        };

        std::string name;
        std::string prefix;
        std::string suffix;
        std::string placeholder;
        bool selectPlaceholder = false;
        bool keepSelection = true;
        CursorPlacement cursorPlacement = CursorPlacement::AfterPrefix;
    };

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
    void refreshCursorMetrics();
    int documentLineNumber() const noexcept;
    int documentColumnNumber() const noexcept;
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
    std::vector<int> pendingInfoLines;
    bool infoViewNeedsFullRefresh = false;
    bool lineNumberCacheValid = false;
    uint lineNumberCachePtr = 0;
    int lineNumberCacheNumber = 0;
    int cursorLineNumber = 0;
    int cursorColumnNumber = 0;
    int wrapTopSegmentOffset = 0;
    int wrapDesiredVisualColumn = -1;
    TPoint wrapCursorScreenPos {0, 0};

    void onContentModified();
    void queueInfoLine(int lineNumber);
    void queueInfoLineRange(int firstLine, int lastLine);
    void requestInfoViewFullRefresh();
    void clearInfoViewQueue();
    int lineNumberForPointer(uint pointer);
    uint pointerForLine(int lineNumber);
    void enqueuePendingInfoLine(int lineNumber);
    void resetLineNumberCache();
    int computeLineNumberForPointer(uint pointer);
    void applyInlineCommand(const InlineCommandSpec &spec);
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

    struct WrapSegment
    {
        int startColumn = 0;
        int endColumn = 0;
    };

    struct WrapLayout
    {
        std::vector<WrapSegment> segments;
        int lineColumns = 0;
    };

    void computeWrapLayout(uint linePtr, WrapLayout &layout);
    void computeWrapLayoutFromCells(const TScreenCell *cells, int lineColumns, int wrapWidth, WrapLayout &layout);
    int wrapSegmentForColumn(const WrapLayout &layout, int column) const;
    static void buildWordWrapSegments(const TScreenCell *cells, int lineColumns, int wrapWidth, std::vector<WrapSegment> &segments);
    int documentLineCount() const;
    int wrapSegmentCount(const WrapLayout &layout) const;
    WrapSegment segmentAt(const WrapLayout &layout, int index) const;
    void normalizeWrapTop(int &docLine, int &segmentOffset);
    int computeWrapCaretRow(int docLine, int segmentOffset, uint caretLinePtr, const WrapLayout &caretLayout, int caretSegment) const;
    void ensureWrapViewport(const WrapLayout &caretLayout, int caretSegment);
    void updateWrapCursorVisualPosition(const WrapLayout &caretLayout, int caretSegment);
    int currentWrapLocalColumn(const WrapLayout &layout, int segmentIndex) const;
    void updateWrapStateAfterMovement(bool preserveDesiredColumn);
    bool handleWrapKeyEvent(TEvent &event);
    void moveCaretVertically(int lines, uchar selectMode);
    bool moveCaretOneStep(int direction, uchar selectMode, int desiredColumn);
    bool moveCaretDownSegment(uint linePtr, const WrapLayout &layout, int segmentIndex, uchar selectMode, int desiredColumn);
    bool moveCaretToNextDocumentLine(uint linePtr, uchar selectMode, int desiredColumn);
    bool moveCaretUpSegment(uint linePtr, const WrapLayout &layout, int segmentIndex, uchar selectMode, int desiredColumn);
    bool moveCaretToPreviousDocumentLine(uint linePtr, uchar selectMode, int desiredColumn);

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

    void updateLines(const std::vector<int> &lineNumbers);

    void invalidateState() noexcept { cachedPrefixPtr = UINT_MAX; }
    void setEditor(MarkdownFileEditor *ed) noexcept { editor = ed; }

private:
    struct LineRenderInfo
    {
        bool hasLine = false;
        bool isActive = false;
        bool lineActive = false;
        MarkdownLineKind lineKind = MarkdownLineKind::Unknown;
        std::string displayLabel;
        std::string groupLabel;
        int lineNumber = -1;
        int visualRowIndex = 0;
        int visualRowCount = 1;
    };

    struct LineGroupCache
    {
        int lineNumber = -1;
        int firstRow = 0;
        int visibleRows = 0;
        int totalRows = 0;
        int activeRow = -1;
    };

    MarkdownFileEditor *editor;
    MarkdownParserState cachedState{};
    uint cachedPrefixPtr = UINT_MAX;
    uint cachedVersion = 0;
    std::vector<LineRenderInfo> cachedLines;
    std::vector<LineGroupCache> cachedGroups;
    int cachedTopLineNumber = -1;
    std::optional<std::string> cachedLabelBeforeView;
    std::optional<std::string> cachedLabelAfterView;
    bool cacheValid = false;

    MarkdownParserState computeState(uint topPtr);
    LineRenderInfo buildLineInfo(uint linePtr, int lineNumber);
    LineRenderInfo buildLineInfo(uint linePtr, int lineNumber, MarkdownParserState &state);
    void rebuildCache();
    void renderRow(int row);
    void refreshBoundaryLabels(uint topPtr, uint linePtrAfterView);
    static std::string filterLabel(const std::string &label);
};

class MarkdownEditWindow : public TWindow
{
public:
    MarkdownEditWindow(const TRect &bounds, TStringView fileName, int aNumber) noexcept;
    MarkdownFileEditor *editor() noexcept { return fileEditor; }
    void updateLayoutForMode();
    void updateWindowTitle();
    bool saveDocument(bool forceSaveAs);

    virtual void setState(ushort aState, Boolean enable) override;
    virtual void handleEvent(TEvent &event) override;

    void refreshDivider();

private:
    MarkdownFileEditor *fileEditor = nullptr;
    MarkdownInfoView *infoView = nullptr;
    TScrollBar *hScrollBar = nullptr;
    TScrollBar *vScrollBar = nullptr;
    TIndicator *indicator = nullptr;

    void applyWindowTitle(const std::string &titleText);

protected:
    static TFrame *initFrame(TRect bounds);
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
