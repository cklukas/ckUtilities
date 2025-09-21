#pragma once

#include "markdown_parser.hpp"

#define Uses_TWindow
#define Uses_TScrollBar
#define Uses_TIndicator
#define Uses_TView
#define Uses_TEditWindow
#define Uses_TFileEditor
#define Uses_TRect
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

#include <memory>
#include <string>
#include <climits>

namespace ck::edit
{

class MarkdownInfoView;

class MarkdownFileEditor : public TFileEditor
{
public:
    MarkdownFileEditor(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll,
                       TIndicator *indicator, TStringView fileName) noexcept;

    void setInfoView(MarkdownInfoView *view) noexcept { infoView = view; }
    void setMarkdownMode(bool value) noexcept { markdownMode = value; notifyInfoView(); }
    bool isMarkdownMode() const noexcept { return markdownMode; }

    void toggleWrap();
    bool isWrapEnabled() const noexcept { return wrapEnabled; }
    void toggleMarkdownMode();

    void applyHeadingLevel(int level);
    void clearHeading();
    void applyBold();
    void applyItalic();
    void applyBoldItalic();
    void removeFormatting();
    void applyBlockQuote();
    void removeBlockQuote();

    void insertBulletList(int count);
    void insertNumberedList(int count);
    void insertLink();
    void insertImage();
    void insertTable();
    void tableInsertRowAbove();
    void tableInsertRowBelow();
    void tableDeleteRow();
    void tableInsertColumnBefore();
    void tableInsertColumnAfter();
    void tableDeleteColumn();
    void tableDeleteTable();
    void tableAlignColumn(MarkdownTableAlignment alignment);

    virtual void handleEvent(TEvent &event) override;
    virtual void draw() override;

    MarkdownAnalyzer &analyzer() noexcept { return markdownAnalyzer; }
    uint topLinePointer();
    std::string lineText(uint linePtr);
    void notifyInfoView();
    uint stateVersion() const noexcept { return cachedStateVersion; }

private:
    friend class MarkdownInfoView;
    MarkdownInfoView *infoView = nullptr;
    MarkdownAnalyzer markdownAnalyzer;
    bool wrapEnabled = false;
    bool markdownMode = true;
    uint cachedStateVersion = 0;

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

private:
    MarkdownFileEditor *fileEditor = nullptr;
    MarkdownInfoView *infoView = nullptr;
    TScrollBar *hScrollBar = nullptr;
    TScrollBar *vScrollBar = nullptr;
    TIndicator *indicator = nullptr;
};

class MarkdownEditorApp : public TApplication
{
public:
    MarkdownEditorApp(int argc, char **argv);

    static TMenuBar *initMenuBar(TRect);
    static TStatusLine *initStatusLine(TRect);

    virtual void handleEvent(TEvent &event) override;

private:
    MarkdownEditWindow *openEditor(const char *fileName, Boolean visible);
    void fileOpen();
    void fileNew();
    void changeDir();
    void showAbout();
    void dispatchToEditor(ushort command);
};

} // namespace ck::edit

