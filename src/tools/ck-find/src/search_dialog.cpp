#include "ck/find/search_dialogs.hpp"

#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/dialog_utils.hpp"
#include "ck/find/search_model.hpp"
#include "ck/ui/tab_control.hpp"

#include "command_ids.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <array>
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

constexpr unsigned short cmClearTypeFiltersLocal = 0xf200;
constexpr unsigned short cmClearOwnershipFiltersLocal = 0xf201;
constexpr unsigned short cmClearTraversalFiltersLocal = 0xf202;
constexpr unsigned short cmClearActionsLocal = 0xf203;

constexpr std::array<char, 4> kTypeLettersLeft{'b', 'c', 'd', 'p'};
constexpr std::array<char, 4> kTypeLettersRight{'f', 'l', 's', 'D'};

std::string buildTypeSummary(const TypeFilterOptions &options)
{
    std::string summary = "Extensions: ";
    if (options.useExtensions && options.extensions[0] != '\0')
    {
        summary += bufferToString(options.extensions);
        if (!options.extensionCaseInsensitive)
            summary += " (case-sensitive)";
    }
    else
    {
        summary += "off";
    }

    summary += " | Detectors: ";
    if (options.useDetectors && options.detectorTags[0] != '\0')
        summary += bufferToString(options.detectorTags);
    else
        summary += "off";

    return summary;
}

unsigned short clusterBitsFromLetters(const std::string &letters, const std::array<char, 4> &mapping)
{
    unsigned short bits = 0;
    for (std::size_t i = 0; i < mapping.size(); ++i)
    {
        if (letters.find(mapping[i]) != std::string::npos)
            bits |= static_cast<unsigned short>(1u << i);
    }
    return bits;
}

void lettersFromClusterBits(unsigned short bits, const std::array<char, 4> &mapping, std::string &out)
{
    for (std::size_t i = 0; i < mapping.size(); ++i)
    {
        if (bits & (1u << i))
            out.push_back(mapping[i]);
    }
}

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
    void onDeactivated() override;
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
    syncOptionFlags();
    if (m_specNameInput)
        m_specNameInput->selectAll(True, True);
}

void QuickStartPage::onDeactivated()
{
    collect();
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

class ContentNamesPage : public ck::ui::TabPageView
{
public:
    ContentNamesPage(const TRect &bounds,
                     SearchNotebookState &state,
                     TextSearchOptions &textOptions,
                     NamePathOptions &nameOptions,
                     TypeFilterOptions &typeOptions);

    void populate();
    void collect();

protected:
    void onActivated() override;
    void onDeactivated() override;
    void handleEvent(TEvent &event) override;

private:
    void updateCopyButtonState();
    void updateExtensionControls();
    void updateDetectorControls();

    SearchNotebookState &m_state;
    TextSearchOptions &m_textOptions;
    NamePathOptions &m_nameOptions;
    TypeFilterOptions &m_typeOptions;
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
    TCheckBoxes *m_extensionToggle = nullptr;
    TInputLine *m_extensionInput = nullptr;
    TCheckBoxes *m_detectorToggle = nullptr;
    TInputLine *m_detectorInput = nullptr;
    TButton *m_copyButton = nullptr;
    TButton *m_clearButton = nullptr;
};

class DatesSizesPage : public ck::ui::TabPageView
{
public:
    DatesSizesPage(const TRect &bounds,
                   SearchNotebookState &state,
                   TimeFilterOptions &timeOptions,
                   SizeFilterOptions &sizeOptions);

    void populate();
    void collect();

protected:
    void onActivated() override;
    void onDeactivated() override;
    void handleEvent(TEvent &event) override;

private:
    void updateCustomRangeControls();
    void updateSizeInputs();

    SearchNotebookState &m_state;
    TimeFilterOptions &m_timeOptions;
    SizeFilterOptions &m_sizeOptions;
    TRadioButtons *m_presetButtons = nullptr;
    TCheckBoxes *m_timeFieldBoxes = nullptr;
    TInputLine *m_fromInput = nullptr;
    TInputLine *m_toInput = nullptr;
    TCheckBoxes *m_sizeEnableBoxes = nullptr;
    TInputLine *m_minSizeInput = nullptr;
    TInputLine *m_maxSizeInput = nullptr;
    TInputLine *m_exactSizeInput = nullptr;
    TCheckBoxes *m_sizeFlagBoxes = nullptr;
};

class TypesOwnershipPage : public ck::ui::TabPageView
{
public:
    TypesOwnershipPage(const TRect &bounds,
                       SearchNotebookState &state,
                       TypeFilterOptions &typeOptions,
                       PermissionOwnershipOptions &permOptions);

    void populate();
    void collect();

protected:
    void onActivated() override;
    void onDeactivated() override;
    void handleEvent(TEvent &event) override;

private:
    void updateTypeControls();
    void updatePermissionControls();
    void updateOwnershipControls();
    void updateExtensionSummary();
    void applyOptionFlags();

    SearchNotebookState &m_state;
    TypeFilterOptions &m_typeOptions;
    PermissionOwnershipOptions &m_permOptions;

    TCheckBoxes *m_typeEnableBoxes = nullptr;
    TCheckBoxes *m_typeBoxesLeft = nullptr;
    TCheckBoxes *m_typeBoxesRight = nullptr;
    TCheckBoxes *m_xtypeBoxesLeft = nullptr;
    TCheckBoxes *m_xtypeBoxesRight = nullptr;
    TInputLine *m_extensionSummary = nullptr;
    std::array<char, 128> m_extensionBuffer{};
    TButton *m_clearTypeButton = nullptr;

    TCheckBoxes *m_permBoxes = nullptr;
    TRadioButtons *m_permModeButtons = nullptr;
    TInputLine *m_permInput = nullptr;

    TCheckBoxes *m_ownerBoxes = nullptr;
    TInputLine *m_userInput = nullptr;
    TInputLine *m_uidInput = nullptr;
    TInputLine *m_groupInput = nullptr;
    TInputLine *m_gidInput = nullptr;
    TButton *m_clearOwnershipButton = nullptr;
};

class TraversalPage : public ck::ui::TabPageView
{
public:
    TraversalPage(const TRect &bounds,
                  SearchNotebookState &state,
                  TraversalFilesystemOptions &options);

    void populate();
    void collect();

protected:
    void onActivated() override;
    void onDeactivated() override;
    void handleEvent(TEvent &event) override;

private:
    void updateValueControls();
    void updateFlags();

    SearchNotebookState &m_state;
    TraversalFilesystemOptions &m_options;

    TRadioButtons *m_symlinkButtons = nullptr;
    TRadioButtons *m_warningButtons = nullptr;
    TCheckBoxes *m_flagBoxes = nullptr;
    TCheckBoxes *m_valueBoxes = nullptr;
    TInputLine *m_maxDepthInput = nullptr;
    TInputLine *m_minDepthInput = nullptr;
    TInputLine *m_filesFromInput = nullptr;
    TInputLine *m_fsTypeInput = nullptr;
    TInputLine *m_linkCountInput = nullptr;
    TInputLine *m_sameFileInput = nullptr;
    TInputLine *m_inodeInput = nullptr;
    TButton *m_clearButton = nullptr;
};

class ActionsPage : public ck::ui::TabPageView
{
public:
    ActionsPage(const TRect &bounds,
                SearchNotebookState &state,
                ActionOptions &options);

    void populate();
    void collect();

protected:
    void onActivated() override;
    void onDeactivated() override;
    void handleEvent(TEvent &event) override;

private:
    void updateExecControls();
    void updateFileOutputs();
    void updateWarning();
    void applyOptionFlags();

    SearchNotebookState &m_state;
    ActionOptions &m_options;

    TCheckBoxes *m_outputBoxes = nullptr;
    TCheckBoxes *m_execBoxes = nullptr;
    TRadioButtons *m_execVariantButtons = nullptr;
    TInputLine *m_execInput = nullptr;

    TCheckBoxes *m_fileToggleBoxes = nullptr;
    TCheckBoxes *m_appendBoxes = nullptr;
    TInputLine *m_fprintInput = nullptr;
    TInputLine *m_fprint0Input = nullptr;
    TInputLine *m_flsInput = nullptr;
    TInputLine *m_printfInput = nullptr;
    TInputLine *m_fprintfFileInput = nullptr;
    TInputLine *m_fprintfFormatInput = nullptr;
    TStaticText *m_warningText = nullptr;
    TButton *m_clearButton = nullptr;
};

ContentNamesPage::ContentNamesPage(const TRect &bounds,
                                   SearchNotebookState &state,
                                   TextSearchOptions &textOptions,
                                   NamePathOptions &nameOptions,
                                   TypeFilterOptions &typeOptions)
    : ck::ui::TabPageView(bounds),
      m_state(state),
      m_textOptions(textOptions),
      m_nameOptions(nameOptions),
      m_typeOptions(typeOptions)
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

    insert(new TStaticText(TRect(2, 13, 78, 14), "Prune matching directories"));

    m_pruneFlags = new TCheckBoxes(TRect(2, 14, 18, 16), makeItemList({"Enable -p~r~une",
                                                                       "Directories ~o~nly"}));
    insert(m_pruneFlags);

    m_pruneModeButtons = new TRadioButtons(TRect(20, 14, 54, 18),
                                           makeItemList({"Use -name",
                                                         "Use -iname",
                                                         "Use -path",
                                                         "Use -ipath",
                                                         "Use -regex",
                                                         "Use -iregex"}));
    insert(m_pruneModeButtons);

    m_pruneInput = new TInputLine(TRect(56, 14, 78, 15), sizeof(m_nameOptions.prunePattern) - 1);
    insert(new TLabel(TRect(56, 13, 78, 14), "Pattern:", m_pruneInput));
    insert(m_pruneInput);

    insert(new TStaticText(TRect(2, 16, 78, 17), "Extensions and detectors"));

    m_extensionToggle = new TCheckBoxes(TRect(2, 17, 22, 18), makeItemList({"Filter by e~x~tension"}));
    insert(m_extensionToggle);
    m_extensionInput = new TInputLine(TRect(24, 17, 78, 18), sizeof(m_typeOptions.extensions) - 1);
    insert(m_extensionInput);

    m_detectorToggle = new TCheckBoxes(TRect(2, 18, 22, 19), makeItemList({"Use detector ~t~ags"}));
    insert(m_detectorToggle);
    m_detectorInput = new TInputLine(TRect(24, 18, 78, 19), sizeof(m_typeOptions.detectorTags) - 1);
    insert(m_detectorInput);

    m_copyButton = new TButton(TRect(24, 19, 50, 20), "~U~se quick search text", cmCopySearchToName, bfNormal);
    insert(m_copyButton);
    m_clearButton = new TButton(TRect(52, 19, 78, 20), "C~l~ear name filters", cmClearNameFilters, bfNormal);
    insert(m_clearButton);

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

    unsigned short extensionFlag = m_typeOptions.useExtensions ? 0x0001 : 0;
    if (m_extensionToggle)
        m_extensionToggle->setData(&extensionFlag);
    if (m_extensionInput)
        m_extensionInput->setData(m_typeOptions.extensions.data());

    unsigned short detectorFlag = m_typeOptions.useDetectors ? 0x0001 : 0;
    if (m_detectorToggle)
        m_detectorToggle->setData(&detectorFlag);
    if (m_detectorInput)
        m_detectorInput->setData(m_typeOptions.detectorTags.data());

    updateCopyButtonState();
    updateExtensionControls();
    updateDetectorControls();
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

    if (m_extensionToggle)
    {
        unsigned short flag = 0;
        m_extensionToggle->getData(&flag);
        m_typeOptions.useExtensions = (flag & 0x0001) != 0;
        if (!m_typeOptions.useExtensions)
            m_typeOptions.extensions.fill('\0');
    }
    if (m_extensionInput && m_typeOptions.useExtensions)
        m_extensionInput->getData(m_typeOptions.extensions.data());

    if (m_detectorToggle)
    {
        unsigned short flag = 0;
        m_detectorToggle->getData(&flag);
        m_typeOptions.useDetectors = (flag & 0x0001) != 0;
        if (!m_typeOptions.useDetectors)
            m_typeOptions.detectorTags.fill('\0');
    }
    if (m_detectorInput && m_typeOptions.useDetectors)
        m_detectorInput->getData(m_typeOptions.detectorTags.data());

    const bool hasNameFilters = (matcherFlags != 0) || (pruneFlags & 0x0001);
    if (hasNameFilters)
        m_state.optionPrimaryFlags |= kOptionNamePathBit;
    else
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionNamePathBit);

    const bool hasText = m_state.searchText[0] != '\0';
    const bool textEnabled = m_textOptions.searchInContents || m_textOptions.searchInFileNames;
    if (hasText && textEnabled)
        m_state.optionPrimaryFlags |= kOptionTextBit;
    else
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionTextBit);

    const bool hasTypeFilters = m_typeOptions.useExtensions || m_typeOptions.useDetectors ||
                                m_typeOptions.typeEnabled || m_typeOptions.xtypeEnabled;
    if (hasTypeFilters)
        m_state.optionPrimaryFlags |= kOptionTypeBit;
    else
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionTypeBit);
}

