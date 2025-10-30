#include "ck/find/search_dialogs.hpp"

#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/dialog_utils.hpp"
#include "ck/find/search_model.hpp"
#include "ck/ui/tab_control.hpp"

#include "command_ids.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#define Uses_MsgBox
#define Uses_TButton
#define Uses_TCheckBoxes
#define Uses_TChDirDialog
#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TRadioButtons
#define Uses_TMessageBox
#define Uses_TProgram
#define Uses_TStaticText
#include <tvision/tv.h>

namespace ck::find
{

bool editTextOptions(TextSearchOptions &options);
bool editNamePathOptions(NamePathOptions &options);
bool editTimeFilters(TimeFilterOptions &options);
bool editSizeFilters(SizeFilterOptions &options);
bool editTypeFilters(TypeFilterOptions &options);
bool editPermissionOwnership(PermissionOwnershipOptions &options);
bool editTraversalFilters(TraversalFilesystemOptions &options);
bool editActionOptions(ActionOptions &options);

namespace
{

constexpr unsigned short kGeneralRecursiveBit = 0x0001;
constexpr unsigned short kGeneralHiddenBit = 0x0002;
constexpr unsigned short kGeneralSymlinkBit = 0x0004;
constexpr unsigned short kGeneralStayOnFsBit = 0x0008;

constexpr unsigned short kOptionTextBit = 0x0001;
constexpr unsigned short kOptionNamePathBit = 0x0002;
constexpr unsigned short kOptionTimeBit = 0x0004;
constexpr unsigned short kOptionSizeBit = 0x0008;
constexpr unsigned short kOptionTypeBit = 0x0010;

constexpr unsigned short kOptionPermissionBit = 0x0001;
constexpr unsigned short kOptionTraversalBit = 0x0002;
constexpr unsigned short kOptionActionBit = 0x0004;

struct SearchNotebookState
{
    char specName[128]{};
    char startLocation[PATH_MAX]{};
    char searchText[256]{};
    char includePatterns[256]{};
    char excludePatterns[256]{};
    unsigned short generalFlags = 0;
    unsigned short optionPrimaryFlags = 0;
    unsigned short optionSecondaryFlags = 0;
    unsigned short quickSearchMode = 2; // 0 = contents, 1 = names, 2 = both
    unsigned short quickTypePreset = 0; // 0 = all, 1 = documents, 2 = images, 3 = audio, 4 = archives, 5 = custom
};

class QuickStartPage : public ck::ui::TabPageView
{
public:
    QuickStartPage(const TRect &bounds, SearchNotebookState &state);

    void onActivated() override;
    void populateFromState();
    void collect();
    void setStartLocation(const char *path);
    const char *startLocation() const noexcept;
    void syncOptionFlags();

private:
    SearchNotebookState &m_state;
    TInputLine *m_specNameInput = nullptr;
    TInputLine *m_startInput = nullptr;
    TInputLine *m_searchTextInput = nullptr;
    TInputLine *m_includeInput = nullptr;
    TInputLine *m_excludeInput = nullptr;
    TCheckBoxes *m_generalBoxes = nullptr;
    TCheckBoxes *m_primaryBoxes = nullptr;
    TCheckBoxes *m_secondaryBoxes = nullptr;
    TRadioButtons *m_searchModeButtons = nullptr;
    TRadioButtons *m_typePresetButtons = nullptr;
};

class ContentNamesPage : public ck::ui::TabPageView
{
public:
    ContentNamesPage(const TRect &bounds, TextSearchOptions &textOptions, NamePathOptions &nameOptions);

    void populate();
    void collect();

private:
    TextSearchOptions &m_textOptions;
    NamePathOptions &m_nameOptions;
    TRadioButtons *m_textModeButtons = nullptr;
    TCheckBoxes *m_textFlagBoxes = nullptr;
    TCheckBoxes *m_matcherBoxes = nullptr;
    TInputLine *m_nameInput = nullptr;
    TInputLine *m_inameInput = nullptr;
    TInputLine *m_pathInput = nullptr;
    TInputLine *m_ipathInput = nullptr;
    TInputLine *m_regexInput = nullptr;
    TInputLine *m_iregexInput = nullptr;
    TInputLine *m_lnameInput = nullptr;
    TInputLine *m_ilnameInput = nullptr;
    TCheckBoxes *m_pruneFlags = nullptr;
    TRadioButtons *m_pruneModeButtons = nullptr;
    TInputLine *m_pruneInput = nullptr;
};

QuickStartPage::QuickStartPage(const TRect &bounds, SearchNotebookState &state)
    : ck::ui::TabPageView(bounds),
      m_state(state)
{
    m_specNameInput = new TInputLine(TRect(2, 1, 60, 2), sizeof(m_state.specName) - 1);
    insert(new TLabel(TRect(1, 0, 18, 1), "~N~ame:", m_specNameInput));
    insert(m_specNameInput);

    insert(new TStaticText(TRect(2, 2, 78, 4),
                           "Choose a starting folder and optional patterns.\n"
                           "Use other tabs for advanced filters."));

    m_startInput = new TInputLine(TRect(2, 4, 60, 5), sizeof(m_state.startLocation) - 1);
    insert(new TLabel(TRect(1, 3, 27, 4), "Start ~L~ocation:", m_startInput));
    insert(m_startInput);
    insert(new TButton(TRect(61, 4, 77, 6), "~B~rowse...", cmBrowseStart, bfNormal));

    m_searchTextInput = new TInputLine(TRect(2, 6, 77, 7), sizeof(m_state.searchText) - 1);
    insert(new TLabel(TRect(1, 5, 25, 6), "~S~earch text:", m_searchTextInput));
    insert(m_searchTextInput);

    m_searchModeButtons = new TRadioButtons(TRect(2, 7, 30, 11),
                                            makeItemList({"Search ~c~ontents",
                                                          "Search ~n~ames only",
                                                          "Search ~b~oth"}));
    insert(m_searchModeButtons);

    m_includeInput = new TInputLine(TRect(2, 8, 38, 9), sizeof(m_state.includePatterns) - 1);
    insert(new TLabel(TRect(1, 7, 28, 8), "~I~nclude patterns:", m_includeInput));
    insert(m_includeInput);

    m_excludeInput = new TInputLine(TRect(40, 8, 77, 9), sizeof(m_state.excludePatterns) - 1);
    insert(new TLabel(TRect(39, 7, 76, 8), "~E~xclude patterns:", m_excludeInput));
    insert(m_excludeInput);

    m_generalBoxes = new TCheckBoxes(TRect(32, 7, 62, 12),
                                     makeItemList({"~R~ecursive",
                                                   "Include ~h~idden",
                                                   "Follow s~y~mlinks",
                                                   "Stay on same file ~s~ystem"}));
    insert(m_generalBoxes);

    m_primaryBoxes = new TCheckBoxes(TRect(2, 12, 30, 17),
                                     makeItemList({"~T~ext search",
                                                   "Name/~P~ath",
                                                   "~T~ime filters",
                                                   "Si~z~e filters",
                                                   "File ~t~ype filters"}));
    insert(m_primaryBoxes);

    m_secondaryBoxes = new TCheckBoxes(TRect(32, 12, 51, 17),
                                       makeItemList({"~P~ermissions",
                                                     "T~r~aversal",
                                                     "~A~ctions"}));
    insert(m_secondaryBoxes);

    m_typePresetButtons = new TRadioButtons(TRect(53, 12, 77, 17),
                                            makeItemList({"All ~f~iles",
                                                          "~D~ocuments",
                                                          "~I~mages",
                                                          "~A~udio",
                                                          "~R~chives",
                                                          "~C~ustom"}));
    insert(m_typePresetButtons);
    insert(new TLabel(TRect(53, 11, 77, 12), "Type ~Y~preset:", m_typePresetButtons));

    insert(new TButton(TRect(2, 18, 22, 20), "Adva~n~ced filters...", cmTabContentNames, bfNormal));
    insert(new TButton(TRect(24, 18, 40, 20), "Text ~O~ptions...", cmTextOptions, bfNormal));
    insert(new TButton(TRect(42, 18, 58, 20), "Name/~P~ath...", cmNamePathOptions, bfNormal));
    insert(new TButton(TRect(60, 18, 76, 20), "Time ~T~ests...", cmTimeFilters, bfNormal));

    populateFromState();
}

void QuickStartPage::onActivated()
{
    if (m_specNameInput)
        m_specNameInput->selectAll(True, True);
}

void QuickStartPage::populateFromState()
{
    if (m_specNameInput)
        m_specNameInput->setData(m_state.specName);
    if (m_startInput)
        m_startInput->setData(m_state.startLocation);
    if (m_searchTextInput)
        m_searchTextInput->setData(m_state.searchText);
    if (m_includeInput)
        m_includeInput->setData(m_state.includePatterns);
    if (m_excludeInput)
        m_excludeInput->setData(m_state.excludePatterns);
    if (m_searchModeButtons)
        m_searchModeButtons->setData(&m_state.quickSearchMode);
    if (m_typePresetButtons)
        m_typePresetButtons->setData(&m_state.quickTypePreset);
    syncOptionFlags();
}

void QuickStartPage::collect()
{
    if (m_specNameInput)
        m_specNameInput->getData(m_state.specName);
    if (m_startInput)
        m_startInput->getData(m_state.startLocation);
    if (m_searchTextInput)
        m_searchTextInput->getData(m_state.searchText);
    if (m_includeInput)
        m_includeInput->getData(m_state.includePatterns);
    if (m_excludeInput)
        m_excludeInput->getData(m_state.excludePatterns);

    if (m_generalBoxes)
    {
        unsigned short flags = m_state.generalFlags;
        m_generalBoxes->getData(&flags);
        m_state.generalFlags = flags;
    }
    if (m_primaryBoxes)
    {
        unsigned short flags = m_state.optionPrimaryFlags;
        m_primaryBoxes->getData(&flags);
        m_state.optionPrimaryFlags = flags;
    }
    if (m_secondaryBoxes)
    {
        unsigned short flags = m_state.optionSecondaryFlags;
        m_secondaryBoxes->getData(&flags);
        m_state.optionSecondaryFlags = flags;
    }
    if (m_searchModeButtons)
        m_searchModeButtons->getData(&m_state.quickSearchMode);
    if (m_typePresetButtons)
        m_typePresetButtons->getData(&m_state.quickTypePreset);

    if (m_state.searchText[0] != '\0')
        m_state.optionPrimaryFlags |= kOptionTextBit;
    if (m_state.quickTypePreset == 0)
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionTypeBit);
    else if (m_state.quickTypePreset != 5)
        m_state.optionPrimaryFlags |= kOptionTypeBit;
}

void QuickStartPage::setStartLocation(const char *path)
{
    if (!path)
        return;
    std::snprintf(m_state.startLocation, sizeof(m_state.startLocation), "%s", path);
    if (m_startInput)
        m_startInput->setData(m_state.startLocation);
}

const char *QuickStartPage::startLocation() const noexcept
{
    return m_state.startLocation;
}

void QuickStartPage::syncOptionFlags()
{
    if (m_generalBoxes)
    {
        unsigned short flags = m_state.generalFlags;
        m_generalBoxes->setData(&flags);
    }
    if (m_primaryBoxes)
    {
        unsigned short flags = m_state.optionPrimaryFlags;
        m_primaryBoxes->setData(&flags);
    }
    if (m_secondaryBoxes)
    {
        unsigned short flags = m_state.optionSecondaryFlags;
        m_secondaryBoxes->setData(&flags);
    }
    if (m_searchModeButtons)
        m_searchModeButtons->setData(&m_state.quickSearchMode);
    if (m_typePresetButtons)
        m_typePresetButtons->setData(&m_state.quickTypePreset);
}

ContentNamesPage::ContentNamesPage(const TRect &bounds, TextSearchOptions &textOptions, NamePathOptions &nameOptions)
    : ck::ui::TabPageView(bounds),
      m_textOptions(textOptions),
      m_nameOptions(nameOptions)
{
    m_textModeButtons = new TRadioButtons(TRect(2, 1, 30, 5),
                                          makeItemList({"Contains te~x~t",
                                                        "Match ~w~hole word",
                                                        "Regular ~e~xpression"}));
    insert(m_textModeButtons);

    m_textFlagBoxes = new TCheckBoxes(TRect(32, 1, 58, 6),
                                      makeItemList({"~M~atch case",
                                                    "Search file ~c~ontents",
                                                    "Search file ~n~ames",
                                                    "Allow ~m~ultiple terms",
                                                    "Treat ~b~inary as text"}));
    insert(m_textFlagBoxes);

    insert(new TStaticText(TRect(2, 6, 78, 7), "Name and path filters"));

    m_matcherBoxes = new TCheckBoxes(TRect(2, 7, 28, 15),
                                     makeItemList({"~N~ame",
                                                   "Case-insensitive ~n~ame",
                                                   "~P~ath",
                                                   "Case-insensitive pa~t~h",
                                                   "Regular e~x~pression",
                                                   "Case-insensitive re~g~ex",
                                                   "Symlink ~l~name",
                                                   "Case-insensitive l~n~ame"}));
    insert(m_matcherBoxes);

    m_nameInput = new TInputLine(TRect(30, 7, 55, 8), sizeof(m_nameOptions.namePattern) - 1);
    insert(new TLabel(TRect(30, 6, 55, 7), "~N~ame pattern:", m_nameInput));
    insert(m_nameInput);

    m_inameInput = new TInputLine(TRect(57, 7, 78, 8), sizeof(m_nameOptions.inamePattern) - 1);
    insert(new TLabel(TRect(57, 6, 78, 7), "Case-insensitive ~n~ame:", m_inameInput));
    insert(m_inameInput);

    m_pathInput = new TInputLine(TRect(30, 8, 55, 9), sizeof(m_nameOptions.pathPattern) - 1);
    insert(new TLabel(TRect(30, 7, 55, 8), "~P~ath glob:", m_pathInput));
    insert(m_pathInput);

    m_ipathInput = new TInputLine(TRect(57, 8, 78, 9), sizeof(m_nameOptions.ipathPattern) - 1);
    insert(new TLabel(TRect(57, 7, 78, 8), "Case-insensitive pa~t~h:", m_ipathInput));
    insert(m_ipathInput);

    m_regexInput = new TInputLine(TRect(30, 9, 55, 10), sizeof(m_nameOptions.regexPattern) - 1);
    insert(new TLabel(TRect(30, 8, 55, 9), "Re~g~ex:", m_regexInput));
    insert(m_regexInput);

    m_iregexInput = new TInputLine(TRect(57, 9, 78, 10), sizeof(m_nameOptions.iregexPattern) - 1);
    insert(new TLabel(TRect(57, 8, 78, 9), "Case-insensitive re~g~ex:", m_iregexInput));
    insert(m_iregexInput);

    m_lnameInput = new TInputLine(TRect(30, 10, 55, 11), sizeof(m_nameOptions.lnamePattern) - 1);
    insert(new TLabel(TRect(30, 9, 55, 10), "Symlink ~l~name:", m_lnameInput));
    insert(m_lnameInput);

    m_ilnameInput = new TInputLine(TRect(57, 10, 78, 11), sizeof(m_nameOptions.ilnamePattern) - 1);
    insert(new TLabel(TRect(57, 9, 78, 10), "Case-insensitive l~n~ame:", m_ilnameInput));
    insert(m_ilnameInput);

    insert(new TStaticText(TRect(2, 15, 78, 16), "Prune matching directories"));

    m_pruneFlags = new TCheckBoxes(TRect(2, 16, 16, 18),
                                   makeItemList({"Enable -p~r~une",
                                                 "Directories ~o~nly"}));
    insert(m_pruneFlags);

    m_pruneModeButtons = new TRadioButtons(TRect(18, 16, 54, 20),
                                           makeItemList({"Use -name",
                                                         "Use -iname",
                                                         "Use -path",
                                                         "Use -ipath",
                                                         "Use -regex",
                                                         "Use -iregex"}));
    insert(m_pruneModeButtons);

    m_pruneInput = new TInputLine(TRect(56, 16, 78, 17), sizeof(m_nameOptions.prunePattern) - 1);
    insert(new TLabel(TRect(56, 15, 78, 16), "Pattern:", m_pruneInput));
    insert(m_pruneInput);

    populate();
}

void ContentNamesPage::populate()
{
    unsigned short mode = static_cast<unsigned short>(m_textOptions.mode);
    if (m_textModeButtons)
        m_textModeButtons->setData(&mode);

    unsigned short textFlags = 0;
    if (m_textOptions.matchCase)
        textFlags |= 0x0001;
    if (m_textOptions.searchInContents)
        textFlags |= 0x0002;
    if (m_textOptions.searchInFileNames)
        textFlags |= 0x0004;
    if (m_textOptions.allowMultipleTerms)
        textFlags |= 0x0008;
    if (m_textOptions.treatBinaryAsText)
        textFlags |= 0x0010;
    if (m_textFlagBoxes)
        m_textFlagBoxes->setData(&textFlags);

    unsigned short matcherFlags = 0;
    if (m_nameOptions.nameEnabled)
        matcherFlags |= 0x0001;
    if (m_nameOptions.inameEnabled)
        matcherFlags |= 0x0002;
    if (m_nameOptions.pathEnabled)
        matcherFlags |= 0x0004;
    if (m_nameOptions.ipathEnabled)
        matcherFlags |= 0x0008;
    if (m_nameOptions.regexEnabled)
        matcherFlags |= 0x0010;
    if (m_nameOptions.iregexEnabled)
        matcherFlags |= 0x0020;
    if (m_nameOptions.lnameEnabled)
        matcherFlags |= 0x0040;
    if (m_nameOptions.ilnameEnabled)
        matcherFlags |= 0x0080;
    if (m_matcherBoxes)
        m_matcherBoxes->setData(&matcherFlags);

    if (m_nameInput)
        m_nameInput->setData(m_nameOptions.namePattern.data());
    if (m_inameInput)
        m_inameInput->setData(m_nameOptions.inamePattern.data());
    if (m_pathInput)
        m_pathInput->setData(m_nameOptions.pathPattern.data());
    if (m_ipathInput)
        m_ipathInput->setData(m_nameOptions.ipathPattern.data());
    if (m_regexInput)
        m_regexInput->setData(m_nameOptions.regexPattern.data());
    if (m_iregexInput)
        m_iregexInput->setData(m_nameOptions.iregexPattern.data());
    if (m_lnameInput)
        m_lnameInput->setData(m_nameOptions.lnamePattern.data());
    if (m_ilnameInput)
        m_ilnameInput->setData(m_nameOptions.ilnamePattern.data());

    unsigned short pruneFlags = 0;
    if (m_nameOptions.pruneEnabled)
        pruneFlags |= 0x0001;
    if (m_nameOptions.pruneDirectoriesOnly)
        pruneFlags |= 0x0002;
    if (m_pruneFlags)
        m_pruneFlags->setData(&pruneFlags);

    unsigned short pruneMode = static_cast<unsigned short>(m_nameOptions.pruneTest);
    if (m_pruneModeButtons)
        m_pruneModeButtons->setData(&pruneMode);

    if (m_pruneInput)
        m_pruneInput->setData(m_nameOptions.prunePattern.data());
}

void ContentNamesPage::collect()
{
    unsigned short mode = 0;
    if (m_textModeButtons)
    {
        m_textModeButtons->getData(&mode);
        m_textOptions.mode = static_cast<TextSearchOptions::Mode>(mode);
    }

    unsigned short textFlags = 0;
    if (m_textFlagBoxes)
        m_textFlagBoxes->getData(&textFlags);
    m_textOptions.matchCase = (textFlags & 0x0001) != 0;
    m_textOptions.searchInContents = (textFlags & 0x0002) != 0;
    m_textOptions.searchInFileNames = (textFlags & 0x0004) != 0;
    m_textOptions.allowMultipleTerms = (textFlags & 0x0008) != 0;
    m_textOptions.treatBinaryAsText = (textFlags & 0x0010) != 0;

    unsigned short matcherFlags = 0;
    if (m_matcherBoxes)
        m_matcherBoxes->getData(&matcherFlags);
    m_nameOptions.nameEnabled = (matcherFlags & 0x0001) != 0;
    m_nameOptions.inameEnabled = (matcherFlags & 0x0002) != 0;
    m_nameOptions.pathEnabled = (matcherFlags & 0x0004) != 0;
    m_nameOptions.ipathEnabled = (matcherFlags & 0x0008) != 0;
    m_nameOptions.regexEnabled = (matcherFlags & 0x0010) != 0;
    m_nameOptions.iregexEnabled = (matcherFlags & 0x0020) != 0;
    m_nameOptions.lnameEnabled = (matcherFlags & 0x0040) != 0;
    m_nameOptions.ilnameEnabled = (matcherFlags & 0x0080) != 0;

    if (m_nameInput)
        m_nameInput->getData(m_nameOptions.namePattern.data());
    if (m_inameInput)
        m_inameInput->getData(m_nameOptions.inamePattern.data());
    if (m_pathInput)
        m_pathInput->getData(m_nameOptions.pathPattern.data());
    if (m_ipathInput)
        m_ipathInput->getData(m_nameOptions.ipathPattern.data());
    if (m_regexInput)
        m_regexInput->getData(m_nameOptions.regexPattern.data());
    if (m_iregexInput)
        m_iregexInput->getData(m_nameOptions.iregexPattern.data());
    if (m_lnameInput)
        m_lnameInput->getData(m_nameOptions.lnamePattern.data());
    if (m_ilnameInput)
        m_ilnameInput->getData(m_nameOptions.ilnamePattern.data());

    unsigned short pruneFlags = 0;
    if (m_pruneFlags)
        m_pruneFlags->getData(&pruneFlags);
    m_nameOptions.pruneEnabled = (pruneFlags & 0x0001) != 0;
    m_nameOptions.pruneDirectoriesOnly = (pruneFlags & 0x0002) != 0;

    unsigned short pruneMode = 0;
    if (m_pruneModeButtons)
        m_pruneModeButtons->getData(&pruneMode);
    m_nameOptions.pruneTest = static_cast<NamePathOptions::PruneTest>(pruneMode);

    if (m_pruneInput)
        m_pruneInput->getData(m_nameOptions.prunePattern.data());
}

class SearchNotebookDialog : public TDialog
{
public:
    SearchNotebookDialog(SearchSpecification &spec, SearchNotebookState &state);

protected:
    void handleEvent(TEvent &event) override;
    Boolean valid(ushort command) override;

private:
    void browseStartLocation();
    void applyStateToSpecification();
    void applyQuickSelections();