void ContentNamesPage::onActivated()
{
    populate();
}

void ContentNamesPage::onDeactivated()
{
    collect();
}

void ContentNamesPage::updateCopyButtonState()
{
    if (!m_copyButton)
        return;
    const bool hasText = m_state.searchText[0] != '\0';
    m_copyButton->setState(sfDisabled, hasText ? False : True);
}

void ContentNamesPage::updateExtensionControls()
{
    if (!m_extensionToggle || !m_extensionInput)
        return;

    unsigned short flag = 0;
    m_extensionToggle->getData(&flag);
    const bool enabled = (flag & 0x0001) != 0;
    m_extensionInput->setState(sfDisabled, enabled ? False : True);
}

void ContentNamesPage::updateDetectorControls()
{
    if (!m_detectorToggle || !m_detectorInput)
        return;

    unsigned short flag = 0;
    m_detectorToggle->getData(&flag);
    const bool enabled = (flag & 0x0001) != 0;
    m_detectorInput->setState(sfDisabled, enabled ? False : True);
}

void ContentNamesPage::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmCopySearchToName:
            if (m_state.searchText[0] != '\0')
            {
                copyToArray(m_nameOptions.namePattern, m_state.searchText);
                m_nameOptions.nameEnabled = true;
                m_state.optionPrimaryFlags |= kOptionNamePathBit;
                populate();
            }
            clearEvent(event);
            return;
        case cmClearNameFilters:
            m_nameOptions = NamePathOptions{};
            m_typeOptions.typeEnabled = false;
            m_typeOptions.xtypeEnabled = false;
            m_typeOptions.useExtensions = false;
            m_typeOptions.extensions.fill('\0');
            m_typeOptions.useDetectors = false;
            m_typeOptions.detectorTags.fill('\0');
            m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionNamePathBit);
            populate();
            clearEvent(event);
            return;
        default:
            break;
        }
    }
    TGroup::handleEvent(event);
    updateCopyButtonState();
    updateExtensionControls();
    updateDetectorControls();
}

DatesSizesPage::DatesSizesPage(const TRect &bounds,
                               SearchNotebookState &state,
                               TimeFilterOptions &timeOptions,
                               SizeFilterOptions &sizeOptions)
    : ck::ui::TabPageView(bounds),
      m_state(state),
      m_timeOptions(timeOptions),
      m_sizeOptions(sizeOptions)
{
    m_presetButtons = new TRadioButtons(TRect(2, 1, 26, 9),
                                        makeItemList({"Any ~t~ime",
                                                      "Past ~1~ day",
                                                      "Past ~7~ days",
                                                      "Past ~1~ month",
                                                      "Past ~6~ months",
                                                      "Past ~1~ year",
                                                      "Past ~6~ years",
                                                      "~C~ustom range"}));
    insert(m_presetButtons);

    m_timeFieldBoxes = new TCheckBoxes(TRect(28, 1, 54, 5),
                                       makeItemList({"Last ~m~odified",
                                                     "~C~reation time",
                                                     "Last ~a~ccess"}));
    insert(m_timeFieldBoxes);

    m_fromInput = new TInputLine(TRect(28, 5, 54, 6), sizeof(m_timeOptions.customFrom) - 1);
    insert(new TLabel(TRect(28, 4, 54, 5), "~F~rom (YYYY-MM-DD):", m_fromInput));
    insert(m_fromInput);

    m_toInput = new TInputLine(TRect(56, 5, 78, 6), sizeof(m_timeOptions.customTo) - 1);
    insert(new TLabel(TRect(56, 4, 78, 5), "~T~o (YYYY-MM-DD):", m_toInput));
    insert(m_toInput);

    insert(new TButton(TRect(56, 7, 78, 9), "Advanced ~T~ime...", cmTimeFilters, bfNormal));

    insert(new TStaticText(TRect(2, 9, 78, 10), "Size filters"));

    m_sizeEnableBoxes = new TCheckBoxes(TRect(2, 10, 24, 14),
                                        makeItemList({"Use ~m~in size",
                                                      "Use ma~x~ size",
                                                      "Use e~x~act size"}));
    insert(m_sizeEnableBoxes);

    m_minSizeInput = new TInputLine(TRect(26, 10, 42, 11), sizeof(m_sizeOptions.minSpec) - 1);
    insert(new TLabel(TRect(26, 9, 42, 10), "Min:", m_minSizeInput));
    insert(m_minSizeInput);

    m_maxSizeInput = new TInputLine(TRect(44, 10, 60, 11), sizeof(m_sizeOptions.maxSpec) - 1);
    insert(new TLabel(TRect(44, 9, 60, 10), "Max:", m_maxSizeInput));
    insert(m_maxSizeInput);

    m_exactSizeInput = new TInputLine(TRect(62, 10, 78, 11), sizeof(m_sizeOptions.exactSpec) - 1);
    insert(new TLabel(TRect(62, 9, 78, 10), "Exact:", m_exactSizeInput));
    insert(m_exactSizeInput);

    insert(new TStaticText(TRect(26, 11, 78, 12), "Hint: 10K, 5M, 1G etc."));

    m_sizeFlagBoxes = new TCheckBoxes(TRect(26, 12, 78, 17),
                                      makeItemList({"Inclusive rang~e~ ends",
                                                    "Include ~0~-byte entries",
                                                    "Treat ~d~irectories as files",
                                                    "Use ~d~ecimal units",
                                                    "Match ~e~mpty entries"}));
    insert(m_sizeFlagBoxes);

    insert(new TButton(TRect(26, 17, 44, 19), "Advanced ~S~ize...", cmSizeFilters, bfNormal));

    populate();
}