    SearchSpecification &m_spec;
    SearchNotebookState &m_state;
    ck::ui::TabControl *m_tabControl = nullptr;
    QuickStartPage *m_quickStartPage = nullptr;
    ContentNamesPage *m_contentPage = nullptr;
    ck::ui::TabPageView *m_datesPage = nullptr;
    ck::ui::TabPageView *m_typesPage = nullptr;
    ck::ui::TabPageView *m_traversalPage = nullptr;
    ck::ui::TabPageView *m_actionsPage = nullptr;
};

SearchNotebookDialog::SearchNotebookDialog(SearchSpecification &spec, SearchNotebookState &state)
    : TWindowInit(&TDialog::initFrame),
      TDialog(TRect(0, 0, 83, 25), "Search Builder"),
      m_spec(spec),
      m_state(state)
{
    options |= ofCentered;

    m_tabControl = new ck::ui::TabControl(TRect(1, 1, 82, 22), 2);
    insert(m_tabControl);

    m_quickStartPage = new QuickStartPage(TRect(0, 0, 81, 20), m_state);
    m_tabControl->addTab("Quick", m_quickStartPage, cmTabQuickStart);

    m_contentPage = new ContentNamesPage(TRect(0, 0, 81, 20), m_spec.textOptions, m_spec.namePathOptions);
    m_tabControl->addTab("Content", m_contentPage, cmTabContentNames);
    m_datesPage = m_tabControl->createTab("Dates", cmTabDatesSizes);
    if (m_datesPage)
        m_datesPage->insert(new TStaticText(TRect(2, 2, 78, 18), "Dates & Sizes tab coming soon."));

    m_typesPage = m_tabControl->createTab("Types", cmTabTypesOwnership);
    if (m_typesPage)
        m_typesPage->insert(new TStaticText(TRect(2, 2, 78, 18), "Types & Ownership tab coming soon."));

    m_traversalPage = m_tabControl->createTab("Traverse", cmTabTraversal);
    if (m_traversalPage)
        m_traversalPage->insert(new TStaticText(TRect(2, 2, 78, 18), "Traversal tab coming soon."));

    m_actionsPage = m_tabControl->createTab("Actions", cmTabActions);
    if (m_actionsPage)
        m_actionsPage->insert(new TStaticText(TRect(2, 2, 78, 18), "Actions tab coming soon."));

    insert(new TButton(TRect(2, 22, 18, 24), "~P~review", cmTogglePreview, bfNormal));
    insert(new TButton(TRect(58, 22, 72, 24), "~S~earch", cmOK, bfDefault));
    insert(new TButton(TRect(73, 22, 82, 24), "Cancel", cmCancel, bfNormal));
}

void SearchNotebookDialog::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmBrowseStart:
            browseStartLocation();
            clearEvent(event);
            return;
        case cmTabQuickStart:
        case cmTabContentNames:
        case cmTabDatesSizes:
        case cmTabTypesOwnership:
        case cmTabTraversal:
        case cmTabActions:
            if (m_tabControl && m_tabControl->selectByCommand(event.message.command))
            {
                clearEvent(event);
                return;
            }
            break;
        case cmTabNext:
            if (m_tabControl)
            {
                m_tabControl->nextTab();
                clearEvent(event);
                return;
            }
            break;
        case cmTabPrevious:
            if (m_tabControl)
            {
                m_tabControl->previousTab();
                clearEvent(event);
                return;
            }
            break;
        case cmTextOptions:
            if (editTextOptions(m_spec.textOptions))
            {
                m_state.optionPrimaryFlags |= kOptionTextBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
                if (m_contentPage)
                    m_contentPage->populate();
            }
            clearEvent(event);
            return;
        case cmNamePathOptions:
            if (editNamePathOptions(m_spec.namePathOptions))
            {
                m_state.optionPrimaryFlags |= kOptionNamePathBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
                if (m_contentPage)
                    m_contentPage->populate();
            }
            clearEvent(event);
            return;
        case cmTimeFilters:
            if (editTimeFilters(m_spec.timeOptions))
            {
                m_state.optionPrimaryFlags |= kOptionTimeBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
            }
            clearEvent(event);
            return;
        case cmSizeFilters:
            if (editSizeFilters(m_spec.sizeOptions))
            {
                m_state.optionPrimaryFlags |= kOptionSizeBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
            }
            clearEvent(event);
            return;
        case cmTypeFilters:
            if (editTypeFilters(m_spec.typeOptions))
            {
                m_state.optionPrimaryFlags |= kOptionTypeBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
            }
            clearEvent(event);
            return;
        case cmPermissionOwnership:
            if (editPermissionOwnership(m_spec.permissionOptions))
            {
                m_state.optionSecondaryFlags |= kOptionPermissionBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
            }
            clearEvent(event);
            return;
        case cmTraversalFilters:
            if (editTraversalFilters(m_spec.traversalOptions))
            {
                m_state.optionSecondaryFlags |= kOptionTraversalBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
            }
            clearEvent(event);
            return;
        case cmActionOptions:
            if (editActionOptions(m_spec.actionOptions))
            {
                m_state.optionSecondaryFlags |= kOptionActionBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
            }
            clearEvent(event);
            return;
        default:
            break;
        }
    }

    TDialog::handleEvent(event);
}