void DatesSizesPage::populate()
{
    unsigned short preset = static_cast<unsigned short>(m_timeOptions.preset);
    if (m_presetButtons)
        m_presetButtons->setData(&preset);

    unsigned short fields = 0;
    if (m_timeOptions.includeModified)
        fields |= 0x0001;
    if (m_timeOptions.includeCreated)
        fields |= 0x0002;
    if (m_timeOptions.includeAccessed)
        fields |= 0x0004;
    if (m_timeFieldBoxes)
        m_timeFieldBoxes->setData(&fields);

    if (m_fromInput)
        m_fromInput->setData(m_timeOptions.customFrom.data());
    if (m_toInput)
        m_toInput->setData(m_timeOptions.customTo.data());

    unsigned short sizeEnable = 0;
    if (m_sizeOptions.minEnabled)
        sizeEnable |= 0x0001;
    if (m_sizeOptions.maxEnabled)
        sizeEnable |= 0x0002;
    if (m_sizeOptions.exactEnabled)
        sizeEnable |= 0x0004;
    if (m_sizeEnableBoxes)
        m_sizeEnableBoxes->setData(&sizeEnable);

    if (m_minSizeInput)
        m_minSizeInput->setData(m_sizeOptions.minSpec.data());
    if (m_maxSizeInput)
        m_maxSizeInput->setData(m_sizeOptions.maxSpec.data());
    if (m_exactSizeInput)
        m_exactSizeInput->setData(m_sizeOptions.exactSpec.data());

    unsigned short sizeFlags = 0;
    if (m_sizeOptions.rangeInclusive)
        sizeFlags |= 0x0001;
    if (m_sizeOptions.includeZeroByte)
        sizeFlags |= 0x0002;
    if (m_sizeOptions.treatDirectoriesAsFiles)
        sizeFlags |= 0x0004;
    if (m_sizeOptions.useDecimalUnits)
        sizeFlags |= 0x0008;
    if (m_sizeOptions.emptyEnabled)
        sizeFlags |= 0x0010;
    if (m_sizeFlagBoxes)
        m_sizeFlagBoxes->setData(&sizeFlags);

    updateCustomRangeControls();
    updateSizeInputs();
}

void DatesSizesPage::collect()
{
    unsigned short preset = 0;
    if (m_presetButtons)
    {
        m_presetButtons->getData(&preset);
        m_timeOptions.preset = static_cast<TimeFilterOptions::Preset>(preset);
    }

    unsigned short fields = 0;
    if (m_timeFieldBoxes)
        m_timeFieldBoxes->getData(&fields);
    m_timeOptions.includeModified = (fields & 0x0001) != 0;
    m_timeOptions.includeCreated = (fields & 0x0002) != 0;
    m_timeOptions.includeAccessed = (fields & 0x0004) != 0;

    if (m_fromInput)
        m_fromInput->getData(m_timeOptions.customFrom.data());
    if (m_toInput)
        m_toInput->getData(m_timeOptions.customTo.data());

    unsigned short sizeEnable = 0;
    if (m_sizeEnableBoxes)
        m_sizeEnableBoxes->getData(&sizeEnable);
    m_sizeOptions.minEnabled = (sizeEnable & 0x0001) != 0;
    m_sizeOptions.maxEnabled = (sizeEnable & 0x0002) != 0;
    m_sizeOptions.exactEnabled = (sizeEnable & 0x0004) != 0;

    if (m_minSizeInput)
        m_minSizeInput->getData(m_sizeOptions.minSpec.data());
    if (m_maxSizeInput)
        m_maxSizeInput->getData(m_sizeOptions.maxSpec.data());
    if (m_exactSizeInput)
        m_exactSizeInput->getData(m_sizeOptions.exactSpec.data());

    unsigned short sizeFlags = 0;
    if (m_sizeFlagBoxes)
        m_sizeFlagBoxes->getData(&sizeFlags);
    m_sizeOptions.rangeInclusive = (sizeFlags & 0x0001) != 0;
    m_sizeOptions.includeZeroByte = (sizeFlags & 0x0002) != 0;
    m_sizeOptions.treatDirectoriesAsFiles = (sizeFlags & 0x0004) != 0;
    m_sizeOptions.useDecimalUnits = (sizeFlags & 0x0008) != 0;
    m_sizeOptions.emptyEnabled = (sizeFlags & 0x0010) != 0;

    const bool timeEnabled = (m_timeOptions.preset != TimeFilterOptions::Preset::AnyTime) ||
                             !m_timeOptions.includeModified || m_timeOptions.includeCreated ||
                             m_timeOptions.includeAccessed ||
                             m_timeOptions.customFrom[0] != '\0' || m_timeOptions.customTo[0] != '\0';
    if (timeEnabled)
        m_state.optionPrimaryFlags |= kOptionTimeBit;
    else
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionTimeBit);

    const bool sizeEnabled = m_sizeOptions.minEnabled || m_sizeOptions.maxEnabled ||
                              m_sizeOptions.exactEnabled || m_sizeOptions.emptyEnabled;
    if (sizeEnabled)
        m_state.optionPrimaryFlags |= kOptionSizeBit;
    else
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionSizeBit);
}

void DatesSizesPage::onActivated()
{
    populate();
}

void DatesSizesPage::onDeactivated()
{
    collect();
}

void DatesSizesPage::updateCustomRangeControls()
{
    if (!m_presetButtons)
        return;

    unsigned short preset = 0;
    m_presetButtons->getData(&preset);
    const bool custom = (preset == static_cast<unsigned short>(TimeFilterOptions::Preset::CustomRange));

    if (m_fromInput)
        m_fromInput->setState(sfDisabled, custom ? False : True);
    if (m_toInput)
        m_toInput->setState(sfDisabled, custom ? False : True);
}

void DatesSizesPage::updateSizeInputs()
{
    if (!m_sizeEnableBoxes)
        return;

    unsigned short enabled = 0;
    m_sizeEnableBoxes->getData(&enabled);
    const bool minEnabled = (enabled & 0x0001) != 0;
    const bool maxEnabled = (enabled & 0x0002) != 0;
    const bool exactEnabled = (enabled & 0x0004) != 0;

    if (m_minSizeInput)
        m_minSizeInput->setState(sfDisabled, minEnabled ? False : True);
    if (m_maxSizeInput)
        m_maxSizeInput->setState(sfDisabled, maxEnabled ? False : True);
    if (m_exactSizeInput)
        m_exactSizeInput->setState(sfDisabled, exactEnabled ? False : True);
}

void DatesSizesPage::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmTimeFilters:
            if (editTimeFilters(m_timeOptions))
                populate();
            clearEvent(event);
            return;
        case cmSizeFilters:
            if (editSizeFilters(m_sizeOptions))
                populate();
            clearEvent(event);
            return;
        default:
            break;
        }
    }
    TGroup::handleEvent(event);
    updateCustomRangeControls();
    updateSizeInputs();
}

TypesOwnershipPage::TypesOwnershipPage(const TRect &bounds,
                                       SearchNotebookState &state,
                                       TypeFilterOptions &typeOptions,
                                       PermissionOwnershipOptions &permOptions)
    : ck::ui::TabPageView(bounds),
      m_state(state),
      m_typeOptions(typeOptions),
      m_permOptions(permOptions)
{
    insert(new TStaticText(TRect(2, 0, 78, 1), "Filter files by type, permissions, and ownership."));

    m_typeEnableBoxes = new TCheckBoxes(TRect(2, 1, 42, 3),
                                        makeItemList({"Enable -~t~ype (-type)",
                                                      "Enable -~x~type (-xtype)"}));
    insert(m_typeEnableBoxes);

    m_typeBoxesLeft = new TCheckBoxes(TRect(2, 3, 22, 7),
                                      makeItemList({"Block device (b)",
                                                    "Character device (c)",
                                                    "Directory (d)",
                                                    "FIFO / pipe (p)"}));
    insert(m_typeBoxesLeft);

    m_typeBoxesRight = new TCheckBoxes(TRect(22, 3, 42, 7),
                                       makeItemList({"Regular file (f)",
                                                     "Symbolic link (l)",
                                                     "Socket (s)",
                                                     "Door (D)"}));
    insert(m_typeBoxesRight);

    m_xtypeBoxesLeft = new TCheckBoxes(TRect(42, 3, 62, 7),
                                       makeItemList({"b (post)",
                                                     "c (post)",
                                                     "d (post)",
                                                     "p (post)"}));
    insert(m_xtypeBoxesLeft);

    m_xtypeBoxesRight = new TCheckBoxes(TRect(62, 3, 78, 7),
                                        makeItemList({"f (post)",
                                                      "l (post)",
                                                      "s (post)",
                                                      "D (post)"}));
    insert(m_xtypeBoxesRight);

    m_extensionSummary = new TInputLine(TRect(2, 7, 56, 8), static_cast<int>(m_extensionBuffer.size()) - 1);
    insert(new TLabel(TRect(2, 6, 56, 7), "Extension / detector summary:", m_extensionSummary));
    insert(m_extensionSummary);
    m_extensionSummary->setState(sfDisabled, True);

    insert(new TButton(TRect(58, 6, 78, 8), "Advanced ~t~ype...", cmTypeFilters, bfNormal));
    m_clearTypeButton = new TButton(TRect(58, 8, 78, 10), "Clear type filter", cmClearTypeFiltersLocal, bfNormal);
    insert(m_clearTypeButton);

    insert(new TStaticText(TRect(2, 9, 78, 10), "Permissions"));

    m_permBoxes = new TCheckBoxes(TRect(2, 10, 28, 14),
                                  makeItemList({"Use -~p~erm value",
                                                "-~r~eadable",
                                                "-~w~ritable",
                                                "-~e~xecutable"}));
    insert(m_permBoxes);

    m_permModeButtons = new TRadioButtons(TRect(30, 10, 58, 14),
                                          makeItemList({"Exact (-perm value)",
                                                        "All bits (-perm -mode)",
                                                        "Any bit (-perm /mode)"}));
    insert(m_permModeButtons);

    m_permInput = new TInputLine(TRect(58, 10, 78, 11), sizeof(m_permOptions.permSpec) - 1);
    insert(new TLabel(TRect(58, 9, 78, 10), "-perm:", m_permInput));
    insert(m_permInput);

    insert(new TButton(TRect(58, 11, 78, 13), "Advanced ~p~erms...", cmPermissionOwnership, bfNormal));

    insert(new TStaticText(TRect(2, 14, 78, 15), "Ownership"));

    m_ownerBoxes = new TCheckBoxes(TRect(2, 15, 28, 20),
                                   makeItemList({"Filter ~u~ser (-user)",
                                                 "Match ~U~ID (-uid)",
                                                 "Filter ~g~roup (-group)",
                                                 "Match ~G~ID (-gid)",
                                                 "-~n~ouser",
                                                 "-~n~ogroup"}));
    insert(m_ownerBoxes);

    m_userInput = new TInputLine(TRect(30, 15, 78, 16), sizeof(m_permOptions.user) - 1);
    insert(new TLabel(TRect(30, 14, 78, 15), "User name:", m_userInput));
    insert(m_userInput);

    m_uidInput = new TInputLine(TRect(30, 16, 78, 17), sizeof(m_permOptions.uid) - 1);
    insert(new TLabel(TRect(30, 15, 78, 16), "UID:", m_uidInput));
    insert(m_uidInput);

    m_groupInput = new TInputLine(TRect(30, 17, 78, 18), sizeof(m_permOptions.group) - 1);
    insert(new TLabel(TRect(30, 16, 78, 17), "Group:", m_groupInput));
    insert(m_groupInput);

    m_gidInput = new TInputLine(TRect(30, 18, 78, 19), sizeof(m_permOptions.gid) - 1);
    insert(new TLabel(TRect(30, 17, 78, 18), "GID:", m_gidInput));
    insert(m_gidInput);

    m_clearOwnershipButton = new TButton(TRect(58, 18, 78, 20), "Clear ownership", cmClearOwnershipFiltersLocal, bfNormal);
    insert(m_clearOwnershipButton);

    populate();
}

void TypesOwnershipPage::populate()
{
    unsigned short enableFlags = 0;
    if (m_typeOptions.typeEnabled)
        enableFlags |= 0x0001;
    if (m_typeOptions.xtypeEnabled)
        enableFlags |= 0x0002;
    if (m_typeEnableBoxes)
        m_typeEnableBoxes->setData(&enableFlags);

    std::string typeLetters = bufferToString(m_typeOptions.typeLetters);
    unsigned short leftBits = clusterBitsFromLetters(typeLetters, kTypeLettersLeft);
    if (m_typeBoxesLeft)
        m_typeBoxesLeft->setData(&leftBits);
    unsigned short rightBits = clusterBitsFromLetters(typeLetters, kTypeLettersRight);
    if (m_typeBoxesRight)
        m_typeBoxesRight->setData(&rightBits);

    std::string xtypeLetters = bufferToString(m_typeOptions.xtypeLetters);
    leftBits = clusterBitsFromLetters(xtypeLetters, kTypeLettersLeft);
    if (m_xtypeBoxesLeft)
        m_xtypeBoxesLeft->setData(&leftBits);
    rightBits = clusterBitsFromLetters(xtypeLetters, kTypeLettersRight);
    if (m_xtypeBoxesRight)
        m_xtypeBoxesRight->setData(&rightBits);

    updateExtensionSummary();

    unsigned short permFlags = 0;
    if (m_permOptions.permEnabled)
        permFlags |= 0x0001;
    if (m_permOptions.readable)
        permFlags |= 0x0002;
    if (m_permOptions.writable)
        permFlags |= 0x0004;
    if (m_permOptions.executable)
        permFlags |= 0x0008;
    if (m_permBoxes)
        m_permBoxes->setData(&permFlags);

    unsigned short mode = static_cast<unsigned short>(m_permOptions.permMode);
    if (m_permModeButtons)
        m_permModeButtons->setData(&mode);

    if (m_permInput)
        m_permInput->setData(m_permOptions.permSpec.data());

    unsigned short ownerFlags = 0;
    if (m_permOptions.userEnabled)
        ownerFlags |= 0x0001;
    if (m_permOptions.uidEnabled)
        ownerFlags |= 0x0002;
    if (m_permOptions.groupEnabled)
        ownerFlags |= 0x0004;
    if (m_permOptions.gidEnabled)
        ownerFlags |= 0x0008;
    if (m_permOptions.noUser)
        ownerFlags |= 0x0010;
    if (m_permOptions.noGroup)
        ownerFlags |= 0x0020;
    if (m_ownerBoxes)
        m_ownerBoxes->setData(&ownerFlags);

    if (m_userInput)
        m_userInput->setData(m_permOptions.user.data());
    if (m_uidInput)
        m_uidInput->setData(m_permOptions.uid.data());
    if (m_groupInput)
        m_groupInput->setData(m_permOptions.group.data());
    if (m_gidInput)
        m_gidInput->setData(m_permOptions.gid.data());

    updateTypeControls();
    updatePermissionControls();
    updateOwnershipControls();
    applyOptionFlags();
}

void TypesOwnershipPage::collect()
{
    unsigned short enableFlags = 0;
    if (m_typeEnableBoxes)
        m_typeEnableBoxes->getData(&enableFlags);
    m_typeOptions.typeEnabled = (enableFlags & 0x0001) != 0;
    m_typeOptions.xtypeEnabled = (enableFlags & 0x0002) != 0;

    std::string typeLetters;
    unsigned short leftBits = 0;
    if (m_typeBoxesLeft)
        m_typeBoxesLeft->getData(&leftBits);
    lettersFromClusterBits(leftBits, kTypeLettersLeft, typeLetters);
    unsigned short rightBits = 0;
    if (m_typeBoxesRight)
        m_typeBoxesRight->getData(&rightBits);
    lettersFromClusterBits(rightBits, kTypeLettersRight, typeLetters);
    copyToArray(m_typeOptions.typeLetters, typeLetters.c_str());
    if (!m_typeOptions.typeEnabled)
        m_typeOptions.typeLetters[0] = '\0';

    std::string xtypeLetters;
    if (m_xtypeBoxesLeft)
        m_xtypeBoxesLeft->getData(&leftBits);
    else
        leftBits = 0;
    lettersFromClusterBits(leftBits, kTypeLettersLeft, xtypeLetters);
    if (m_xtypeBoxesRight)
        m_xtypeBoxesRight->getData(&rightBits);
    else
        rightBits = 0;
    lettersFromClusterBits(rightBits, kTypeLettersRight, xtypeLetters);
    copyToArray(m_typeOptions.xtypeLetters, xtypeLetters.c_str());
    if (!m_typeOptions.xtypeEnabled)
        m_typeOptions.xtypeLetters[0] = '\0';

    unsigned short permFlags = 0;
    if (m_permBoxes)
        m_permBoxes->getData(&permFlags);
    m_permOptions.permEnabled = (permFlags & 0x0001) != 0;
    m_permOptions.readable = (permFlags & 0x0002) != 0;
    m_permOptions.writable = (permFlags & 0x0004) != 0;
    m_permOptions.executable = (permFlags & 0x0008) != 0;
    if (m_permOptions.permEnabled)
    {
        if (m_permModeButtons)
        {
            unsigned short mode = 0;
            m_permModeButtons->getData(&mode);
            m_permOptions.permMode = static_cast<PermissionOwnershipOptions::PermMode>(mode);
        }
        if (m_permInput)
            m_permInput->getData(m_permOptions.permSpec.data());
    }
    else
    {
        m_permOptions.permSpec.fill('\0');
    }

    unsigned short ownerFlags = 0;
    if (m_ownerBoxes)
        m_ownerBoxes->getData(&ownerFlags);
    m_permOptions.userEnabled = (ownerFlags & 0x0001) != 0;
    m_permOptions.uidEnabled = (ownerFlags & 0x0002) != 0;
    m_permOptions.groupEnabled = (ownerFlags & 0x0004) != 0;
    m_permOptions.gidEnabled = (ownerFlags & 0x0008) != 0;
    m_permOptions.noUser = (ownerFlags & 0x0010) != 0;
    m_permOptions.noGroup = (ownerFlags & 0x0020) != 0;

    if (m_permOptions.userEnabled && m_userInput)
        m_userInput->getData(m_permOptions.user.data());
    else
        m_permOptions.user.fill('\0');

    if (m_permOptions.uidEnabled && m_uidInput)
        m_uidInput->getData(m_permOptions.uid.data());
    else
        m_permOptions.uid.fill('\0');

    if (m_permOptions.groupEnabled && m_groupInput)
        m_groupInput->getData(m_permOptions.group.data());
    else
        m_permOptions.group.fill('\0');

    if (m_permOptions.gidEnabled && m_gidInput)
        m_gidInput->getData(m_permOptions.gid.data());
    else
        m_permOptions.gid.fill('\0');

    applyOptionFlags();
}