Boolean SearchNotebookDialog::valid(ushort command)
{
    if (command == cmOK)
    {
        if (m_quickStartPage)
            m_quickStartPage->collect();
        applyQuickSelections();
        if (m_contentPage)
            m_contentPage->collect();
        applyStateToSpecification();
    }
    return TDialog::valid(command);
}

void SearchNotebookDialog::browseStartLocation()
{
    char location[PATH_MAX]{};
    std::snprintf(location, sizeof(location), "%s", m_state.startLocation[0] ? m_state.startLocation : ".");

    std::filesystem::path originalDir;
    try
    {
        originalDir = std::filesystem::current_path();
    }
    catch (...)
    {
    }

    try
    {
        std::filesystem::current_path(std::filesystem::path(location));
    }
    catch (...)
    {
    }

    auto *dialog = new TChDirDialog(cdNormal, 1);
    unsigned short result = TProgram::application->executeDialog(dialog);
    TObject::destroy(dialog);

    std::filesystem::path selectedDir;
    try
    {
        selectedDir = std::filesystem::current_path();
    }
    catch (...)
    {
    }

    if (!originalDir.empty())
    {
        try
        {
            std::filesystem::current_path(originalDir);
        }
        catch (...)
        {
        }
    }

    if (result == cmCancel || selectedDir.empty())
        return;

    std::string newDir = selectedDir.string();
    std::snprintf(m_state.startLocation, sizeof(m_state.startLocation), "%s", newDir.c_str());
    if (m_quickStartPage)
        m_quickStartPage->setStartLocation(m_state.startLocation);
}

void SearchNotebookDialog::applyStateToSpecification()
{
    copyToArray(m_spec.specName, m_state.specName);
    copyToArray(m_spec.startLocation, m_state.startLocation);
    copyToArray(m_spec.searchText, m_state.searchText);
    copyToArray(m_spec.includePatterns, m_state.includePatterns);
    copyToArray(m_spec.excludePatterns, m_state.excludePatterns);

    m_spec.includeSubdirectories = (m_state.generalFlags & kGeneralRecursiveBit) != 0;
    m_spec.includeHidden = (m_state.generalFlags & kGeneralHiddenBit) != 0;
    m_spec.followSymlinks = (m_state.generalFlags & kGeneralSymlinkBit) != 0;
    m_spec.stayOnSameFilesystem = (m_state.generalFlags & kGeneralStayOnFsBit) != 0;

    if (m_spec.followSymlinks)
        m_spec.traversalOptions.symlinkMode = TraversalFilesystemOptions::SymlinkMode::Everywhere;
    else if (m_spec.traversalOptions.symlinkMode == TraversalFilesystemOptions::SymlinkMode::Everywhere)
        m_spec.traversalOptions.symlinkMode = TraversalFilesystemOptions::SymlinkMode::Physical;

    m_spec.traversalOptions.stayOnFilesystem = m_spec.stayOnSameFilesystem;

    m_spec.enableNamePathTests = (m_state.optionPrimaryFlags & kOptionNamePathBit) != 0;
    m_spec.enableTimeFilters = (m_state.optionPrimaryFlags & kOptionTimeBit) != 0;
    m_spec.enableSizeFilters = (m_state.optionPrimaryFlags & kOptionSizeBit) != 0;
    m_spec.enableTypeFilters = (m_state.optionPrimaryFlags & kOptionTypeBit) != 0;

    m_spec.enablePermissionOwnership = (m_state.optionSecondaryFlags & kOptionPermissionBit) != 0;
    m_spec.enableTraversalFilters = (m_state.optionSecondaryFlags & kOptionTraversalBit) != 0;
    m_spec.enableActionOptions = (m_state.optionSecondaryFlags & kOptionActionBit) != 0;

    const bool hasText = m_spec.searchText[0] != '\0';
    m_spec.enableTextSearch = hasText && (m_spec.textOptions.searchInContents || m_spec.textOptions.searchInFileNames);
}