void TypesOwnershipPage::onActivated()
{
    populate();
}

void TypesOwnershipPage::onDeactivated()
{
    collect();
}

void TypesOwnershipPage::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmClearTypeFiltersLocal:
            m_typeOptions = TypeFilterOptions{};
            applyOptionFlags();
            populate();
            clearEvent(event);
            return;
        case cmClearOwnershipFiltersLocal:
            m_permOptions = PermissionOwnershipOptions{};
            applyOptionFlags();
            populate();
            clearEvent(event);
            return;
        default:
            break;
        }
    }

    TGroup::handleEvent(event);
    updateTypeControls();
    updatePermissionControls();
    updateOwnershipControls();
}

void TypesOwnershipPage::updateTypeControls()
{
    unsigned short enableFlags = 0;
    if (m_typeEnableBoxes)
        m_typeEnableBoxes->getData(&enableFlags);

    const Boolean disableType = (enableFlags & 0x0001) != 0 ? False : True;
    const Boolean disableXType = (enableFlags & 0x0002) != 0 ? False : True;

    if (m_typeBoxesLeft)
        m_typeBoxesLeft->setState(sfDisabled, disableType);
    if (m_typeBoxesRight)
        m_typeBoxesRight->setState(sfDisabled, disableType);
    if (m_xtypeBoxesLeft)
        m_xtypeBoxesLeft->setState(sfDisabled, disableXType);
    if (m_xtypeBoxesRight)
        m_xtypeBoxesRight->setState(sfDisabled, disableXType);
}

void TypesOwnershipPage::updatePermissionControls()
{
    unsigned short flags = 0;
    if (m_permBoxes)
        m_permBoxes->getData(&flags);
    const Boolean enablePerm = (flags & 0x0001) != 0 ? False : True;

    if (m_permModeButtons)
        m_permModeButtons->setState(sfDisabled, enablePerm);
    if (m_permInput)
        m_permInput->setState(sfDisabled, enablePerm);
}

void TypesOwnershipPage::updateOwnershipControls()
{
    unsigned short ownerFlags = 0;
    if (m_ownerBoxes)
        m_ownerBoxes->getData(&ownerFlags);

    const Boolean userDisabled = (ownerFlags & 0x0001) != 0 ? False : True;
    const Boolean uidDisabled = (ownerFlags & 0x0002) != 0 ? False : True;
    const Boolean groupDisabled = (ownerFlags & 0x0004) != 0 ? False : True;
    const Boolean gidDisabled = (ownerFlags & 0x0008) != 0 ? False : True;

    if (m_userInput)
        m_userInput->setState(sfDisabled, userDisabled);
    if (m_uidInput)
        m_uidInput->setState(sfDisabled, uidDisabled);
    if (m_groupInput)
        m_groupInput->setState(sfDisabled, groupDisabled);
    if (m_gidInput)
        m_gidInput->setState(sfDisabled, gidDisabled);
}

void TypesOwnershipPage::updateExtensionSummary()
{
    if (!m_extensionSummary)
        return;
    m_extensionBuffer.fill('\0');
    std::string summary = buildTypeSummary(m_typeOptions);
    std::snprintf(m_extensionBuffer.data(), m_extensionBuffer.size(), "%s", summary.c_str());
    m_extensionSummary->setData(m_extensionBuffer.data());
}

void TypesOwnershipPage::applyOptionFlags()
{
    const bool hasTypeLetters = m_typeOptions.typeEnabled && m_typeOptions.typeLetters[0] != '\0';
    const bool hasXTypeLetters = m_typeOptions.xtypeEnabled && m_typeOptions.xtypeLetters[0] != '\0';
    const bool hasTypeFilters = hasTypeLetters || hasXTypeLetters ||
                                m_typeOptions.useExtensions || m_typeOptions.useDetectors;
    if (hasTypeFilters)
        m_state.optionPrimaryFlags |= kOptionTypeBit;
    else
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionTypeBit);

    const bool hasPermFilters = m_permOptions.permEnabled || m_permOptions.readable ||
                                m_permOptions.writable || m_permOptions.executable;
    const bool hasOwnerFilters = m_permOptions.userEnabled || m_permOptions.uidEnabled ||
                                 m_permOptions.groupEnabled || m_permOptions.gidEnabled ||
                                 m_permOptions.noUser || m_permOptions.noGroup;
    if (hasPermFilters || hasOwnerFilters)
        m_state.optionSecondaryFlags |= kOptionPermissionBit;
    else
        m_state.optionSecondaryFlags &= static_cast<unsigned short>(~kOptionPermissionBit);
}

TraversalPage::TraversalPage(const TRect &bounds,
                             SearchNotebookState &state,
                             TraversalFilesystemOptions &options)
    : ck::ui::TabPageView(bounds),
      m_state(state),
      m_options(options)
{
    insert(new TStaticText(TRect(2, 0, 78, 1),
                           "Control how ck-find walks directories and limits traversal scope."));

    m_symlinkButtons = new TRadioButtons(TRect(2, 1, 26, 5),
                                         makeItemList({"Physical walk (-~P~)",
                                                       "Follow args only (-~H~)",
                                                       "Follow all symlinks (-~L~)"}));
    insert(m_symlinkButtons);

    m_warningButtons = new TRadioButtons(TRect(28, 1, 56, 5),
                                         makeItemList({"Default warnings",
                                                       "Always warn (-warn)",
                                                       "Suppress warn (-nowarn)"}));
    insert(m_warningButtons);

    m_flagBoxes = new TCheckBoxes(TRect(2, 5, 28, 11),
                                  makeItemList({"Use -~d~epth",
                                                "Stay on file~s~ystem",
                                                "Assume -nolea~f~",
                                                "Ignore readdir race",
                                                "Use -day~s~tart"}));
    insert(m_flagBoxes);

    m_valueBoxes = new TCheckBoxes(TRect(28, 5, 56, 13),
                                   makeItemList({"Limit ~m~ax depth",
                                                 "Limit mi~n~ depth",
                                                 "Paths from ~f~ile",
                                                 "List is NU~L~-separated",
                                                 "Filter ~f~stype",
                                                 "Match link ~c~ount",
                                                 "Match ~s~amefile",
                                                 "Match ~i~node"}));
    insert(m_valueBoxes);

    m_maxDepthInput = new TInputLine(TRect(58, 6, 78, 7), static_cast<int>(m_options.maxDepth.size()) - 1);
    insert(new TLabel(TRect(58, 5, 78, 6), "Max depth:", m_maxDepthInput));
    insert(m_maxDepthInput);

    m_minDepthInput = new TInputLine(TRect(58, 8, 78, 9), static_cast<int>(m_options.minDepth.size()) - 1);
    insert(new TLabel(TRect(58, 7, 78, 8), "Min depth:", m_minDepthInput));
    insert(m_minDepthInput);

    m_filesFromInput = new TInputLine(TRect(2, 13, 60, 14), std::min<int>(static_cast<int>(m_options.filesFrom.size()) - 1, 255));
    insert(new TLabel(TRect(2, 12, 60, 13), "-files-from list:", m_filesFromInput));
    insert(m_filesFromInput);

    m_fsTypeInput = new TInputLine(TRect(62, 13, 78, 14), static_cast<int>(m_options.fsType.size()) - 1);
    insert(new TLabel(TRect(60, 12, 78, 13), "fstype:", m_fsTypeInput));
    insert(m_fsTypeInput);

    m_linkCountInput = new TInputLine(TRect(62, 14, 78, 15), static_cast<int>(m_options.linkCount.size()) - 1);
    insert(new TLabel(TRect(60, 13, 78, 14), "Links:", m_linkCountInput));
    insert(m_linkCountInput);

    m_sameFileInput = new TInputLine(TRect(2, 15, 60, 16), std::min<int>(static_cast<int>(m_options.sameFile.size()) - 1, 255));
    insert(new TLabel(TRect(2, 14, 60, 15), "-samefile target:", m_sameFileInput));
    insert(m_sameFileInput);

    m_inodeInput = new TInputLine(TRect(62, 15, 78, 16), static_cast<int>(m_options.inode.size()) - 1);
    insert(new TLabel(TRect(60, 14, 78, 15), "Inode:", m_inodeInput));
    insert(m_inodeInput);

    insert(new TButton(TRect(60, 17, 78, 19), "Advanced ~t~raversal...", cmTraversalFilters, bfNormal));
    m_clearButton = new TButton(TRect(42, 17, 60, 19), "Clear traversal", cmClearTraversalFiltersLocal, bfNormal);
    insert(m_clearButton);

    insert(new TStaticText(TRect(2, 17, 40, 19), "Tip: depth, fstype, and samefile can impact performance."));

    populate();
}