void SearchNotebookDialog::applyQuickSelections()
{
    const bool hasText = m_state.searchText[0] != '\0';
    if (!hasText)
    {
        m_spec.textOptions.searchInContents = false;
        m_spec.textOptions.searchInFileNames = false;
    }
    else
    {
        switch (m_state.quickSearchMode)
        {
        case 0: // contents only
            m_spec.textOptions.searchInContents = true;
            m_spec.textOptions.searchInFileNames = false;
            break;
        case 1: // names only
            m_spec.textOptions.searchInContents = false;
            m_spec.textOptions.searchInFileNames = true;
            break;
        default: // both
            m_spec.textOptions.searchInContents = true;
            m_spec.textOptions.searchInFileNames = true;
            break;
        }
        m_state.optionPrimaryFlags |= kOptionTextBit;
    }

    switch (m_state.quickTypePreset)
    {
    case 0: // all files
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionTypeBit);
        m_spec.enableTypeFilters = false;
        m_spec.typeOptions.useExtensions = false;
        break;
    case 5: // custom – leave as-is
        if (m_state.optionPrimaryFlags & kOptionTypeBit)
            m_spec.enableTypeFilters = true;
        break;
    default:
    {
        const char *extensions = nullptr;
        switch (m_state.quickTypePreset)
        {
        case 1:
            extensions = "pdf,doc,docx,txt,md,rtf";
            break;
        case 2:
            extensions = "jpg,jpeg,png,gif,bmp,svg,webp";
            break;
        case 3:
            extensions = "mp3,flac,wav,ogg,aac";
            break;
        case 4:
        default:
            extensions = "zip,tar,gz,bz2,xz,7z";
            break;
        }
        if (extensions)
        {
            m_state.optionPrimaryFlags |= kOptionTypeBit;
            m_spec.enableTypeFilters = true;
            m_spec.typeOptions.typeEnabled = false;
            m_spec.typeOptions.xtypeEnabled = false;
            m_spec.typeOptions.useExtensions = true;
            m_spec.typeOptions.extensionCaseInsensitive = true;
            copyToArray(m_spec.typeOptions.extensions, extensions);
            m_spec.typeOptions.useDetectors = false;
            m_spec.typeOptions.detectorTags[0] = '\0';
        }
        break;
    }
    }
}

} // namespace