void TraversalPage::populate()
{
    if (m_symlinkButtons)
    {
        unsigned short mode = static_cast<unsigned short>(m_options.symlinkMode);
        m_symlinkButtons->setData(&mode);
    }

    if (m_warningButtons)
    {
        unsigned short warn = static_cast<unsigned short>(m_options.warningMode);
        m_warningButtons->setData(&warn);
    }

    unsigned short flagBits = 0;
    if (m_options.depthFirst)
        flagBits |= 0x0001;
    if (m_options.stayOnFilesystem)
        flagBits |= 0x0002;
    if (m_options.assumeNoLeaf)
        flagBits |= 0x0004;
    if (m_options.ignoreReaddirRace)
        flagBits |= 0x0008;
    if (m_options.dayStart)
        flagBits |= 0x0010;
    if (m_flagBoxes)
        m_flagBoxes->setData(&flagBits);

    unsigned short valueBits = 0;
    if (m_options.maxDepthEnabled)
        valueBits |= 0x0001;
    if (m_options.minDepthEnabled)
        valueBits |= 0x0002;
    if (m_options.filesFromEnabled)
        valueBits |= 0x0004;
    if (m_options.filesFromNullSeparated)
        valueBits |= 0x0008;
    if (m_options.fstypeEnabled)
        valueBits |= 0x0010;
    if (m_options.linksEnabled)
        valueBits |= 0x0020;
    if (m_options.sameFileEnabled)
        valueBits |= 0x0040;
    if (m_options.inumEnabled)
        valueBits |= 0x0080;
    if (m_valueBoxes)
        m_valueBoxes->setData(&valueBits);

    if (m_maxDepthInput)
        m_maxDepthInput->setData(m_options.maxDepth.data());
    if (m_minDepthInput)
        m_minDepthInput->setData(m_options.minDepth.data());
    if (m_filesFromInput)
        m_filesFromInput->setData(m_options.filesFrom.data());
    if (m_fsTypeInput)
        m_fsTypeInput->setData(m_options.fsType.data());
    if (m_linkCountInput)
        m_linkCountInput->setData(m_options.linkCount.data());
    if (m_sameFileInput)
        m_sameFileInput->setData(m_options.sameFile.data());
    if (m_inodeInput)
        m_inodeInput->setData(m_options.inode.data());

    updateValueControls();
    updateFlags();
}

void TraversalPage::collect()
{
    unsigned short mode = 0;
    if (m_symlinkButtons)
    {
        m_symlinkButtons->getData(&mode);
        m_options.symlinkMode = static_cast<TraversalFilesystemOptions::SymlinkMode>(mode);
    }

    unsigned short warn = 0;
    if (m_warningButtons)
    {
        m_warningButtons->getData(&warn);
        m_options.warningMode = static_cast<TraversalFilesystemOptions::WarningMode>(warn);
    }

    unsigned short flagBits = 0;
    if (m_flagBoxes)
        m_flagBoxes->getData(&flagBits);
    m_options.depthFirst = (flagBits & 0x0001) != 0;
    m_options.stayOnFilesystem = (flagBits & 0x0002) != 0;
    m_options.assumeNoLeaf = (flagBits & 0x0004) != 0;
    m_options.ignoreReaddirRace = (flagBits & 0x0008) != 0;
    m_options.dayStart = (flagBits & 0x0010) != 0;

    unsigned short valueBits = 0;
    if (m_valueBoxes)
        m_valueBoxes->getData(&valueBits);
    m_options.maxDepthEnabled = (valueBits & 0x0001) != 0;
    m_options.minDepthEnabled = (valueBits & 0x0002) != 0;
    m_options.filesFromEnabled = (valueBits & 0x0004) != 0;
    m_options.fstypeEnabled = (valueBits & 0x0010) != 0;
    m_options.linksEnabled = (valueBits & 0x0020) != 0;
    m_options.sameFileEnabled = (valueBits & 0x0040) != 0;
    m_options.inumEnabled = (valueBits & 0x0080) != 0;

    bool filesFromNull = (valueBits & 0x0008) != 0;
    m_options.filesFromNullSeparated = m_options.filesFromEnabled && filesFromNull;

    if (m_options.maxDepthEnabled && m_maxDepthInput)
        m_maxDepthInput->getData(m_options.maxDepth.data());
    else
        m_options.maxDepth.fill('\0');

    if (m_options.minDepthEnabled && m_minDepthInput)
        m_minDepthInput->getData(m_options.minDepth.data());
    else
        m_options.minDepth.fill('\0');

    if (m_options.filesFromEnabled && m_filesFromInput)
        m_filesFromInput->getData(m_options.filesFrom.data());
    else
        m_options.filesFrom.fill('\0');

    if (m_options.fstypeEnabled && m_fsTypeInput)
        m_fsTypeInput->getData(m_options.fsType.data());
    else
        m_options.fsType.fill('\0');

    if (m_options.linksEnabled && m_linkCountInput)
        m_linkCountInput->getData(m_options.linkCount.data());
    else
        m_options.linkCount.fill('\0');

    if (m_options.sameFileEnabled && m_sameFileInput)
        m_sameFileInput->getData(m_options.sameFile.data());
    else
        m_options.sameFile.fill('\0');

    if (m_options.inumEnabled && m_inodeInput)
        m_inodeInput->getData(m_options.inode.data());
    else
        m_options.inode.fill('\0');

    updateValueControls();
    updateFlags();
}

void TraversalPage::onActivated()
{
    populate();
}

void TraversalPage::onDeactivated()
{
    collect();
}

void TraversalPage::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmClearTraversalFiltersLocal:
            m_options = TraversalFilesystemOptions{};
            updateFlags();
            populate();
            clearEvent(event);
            return;
        default:
            break;
        }
    }

    TGroup::handleEvent(event);
    updateValueControls();
}

void TraversalPage::updateValueControls()
{
    if (!m_valueBoxes)
        return;

    unsigned short flags = 0;
    m_valueBoxes->getData(&flags);

    const bool maxEnabled = (flags & 0x0001) != 0;
    const bool minEnabled = (flags & 0x0002) != 0;
    bool filesFromEnabled = (flags & 0x0004) != 0;
    bool nullSeparated = (flags & 0x0008) != 0;
    const bool fstypeEnabled = (flags & 0x0010) != 0;
    const bool linksEnabled = (flags & 0x0020) != 0;
    const bool sameFileEnabled = (flags & 0x0040) != 0;
    const bool inodeEnabled = (flags & 0x0080) != 0;

    if (!filesFromEnabled && nullSeparated)
    {
        flags &= static_cast<unsigned short>(~0x0008);
        m_valueBoxes->setData(&flags);
        nullSeparated = false;
    }

    if (m_maxDepthInput)
        m_maxDepthInput->setState(sfDisabled, maxEnabled ? False : True);
    if (m_minDepthInput)
        m_minDepthInput->setState(sfDisabled, minEnabled ? False : True);
    if (m_filesFromInput)
        m_filesFromInput->setState(sfDisabled, filesFromEnabled ? False : True);
    if (m_fsTypeInput)
        m_fsTypeInput->setState(sfDisabled, fstypeEnabled ? False : True);
    if (m_linkCountInput)
        m_linkCountInput->setState(sfDisabled, linksEnabled ? False : True);
    if (m_sameFileInput)
        m_sameFileInput->setState(sfDisabled, sameFileEnabled ? False : True);
    if (m_inodeInput)
        m_inodeInput->setState(sfDisabled, inodeEnabled ? False : True);
}

void TraversalPage::updateFlags()
{
    if (m_options.symlinkMode == TraversalFilesystemOptions::SymlinkMode::Everywhere)
        m_state.generalFlags |= kGeneralSymlinkBit;
    else
        m_state.generalFlags &= static_cast<unsigned short>(~kGeneralSymlinkBit);

    if (m_options.stayOnFilesystem)
        m_state.generalFlags |= kGeneralStayOnFsBit;
    else
        m_state.generalFlags &= static_cast<unsigned short>(~kGeneralStayOnFsBit);

    const bool traversalActive = m_options.depthFirst || m_options.stayOnFilesystem ||
                                 m_options.assumeNoLeaf || m_options.ignoreReaddirRace ||
                                 m_options.dayStart || m_options.maxDepthEnabled ||
                                 m_options.minDepthEnabled || m_options.filesFromEnabled ||
                                 m_options.filesFromNullSeparated || m_options.fstypeEnabled ||
                                 m_options.linksEnabled || m_options.sameFileEnabled ||
                                 m_options.inumEnabled ||
                                 m_options.symlinkMode != TraversalFilesystemOptions::SymlinkMode::Physical ||
                                 m_options.warningMode != TraversalFilesystemOptions::WarningMode::Default;

    if (traversalActive)
        m_state.optionSecondaryFlags |= kOptionTraversalBit;
    else
        m_state.optionSecondaryFlags &= static_cast<unsigned short>(~kOptionTraversalBit);
}

ActionsPage::ActionsPage(const TRect &bounds,
                         SearchNotebookState &state,
                         ActionOptions &options)
    : ck::ui::TabPageView(bounds),
      m_state(state),
      m_options(options)
{
    insert(new TStaticText(TRect(2, 0, 78, 1),
                           "Select outputs for matches or run commands on each result."));

    m_outputBoxes = new TCheckBoxes(TRect(2, 1, 24, 7),
                                    makeItemList({"Print (-print)",
                                                  "Print\0 (-print0)",
                                                  "Verbose list (-ls)",
                                                  "Delete matches",
                                                  "Stop after first (-quit)"}));
    insert(m_outputBoxes);

    m_execBoxes = new TCheckBoxes(TRect(26, 1, 52, 4),
                                  makeItemList({"Run command on matches (-exec/-ok)",
                                                "Use '+' terminator"}));
    insert(m_execBoxes);

    m_execVariantButtons = new TRadioButtons(TRect(26, 4, 52, 8),
                                             makeItemList({"-exec",
                                                           "-execdir",
                                                           "-ok",
                                                           "-okdir"}));
    insert(m_execVariantButtons);

    m_execInput = new TInputLine(TRect(2, 7, 78, 8), static_cast<int>(m_options.execCommand.size()) - 1);
    insert(new TLabel(TRect(2, 6, 78, 7), "Command template (use {} for path):", m_execInput));
    insert(m_execInput);

    insert(new TStaticText(TRect(2, 8, 78, 9), "File outputs"));

    m_fileToggleBoxes = new TCheckBoxes(TRect(2, 9, 28, 15),
                                        makeItemList({"Enable -fprint",
                                                      "Enable -fprint0",
                                                      "Enable -fls",
                                                      "Enable -printf",
                                                      "Enable -fprintf"}));
    insert(m_fileToggleBoxes);

    m_appendBoxes = new TCheckBoxes(TRect(30, 9, 52, 13),
                                    makeItemList({"Append -fprint",
                                                  "Append -fprint0",
                                                  "Append -fls",
                                                  "Append -fprintf"}));
    insert(m_appendBoxes);

    const int pathLen = std::min<int>(static_cast<int>(m_options.fprintFile.size()) - 1, 255);
    m_fprintInput = new TInputLine(TRect(54, 9, 78, 10), pathLen);
    insert(new TLabel(TRect(54, 8, 78, 9), "-fprint file:", m_fprintInput));
    insert(m_fprintInput);

    m_fprint0Input = new TInputLine(TRect(54, 10, 78, 11), pathLen);
    insert(new TLabel(TRect(54, 9, 78, 10), "-fprint0 file:", m_fprint0Input));
    insert(m_fprint0Input);

    m_flsInput = new TInputLine(TRect(54, 11, 78, 12), pathLen);
    insert(new TLabel(TRect(54, 10, 78, 11), "-fls file:", m_flsInput));
    insert(m_flsInput);

    m_printfInput = new TInputLine(TRect(30, 12, 78, 13), static_cast<int>(m_options.printfFormat.size()) - 1);
    insert(new TLabel(TRect(30, 11, 56, 12), "-printf format:", m_printfInput));
    insert(m_printfInput);

    m_fprintfFileInput = new TInputLine(TRect(30, 13, 54, 14), pathLen);
    insert(new TLabel(TRect(30, 12, 54, 13), "-fprintf file:", m_fprintfFileInput));
    insert(m_fprintfFileInput);

    m_fprintfFormatInput = new TInputLine(TRect(56, 13, 78, 14), static_cast<int>(m_options.fprintfFormat.size()) - 1);
    insert(new TLabel(TRect(56, 12, 78, 13), "-fprintf format:", m_fprintfFormatInput));
    insert(m_fprintfFormatInput);

    m_warningText = new TStaticText(TRect(2, 15, 78, 16),
                                    "Warning: Delete or Exec options can remove or modify files.");
    insert(m_warningText);

    insert(new TButton(TRect(2, 17, 22, 19), "Advanced ~a~ctions...", cmActionOptions, bfNormal));
    m_clearButton = new TButton(TRect(24, 17, 42, 19), "Clear actions", cmClearActionsLocal, bfNormal);
    insert(m_clearButton);

    populate();
}

void ActionsPage::populate()
{
    if (m_outputBoxes)
    {
        unsigned short bits = 0;
        if (m_options.print)
            bits |= 0x0001;
        if (m_options.print0)
            bits |= 0x0002;
        if (m_options.ls)
            bits |= 0x0004;
        if (m_options.deleteMatches)
            bits |= 0x0008;
        if (m_options.quitEarly)
            bits |= 0x0010;
        m_outputBoxes->setData(&bits);
    }

    if (m_execBoxes)
    {
        unsigned short bits = 0;
        if (m_options.execEnabled)
            bits |= 0x0001;
        if (m_options.execUsePlus && m_options.execEnabled)
            bits |= 0x0002;
        m_execBoxes->setData(&bits);
    }

    if (m_execVariantButtons)
    {
        unsigned short variant = static_cast<unsigned short>(m_options.execVariant);
        m_execVariantButtons->setData(&variant);
    }

    if (m_execInput)
        m_execInput->setData(m_options.execCommand.data());

    if (m_fileToggleBoxes)
    {
        unsigned short bits = 0;
        if (m_options.fprintEnabled)
            bits |= 0x0001;
        if (m_options.fprint0Enabled)
            bits |= 0x0002;
        if (m_options.flsEnabled)
            bits |= 0x0004;
        if (m_options.printfEnabled)
            bits |= 0x0008;
        if (m_options.fprintfEnabled)
            bits |= 0x0010;
        m_fileToggleBoxes->setData(&bits);
    }

    if (m_appendBoxes)
    {
        unsigned short bits = 0;
        if (m_options.fprintAppend)
            bits |= 0x0001;
        if (m_options.fprint0Append)
            bits |= 0x0002;
        if (m_options.flsAppend)
            bits |= 0x0004;
        if (m_options.fprintfAppend)
            bits |= 0x0008;
        m_appendBoxes->setData(&bits);
    }

    if (m_fprintInput)
        m_fprintInput->setData(m_options.fprintFile.data());
    if (m_fprint0Input)
        m_fprint0Input->setData(m_options.fprint0File.data());
    if (m_flsInput)
        m_flsInput->setData(m_options.flsFile.data());
    if (m_printfInput)
        m_printfInput->setData(m_options.printfFormat.data());
    if (m_fprintfFileInput)
        m_fprintfFileInput->setData(m_options.fprintfFile.data());
    if (m_fprintfFormatInput)
        m_fprintfFormatInput->setData(m_options.fprintfFormat.data());

    updateExecControls();
    updateFileOutputs();
    updateWarning();
    applyOptionFlags();
}

void ActionsPage::collect()
{
    if (m_outputBoxes)
    {
        unsigned short bits = 0;
        m_outputBoxes->getData(&bits);
        m_options.print = (bits & 0x0001) != 0;
        m_options.print0 = (bits & 0x0002) != 0;
        m_options.ls = (bits & 0x0004) != 0;
        m_options.deleteMatches = (bits & 0x0008) != 0;
        m_options.quitEarly = (bits & 0x0010) != 0;
    }

    bool execEnabled = false;
    if (m_execBoxes)
    {
        unsigned short bits = 0;
        m_execBoxes->getData(&bits);
        execEnabled = (bits & 0x0001) != 0;
        m_options.execEnabled = execEnabled;
        m_options.execUsePlus = execEnabled && ((bits & 0x0002) != 0);
    }
    else
    {
        m_options.execEnabled = false;
        m_options.execUsePlus = false;
    }

    if (m_execVariantButtons)
    {
        unsigned short variant = 0;
        m_execVariantButtons->getData(&variant);
        m_options.execVariant = static_cast<ActionOptions::ExecVariant>(variant);
    }

    if (execEnabled && m_execInput)
        m_execInput->getData(m_options.execCommand.data());
    else
        m_options.execCommand.fill('\0');

    unsigned short fileBits = 0;
    if (m_fileToggleBoxes)
        m_fileToggleBoxes->getData(&fileBits);

    unsigned short appendBits = 0;
    if (m_appendBoxes)
        m_appendBoxes->getData(&appendBits);

    m_options.fprintEnabled = (fileBits & 0x0001) != 0;
    m_options.fprint0Enabled = (fileBits & 0x0002) != 0;
    m_options.flsEnabled = (fileBits & 0x0004) != 0;
    m_options.printfEnabled = (fileBits & 0x0008) != 0;
    m_options.fprintfEnabled = (fileBits & 0x0010) != 0;

    m_options.fprintAppend = m_options.fprintEnabled && ((appendBits & 0x0001) != 0);
    m_options.fprint0Append = m_options.fprint0Enabled && ((appendBits & 0x0002) != 0);
    m_options.flsAppend = m_options.flsEnabled && ((appendBits & 0x0004) != 0);
    m_options.fprintfAppend = m_options.fprintfEnabled && ((appendBits & 0x0008) != 0);

    if (m_options.fprintEnabled && m_fprintInput)
        m_fprintInput->getData(m_options.fprintFile.data());
    else
        m_options.fprintFile.fill('\0');

    if (m_options.fprint0Enabled && m_fprint0Input)
        m_fprint0Input->getData(m_options.fprint0File.data());
    else
        m_options.fprint0File.fill('\0');

    if (m_options.flsEnabled && m_flsInput)
        m_flsInput->getData(m_options.flsFile.data());
    else
        m_options.flsFile.fill('\0');

    if (m_options.printfEnabled && m_printfInput)
        m_printfInput->getData(m_options.printfFormat.data());
    else
        m_options.printfFormat.fill('\0');

    if (m_options.fprintfEnabled)
    {
        if (m_fprintfFileInput)
            m_fprintfFileInput->getData(m_options.fprintfFile.data());
        if (m_fprintfFormatInput)
            m_fprintfFormatInput->getData(m_options.fprintfFormat.data());
    }
    else
    {
        m_options.fprintfFile.fill('\0');
        m_options.fprintfFormat.fill('\0');
    }

    updateExecControls();
    updateFileOutputs();
    updateWarning();
    applyOptionFlags();
}