bool configureSearchSpecification(SearchSpecification &spec)
{
    SearchNotebookState state{};
    std::snprintf(state.specName, sizeof(state.specName), "%s", bufferToString(spec.specName).c_str());
    std::snprintf(state.startLocation, sizeof(state.startLocation), "%s", bufferToString(spec.startLocation).c_str());
    std::snprintf(state.searchText, sizeof(state.searchText), "%s", bufferToString(spec.searchText).c_str());
    std::snprintf(state.includePatterns, sizeof(state.includePatterns), "%s", bufferToString(spec.includePatterns).c_str());
    std::snprintf(state.excludePatterns, sizeof(state.excludePatterns), "%s", bufferToString(spec.excludePatterns).c_str());

    if (spec.includeSubdirectories)
        state.generalFlags |= kGeneralRecursiveBit;
    if (spec.includeHidden)
        state.generalFlags |= kGeneralHiddenBit;
    if (spec.followSymlinks)
        state.generalFlags |= kGeneralSymlinkBit;
    if (spec.stayOnSameFilesystem)
        state.generalFlags |= kGeneralStayOnFsBit;

    if (spec.enableTextSearch)
        state.optionPrimaryFlags |= kOptionTextBit;
    if (spec.enableNamePathTests)
        state.optionPrimaryFlags |= kOptionNamePathBit;
    if (spec.enableTimeFilters)
        state.optionPrimaryFlags |= kOptionTimeBit;
    if (spec.enableSizeFilters)
        state.optionPrimaryFlags |= kOptionSizeBit;
    if (spec.enableTypeFilters)
        state.optionPrimaryFlags |= kOptionTypeBit;

    if (spec.enablePermissionOwnership)
        state.optionSecondaryFlags |= kOptionPermissionBit;
    if (spec.enableTraversalFilters)
        state.optionSecondaryFlags |= kOptionTraversalBit;
    if (spec.enableActionOptions)
        state.optionSecondaryFlags |= kOptionActionBit;

    if (spec.textOptions.searchInContents && !spec.textOptions.searchInFileNames)
        state.quickSearchMode = 0;
    else if (!spec.textOptions.searchInContents && spec.textOptions.searchInFileNames)
        state.quickSearchMode = 1;
    else
        state.quickSearchMode = 2;

    state.quickTypePreset = spec.enableTypeFilters ? 5 : 0;

    auto *dialog = new SearchNotebookDialog(spec, state);
    unsigned short result = TProgram::application->executeDialog(dialog);
    return result == cmOK;
}

} // namespace ck::find