void ActionsPage::onActivated()
{
    populate();
}

void ActionsPage::onDeactivated()
{
    collect();
}

void ActionsPage::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmClearActionsLocal:
            m_options = ActionOptions{};
            populate();
            clearEvent(event);
            return;
        default:
            break;
        }
    }

    TGroup::handleEvent(event);
    updateExecControls();
    updateFileOutputs();
    updateWarning();
}

void ActionsPage::updateExecControls()
{
    if (!m_execBoxes)
        return;

    unsigned short bits = 0;
    m_execBoxes->getData(&bits);
    if ((bits & 0x0001) == 0 && (bits & 0x0002) != 0)
    {
        bits &= static_cast<unsigned short>(~0x0002);
        m_execBoxes->setData(&bits);
    }

    const Boolean execDisabled = (bits & 0x0001) != 0 ? False : True;

    if (m_execVariantButtons)
        m_execVariantButtons->setState(sfDisabled, execDisabled);
    if (m_execInput)
        m_execInput->setState(sfDisabled, execDisabled);
}

void ActionsPage::updateFileOutputs()
{
    if (!m_fileToggleBoxes)
        return;

    unsigned short fileBits = 0;
    m_fileToggleBoxes->getData(&fileBits);

    unsigned short appendBits = 0;
    if (m_appendBoxes)
        m_appendBoxes->getData(&appendBits);

    const Boolean fprintDisabled = (fileBits & 0x0001) != 0 ? False : True;
    const Boolean fprint0Disabled = (fileBits & 0x0002) != 0 ? False : True;
    const Boolean flsDisabled = (fileBits & 0x0004) != 0 ? False : True;
    const Boolean printfDisabled = (fileBits & 0x0008) != 0 ? False : True;
    const Boolean fprintfDisabled = (fileBits & 0x0010) != 0 ? False : True;

    if (fprintDisabled)
        appendBits &= static_cast<unsigned short>(~0x0001);
    if (fprint0Disabled)
        appendBits &= static_cast<unsigned short>(~0x0002);
    if (flsDisabled)
        appendBits &= static_cast<unsigned short>(~0x0004);
    if (fprintfDisabled)
        appendBits &= static_cast<unsigned short>(~0x0008);

    if (m_appendBoxes)
        m_appendBoxes->setData(&appendBits);

    if (m_fprintInput)
        m_fprintInput->setState(sfDisabled, fprintDisabled);
    if (m_fprint0Input)
        m_fprint0Input->setState(sfDisabled, fprint0Disabled);
    if (m_flsInput)
        m_flsInput->setState(sfDisabled, flsDisabled);
    if (m_printfInput)
        m_printfInput->setState(sfDisabled, printfDisabled);
    if (m_fprintfFileInput)
        m_fprintfFileInput->setState(sfDisabled, fprintfDisabled);
    if (m_fprintfFormatInput)
        m_fprintfFormatInput->setState(sfDisabled, fprintfDisabled);
}

void ActionsPage::updateWarning()
{
    if (!m_warningText)
        return;

    bool destructive = false;
    if (m_outputBoxes)
    {
        unsigned short bits = 0;
        m_outputBoxes->getData(&bits);
        destructive = (bits & 0x0008) != 0;
    }
    if (!destructive && m_execBoxes)
    {
        unsigned short bits = 0;
        m_execBoxes->getData(&bits);
        if (bits & 0x0001)
        {
            char command[512]{};
            if (m_execInput)
                m_execInput->getData(command);
            destructive = command[0] != '\0';
        }
    }

    m_warningText->setState(sfVisible, destructive ? True : False);
}

void ActionsPage::applyOptionFlags()
{
    const bool outputActive = m_options.print || m_options.print0 || m_options.ls ||
                              m_options.deleteMatches || m_options.quitEarly;
    const bool execActive = m_options.execEnabled && m_options.execCommand[0] != '\0';
    const bool fileOutputsActive = m_options.fprintEnabled || m_options.fprint0Enabled ||
                                   m_options.flsEnabled || m_options.printfEnabled ||
                                   m_options.fprintfEnabled;

    if (outputActive || execActive || fileOutputsActive)
        m_state.optionSecondaryFlags |= kOptionActionBit;
    else
        m_state.optionSecondaryFlags &= static_cast<unsigned short>(~kOptionActionBit);
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
    DatesSizesPage *m_datesPage = nullptr;
    TypesOwnershipPage *m_typesPage = nullptr;
    TraversalPage *m_traversalPage = nullptr;
    ActionsPage *m_actionsPage = nullptr;
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

    m_contentPage = new ContentNamesPage(TRect(0, 0, 81, 20), m_state, m_spec.textOptions, m_spec.namePathOptions, m_spec.typeOptions);
    m_tabControl->addTab("Content", m_contentPage, cmTabContentNames);

    m_datesPage = new DatesSizesPage(TRect(0, 0, 81, 20), m_state, m_spec.timeOptions, m_spec.sizeOptions);
    m_tabControl->addTab("Dates", m_datesPage, cmTabDatesSizes);

    m_typesPage = new TypesOwnershipPage(TRect(0, 0, 81, 20), m_state, m_spec.typeOptions, m_spec.permissionOptions);
    m_tabControl->addTab("Types", m_typesPage, cmTabTypesOwnership);

    m_traversalPage = new TraversalPage(TRect(0, 0, 81, 20), m_state, m_spec.traversalOptions);
    m_tabControl->addTab("Traverse", m_traversalPage, cmTabTraversal);

    m_actionsPage = new ActionsPage(TRect(0, 0, 81, 20), m_state, m_spec.actionOptions);
    m_tabControl->addTab("Actions", m_actionsPage, cmTabActions);

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
                if (m_datesPage)
                    m_datesPage->populate();
            }
            clearEvent(event);
            return;
        case cmSizeFilters:
            if (editSizeFilters(m_spec.sizeOptions))
            {
                m_state.optionPrimaryFlags |= kOptionSizeBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
                if (m_datesPage)
                    m_datesPage->populate();
            }
            clearEvent(event);
            return;
        case cmTypeFilters:
            if (editTypeFilters(m_spec.typeOptions))
            {
                m_state.optionPrimaryFlags |= kOptionTypeBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
                if (m_contentPage)
                    m_contentPage->populate();
                if (m_typesPage)
                    m_typesPage->populate();
            }
            clearEvent(event);
            return;
        case cmPermissionOwnership:
            if (editPermissionOwnership(m_spec.permissionOptions))
            {
                m_state.optionSecondaryFlags |= kOptionPermissionBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
                if (m_typesPage)
                    m_typesPage->populate();
            }
            clearEvent(event);
            return;
        case cmTraversalFilters:
            if (editTraversalFilters(m_spec.traversalOptions))
            {
                m_state.optionSecondaryFlags |= kOptionTraversalBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
                if (m_traversalPage)
                    m_traversalPage->populate();
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
        if (m_datesPage)
            m_datesPage->collect();
        if (m_typesPage)
            m_typesPage->collect();
        if (m_traversalPage)
            m_traversalPage->collect();
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

    m_spec.enableTextSearch = (m_state.optionPrimaryFlags & kOptionTextBit) != 0;
    if (!m_spec.enableTextSearch)
    {
        m_spec.textOptions.searchInContents = false;
        m_spec.textOptions.searchInFileNames = false;
    }
}

void SearchNotebookDialog::applyQuickSelections()
{
    const bool hasText = m_state.searchText[0] != '\0';
    if (!hasText)
    {
        m_spec.textOptions.searchInContents = false;
        m_spec.textOptions.searchInFileNames = false;
        m_state.optionPrimaryFlags &= static_cast<unsigned short>(~kOptionTextBit);
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
