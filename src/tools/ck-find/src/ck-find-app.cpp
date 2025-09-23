#include "ck/about_dialog.hpp"
#include "ck/app_info.hpp"
#include "ck/launcher.hpp"
#include "ck/find/cli_buffer_utils.hpp"

#define Uses_MsgBox
#define Uses_TApplication
#define Uses_TButton
#define Uses_TCheckBoxes
#define Uses_TDeskTop
#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TKeys
#define Uses_TLabel
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TMessageBox
#define Uses_TProgram
#define Uses_TRadioButtons
#define Uses_TStaticText
#define Uses_TSItem
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#include <tvision/tv.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace
{
constexpr std::string_view kToolId = "ck-find";

const ck::appinfo::ToolInfo &toolInfo()
{
    return ck::appinfo::requireTool(kToolId);
}

enum CommandId : ushort
{
    cmNewSearch = 1000,
    cmLoadSpec,
    cmSaveSpec,
    cmReturnToLauncher,
    cmAbout,
    cmBrowseStart,
    cmTextOptions,
    cmNamePathOptions,
    cmTimeFilters,
    cmSizeFilters,
    cmTypeFilters,
    cmPermissionOwnership,
    cmTraversalFilters,
    cmActionOptions,
    cmDialogLoadSpec,
    cmDialogSaveSpec,
};

constexpr ushort kGeneralRecursiveBit = 0x0001;
constexpr ushort kGeneralHiddenBit = 0x0002;
constexpr ushort kGeneralSymlinkBit = 0x0004;
constexpr ushort kGeneralStayOnFsBit = 0x0008;

constexpr ushort kOptionTextBit = 0x0001;
constexpr ushort kOptionNamePathBit = 0x0002;
constexpr ushort kOptionTimeBit = 0x0004;
constexpr ushort kOptionSizeBit = 0x0008;
constexpr ushort kOptionTypeBit = 0x0010;

constexpr ushort kOptionPermissionBit = 0x0001;
constexpr ushort kOptionTraversalBit = 0x0002;
constexpr ushort kOptionActionBit = 0x0004;

using ck::find::bufferToString;
using ck::find::copyToArray;

TSItem *makeItemList(std::initializer_list<const char *> labels)
{
    TSItem *head = nullptr;
    TSItem **tail = &head;
    for (const char *text : labels)
    {
        *tail = new TSItem(text, nullptr);
        tail = &(*tail)->next;
    }
    return head;
}

struct TextSearchOptions
{
    enum class Mode : ushort
    {
        Contains = 0,
        WholeWord = 1,
        RegularExpression = 2
    };

    Mode mode = Mode::Contains;
    bool matchCase = false;
    bool searchInContents = true;
    bool searchInFileNames = true;
    bool allowMultipleTerms = false;
    bool treatBinaryAsText = false;
};

struct NamePathOptions
{
    enum class PruneTest : ushort
    {
        Name = 0,
        Iname,
        Path,
        Ipath,
        Regex,
        Iregex
    };

    bool nameEnabled = false;
    bool inameEnabled = false;
    bool pathEnabled = false;
    bool ipathEnabled = false;
    bool regexEnabled = false;
    bool iregexEnabled = false;
    bool lnameEnabled = false;
    bool ilnameEnabled = false;
    bool pruneEnabled = false;
    bool pruneDirectoriesOnly = true;
    PruneTest pruneTest = PruneTest::Path;
    std::array<char, 256> namePattern{};
    std::array<char, 256> inamePattern{};
    std::array<char, 256> pathPattern{};
    std::array<char, 256> ipathPattern{};
    std::array<char, 256> regexPattern{};
    std::array<char, 256> iregexPattern{};
    std::array<char, 256> lnamePattern{};
    std::array<char, 256> ilnamePattern{};
    std::array<char, 256> prunePattern{};
};

struct TimeFilterOptions
{
    enum class Preset : ushort
    {
        AnyTime = 0,
        PastDay,
        PastWeek,
        PastMonth,
        PastSixMonths,
        PastYear,
        PastSixYears,
        CustomRange
    };

    Preset preset = Preset::AnyTime;
    bool includeModified = true;
    bool includeCreated = false;
    bool includeAccessed = false;
    std::array<char, 32> customFrom{};
    std::array<char, 32> customTo{};

    bool useMTime = false;
    bool useATime = false;
    bool useCTime = false;
    bool useMMin = false;
    bool useAMin = false;
    bool useCMin = false;
    bool useUsed = false;
    bool useNewer = false;
    bool useANewer = false;
    bool useCNewer = false;
    bool useNewermt = false;
    bool useNewerat = false;
    bool useNewerct = false;
    std::array<char, 16> mtime{};
    std::array<char, 16> atime{};
    std::array<char, 16> ctime{};
    std::array<char, 16> mmin{};
    std::array<char, 16> amin{};
    std::array<char, 16> cmin{};
    std::array<char, 16> used{};
    std::array<char, PATH_MAX> newer{};
    std::array<char, PATH_MAX> anewer{};
    std::array<char, PATH_MAX> cnewer{};
    std::array<char, 64> newermt{};
    std::array<char, 64> newerat{};
    std::array<char, 64> newerct{};
};

struct SizeFilterOptions
{
    bool minEnabled = false;
    bool maxEnabled = false;
    bool exactEnabled = false;
    bool rangeInclusive = true;
    bool includeZeroByte = true;
    bool treatDirectoriesAsFiles = false;
    bool useDecimalUnits = false;
    bool emptyEnabled = false;
    std::array<char, 32> minSpec{};
    std::array<char, 32> maxSpec{};
    std::array<char, 32> exactSpec{};
};

struct TypeFilterOptions
{
    bool typeEnabled = false;
    bool xtypeEnabled = false;
    bool useExtensions = false;
    bool extensionCaseInsensitive = true;
    bool useDetectors = false;
    std::array<char, 16> typeLetters{};
    std::array<char, 16> xtypeLetters{};
    std::array<char, 256> extensions{};
    std::array<char, 256> detectorTags{};
};

struct PermissionOwnershipOptions
{
    enum class PermMode : ushort
    {
        Exact = 0,
        AllBits,
        AnyBit
    };

    bool permEnabled = false;
    bool readable = false;
    bool writable = false;
    bool executable = false;
    PermMode permMode = PermMode::Exact;
    std::array<char, 16> permSpec{};

    bool userEnabled = false;
    bool uidEnabled = false;
    bool groupEnabled = false;
    bool gidEnabled = false;
    bool noUser = false;
    bool noGroup = false;
    std::array<char, 64> user{};
    std::array<char, 32> uid{};
    std::array<char, 64> group{};
    std::array<char, 32> gid{};
};

struct TraversalFilesystemOptions
{
    enum class SymlinkMode : ushort
    {
        Physical = 0,
        CommandLine,
        Everywhere
    };

    enum class WarningMode : ushort
    {
        Default = 0,
        ForceWarn,
        SuppressWarn
    };

    SymlinkMode symlinkMode = SymlinkMode::Physical;
    WarningMode warningMode = WarningMode::Default;
    bool depthFirst = false;
    bool stayOnFilesystem = false;
    bool assumeNoLeaf = false;
    bool ignoreReaddirRace = false;
    bool dayStart = false;
    bool maxDepthEnabled = false;
    bool minDepthEnabled = false;
    bool filesFromEnabled = false;
    bool filesFromNullSeparated = false;
    bool fstypeEnabled = false;
    bool linksEnabled = false;
    bool sameFileEnabled = false;
    bool inumEnabled = false;
    std::array<char, 8> maxDepth{};
    std::array<char, 8> minDepth{};
    std::array<char, PATH_MAX> filesFrom{};
    std::array<char, 64> fsType{};
    std::array<char, 16> linkCount{};
    std::array<char, PATH_MAX> sameFile{};
    std::array<char, 32> inode{};
};

struct ActionOptions
{
    enum class ExecVariant : ushort
    {
        Exec = 0,
        ExecDir,
        Ok,
        OkDir
    };

    bool print = true;
    bool print0 = false;
    bool ls = false;
    bool deleteMatches = false;
    bool quitEarly = false;
    bool execEnabled = false;
    bool execUsePlus = false;
    ExecVariant execVariant = ExecVariant::Exec;
    bool fprintEnabled = false;
    bool fprintAppend = false;
    bool fprint0Enabled = false;
    bool fprint0Append = false;
    bool flsEnabled = false;
    bool flsAppend = false;
    bool printfEnabled = false;
    bool fprintfEnabled = false;
    bool fprintfAppend = false;
    std::array<char, 512> execCommand{};
    std::array<char, PATH_MAX> fprintFile{};
    std::array<char, PATH_MAX> fprint0File{};
    std::array<char, PATH_MAX> flsFile{};
    std::array<char, 256> printfFormat{};
    std::array<char, PATH_MAX> fprintfFile{};
    std::array<char, 256> fprintfFormat{};
};

struct SearchSpecification
{
    std::array<char, 128> specName{};
    std::array<char, PATH_MAX> startLocation{};
    std::array<char, 256> searchText{};
    std::array<char, 256> includePatterns{};
    std::array<char, 256> excludePatterns{};
    bool includeSubdirectories = true;
    bool includeHidden = false;
    bool followSymlinks = false;
    bool stayOnSameFilesystem = false;
    bool enableTextSearch = true;
    bool enableNamePathTests = false;
    bool enableTimeFilters = false;
    bool enableSizeFilters = false;
    bool enableTypeFilters = false;
    bool enablePermissionOwnership = false;
    bool enableTraversalFilters = false;
    bool enableActionOptions = true;
    TextSearchOptions textOptions{};
    NamePathOptions namePathOptions{};
    TimeFilterOptions timeOptions{};
    SizeFilterOptions sizeOptions{};
    TypeFilterOptions typeOptions{};
    PermissionOwnershipOptions permissionOptions{};
    TraversalFilesystemOptions traversalOptions{};
    ActionOptions actionOptions{};
};

SearchSpecification makeDefaultSpecification()
{
    SearchSpecification spec;
    copyToArray(spec.startLocation, ".");
    spec.enableActionOptions = true;
    spec.actionOptions.print = true;
    return spec;
}

bool editTextOptions(TextSearchOptions &options);
bool editNamePathOptions(NamePathOptions &options);
bool editTimeFilters(TimeFilterOptions &options);
bool editSizeFilters(SizeFilterOptions &options);
bool editTypeFilters(TypeFilterOptions &options);
bool editPermissionOwnership(PermissionOwnershipOptions &options);
bool editTraversalFilters(TraversalFilesystemOptions &options);
bool editActionOptions(ActionOptions &options);

struct SearchDialogData
{
    char specName[128]{};
    char startLocation[PATH_MAX]{};
    char searchText[256]{};
    char includePatterns[256]{};
    char excludePatterns[256]{};
    ushort generalFlags = 0;
    ushort optionPrimaryFlags = 0;
    ushort optionSecondaryFlags = 0;
};

class SearchDialog : public TDialog
{
public:
    SearchDialog(SearchSpecification &spec, SearchDialogData &data)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 78, 25), "New Search"),
          m_spec(spec),
          m_data(data)
    {
        options |= ofCentered;
    }

    void setPrimaryBoxes(TCheckBoxes *boxes)
    {
        m_primaryBoxes = boxes;
    }

    void setSecondaryBoxes(TCheckBoxes *boxes)
    {
        m_secondaryBoxes = boxes;
    }

protected:
    void handleEvent(TEvent &event) override
    {
        if (event.what == evCommand)
        {
            switch (event.message.command)
            {
            case cmTextOptions:
                if (editTextOptions(m_spec.textOptions))
                {
                    m_data.optionPrimaryFlags |= kOptionTextBit;
                    if (m_primaryBoxes)
                        m_primaryBoxes->setData(&m_data.optionPrimaryFlags);
                }
                clearEvent(event);
                return;
            case cmNamePathOptions:
                if (editNamePathOptions(m_spec.namePathOptions))
                {
                    m_data.optionPrimaryFlags |= kOptionNamePathBit;
                    if (m_primaryBoxes)
                        m_primaryBoxes->setData(&m_data.optionPrimaryFlags);
                }
                clearEvent(event);
                return;
            case cmTimeFilters:
                if (editTimeFilters(m_spec.timeOptions))
                {
                    m_data.optionPrimaryFlags |= kOptionTimeBit;
                    if (m_primaryBoxes)
                        m_primaryBoxes->setData(&m_data.optionPrimaryFlags);
                }
                clearEvent(event);
                return;
            case cmSizeFilters:
                if (editSizeFilters(m_spec.sizeOptions))
                {
                    m_data.optionPrimaryFlags |= kOptionSizeBit;
                    if (m_primaryBoxes)
                        m_primaryBoxes->setData(&m_data.optionPrimaryFlags);
                }
                clearEvent(event);
                return;
            case cmTypeFilters:
                if (editTypeFilters(m_spec.typeOptions))
                {
                    m_data.optionPrimaryFlags |= kOptionTypeBit;
                    if (m_primaryBoxes)
                        m_primaryBoxes->setData(&m_data.optionPrimaryFlags);
                }
                clearEvent(event);
                return;
            case cmPermissionOwnership:
                if (editPermissionOwnership(m_spec.permissionOptions))
                {
                    m_data.optionSecondaryFlags |= kOptionPermissionBit;
                    if (m_secondaryBoxes)
                        m_secondaryBoxes->setData(&m_data.optionSecondaryFlags);
                }
                clearEvent(event);
                return;
            case cmTraversalFilters:
                if (editTraversalFilters(m_spec.traversalOptions))
                {
                    m_data.optionSecondaryFlags |= kOptionTraversalBit;
                    if (m_secondaryBoxes)
                        m_secondaryBoxes->setData(&m_data.optionSecondaryFlags);
                }
                clearEvent(event);
                return;
            case cmActionOptions:
                if (editActionOptions(m_spec.actionOptions))
                {
                    m_data.optionSecondaryFlags |= kOptionActionBit;
                    if (m_secondaryBoxes)
                        m_secondaryBoxes->setData(&m_data.optionSecondaryFlags);
                }
                clearEvent(event);
                return;
            case cmDialogSaveSpec:
                messageBox("Saving search specifications will be available in a future update.", mfInformation | mfOKButton);
                clearEvent(event);
                return;
            case cmDialogLoadSpec:
                messageBox("Loading search specifications will be available in a future update.", mfInformation | mfOKButton);
                clearEvent(event);
                return;
            case cmBrowseStart:
                messageBox("Directory browsing is not available yet. Enter a path manually for now.", mfInformation | mfOKButton);
                clearEvent(event);
                return;
            default:
                break;
            }
        }
        TDialog::handleEvent(event);
    }

private:
    SearchSpecification &m_spec;
    SearchDialogData &m_data;
    TCheckBoxes *m_primaryBoxes = nullptr;
    TCheckBoxes *m_secondaryBoxes = nullptr;
};

bool configureSearchSpecification(SearchSpecification &spec)
{
    SearchDialogData data{};
    std::snprintf(data.specName, sizeof(data.specName), "%s", bufferToString(spec.specName).c_str());
    std::snprintf(data.startLocation, sizeof(data.startLocation), "%s", bufferToString(spec.startLocation).c_str());
    std::snprintf(data.searchText, sizeof(data.searchText), "%s", bufferToString(spec.searchText).c_str());
    std::snprintf(data.includePatterns, sizeof(data.includePatterns), "%s", bufferToString(spec.includePatterns).c_str());
    std::snprintf(data.excludePatterns, sizeof(data.excludePatterns), "%s", bufferToString(spec.excludePatterns).c_str());

    if (spec.includeSubdirectories)
        data.generalFlags |= kGeneralRecursiveBit;
    if (spec.includeHidden)
        data.generalFlags |= kGeneralHiddenBit;
    if (spec.followSymlinks)
        data.generalFlags |= kGeneralSymlinkBit;
    if (spec.stayOnSameFilesystem)
        data.generalFlags |= kGeneralStayOnFsBit;

    if (spec.enableTextSearch)
        data.optionPrimaryFlags |= kOptionTextBit;
    if (spec.enableNamePathTests)
        data.optionPrimaryFlags |= kOptionNamePathBit;
    if (spec.enableTimeFilters)
        data.optionPrimaryFlags |= kOptionTimeBit;
    if (spec.enableSizeFilters)
        data.optionPrimaryFlags |= kOptionSizeBit;
    if (spec.enableTypeFilters)
        data.optionPrimaryFlags |= kOptionTypeBit;

    if (spec.enablePermissionOwnership)
        data.optionSecondaryFlags |= kOptionPermissionBit;
    if (spec.enableTraversalFilters)
        data.optionSecondaryFlags |= kOptionTraversalBit;
    if (spec.enableActionOptions)
        data.optionSecondaryFlags |= kOptionActionBit;

    auto *dialog = new SearchDialog(spec, data);

    auto *specNameInput = new TInputLine(TRect(3, 2, 60, 3), sizeof(data.specName) - 1);
    dialog->insert(new TLabel(TRect(2, 1, 18, 2), "~N~ame:", specNameInput));
    dialog->insert(specNameInput);
    specNameInput->setData(data.specName);

    dialog->insert(new TStaticText(TRect(3, 3, 74, 5),
                                   "Start with directories and optional text. Advanced buttons\n"
                                   "map directly to sensible find(1) switches."));

    auto *startInput = new TInputLine(TRect(3, 6, 60, 7), sizeof(data.startLocation) - 1);
    dialog->insert(new TLabel(TRect(2, 5, 26, 6), "Start ~l~ocation:", startInput));
    dialog->insert(startInput);
    startInput->setData(data.startLocation);
    dialog->insert(new TButton(TRect(62, 5, 74, 7), "~B~rowse...", cmBrowseStart, bfNormal));

    auto *textInput = new TInputLine(TRect(3, 8, 60, 9), sizeof(data.searchText) - 1);
    dialog->insert(new TLabel(TRect(2, 7, 22, 8), "Te~x~t to find:", textInput));
    dialog->insert(textInput);
    textInput->setData(data.searchText);
    dialog->insert(new TStaticText(TRect(62, 7, 74, 10),
                                   "Leave empty\nfor metadata-only\nqueries."));

    auto *includeInput = new TInputLine(TRect(3, 10, 36, 11), sizeof(data.includePatterns) - 1);
    dialog->insert(new TLabel(TRect(2, 9, 26, 10), "Include patterns:", includeInput));
    dialog->insert(includeInput);
    includeInput->setData(data.includePatterns);

    auto *excludeInput = new TInputLine(TRect(39, 10, 74, 11), sizeof(data.excludePatterns) - 1);
    dialog->insert(new TLabel(TRect(38, 9, 74, 10), "Exclude patterns:", excludeInput));
    dialog->insert(excludeInput);
    excludeInput->setData(data.excludePatterns);

    auto *generalBoxes = new TCheckBoxes(TRect(3, 11, 38, 17),
                                         makeItemList({"~R~ecursive",
                                                       "Include ~h~idden",
                                                       "Follow s~y~mlinks (-L)",
                                                       "Stay on same file ~s~ystem"}));
    dialog->insert(generalBoxes);
    generalBoxes->setData(&data.generalFlags);

    auto *primaryBoxes = new TCheckBoxes(TRect(39, 11, 59, 17),
                                         makeItemList({"~T~ext search",
                                                       "Name/~P~ath tests",
                                                       "~T~ime tests",
                                                       "Si~z~e filters",
                                                       "File ~t~ype filters"}));
    dialog->insert(primaryBoxes);
    primaryBoxes->setData(&data.optionPrimaryFlags);
    dialog->setPrimaryBoxes(primaryBoxes);

    auto *secondaryBoxes = new TCheckBoxes(TRect(60, 11, 77, 17),
                                           makeItemList({"~P~ermissions & owners",
                                                         "T~r~aversal / FS",
                                                         "~A~ctions & output"}));
    dialog->insert(secondaryBoxes);
    secondaryBoxes->setData(&data.optionSecondaryFlags);
    dialog->setSecondaryBoxes(secondaryBoxes);

    dialog->insert(new TButton(TRect(3, 17, 21, 19), "Text ~O~ptions...", cmTextOptions, bfNormal));
    dialog->insert(new TButton(TRect(23, 17, 41, 19), "Name/~P~ath...", cmNamePathOptions, bfNormal));
    dialog->insert(new TButton(TRect(43, 17, 59, 19), "Time ~T~ests...", cmTimeFilters, bfNormal));
    dialog->insert(new TButton(TRect(61, 17, 77, 19), "Si~z~e Filters...", cmSizeFilters, bfNormal));

    dialog->insert(new TButton(TRect(3, 19, 21, 21), "File ~T~ypes...", cmTypeFilters, bfNormal));
    dialog->insert(new TButton(TRect(23, 19, 45, 21), "~P~ermissions...", cmPermissionOwnership, bfNormal));
    dialog->insert(new TButton(TRect(47, 19, 69, 21), "T~r~aversal / FS...", cmTraversalFilters, bfNormal));

    dialog->insert(new TButton(TRect(3, 21, 21, 23), "~A~ctions...", cmActionOptions, bfNormal));
    dialog->insert(new TButton(TRect(23, 21, 37, 23), "~L~oad Spec...", cmDialogLoadSpec, bfNormal));
    dialog->insert(new TButton(TRect(39, 21, 53, 23), "Sa~v~e Spec...", cmDialogSaveSpec, bfNormal));

    dialog->insert(new TButton(TRect(55, 21, 67, 23), "~S~earch", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(69, 21, 77, 23), "Cancel", cmCancel, bfNormal));

    dialog->selectNext(False);

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        specNameInput->getData(data.specName);
        startInput->getData(data.startLocation);
        textInput->getData(data.searchText);
        includeInput->getData(data.includePatterns);
        excludeInput->getData(data.excludePatterns);
        generalBoxes->getData(&data.generalFlags);
        primaryBoxes->getData(&data.optionPrimaryFlags);
        secondaryBoxes->getData(&data.optionSecondaryFlags);

        copyToArray(spec.specName, data.specName);
        copyToArray(spec.startLocation, data.startLocation);
        copyToArray(spec.searchText, data.searchText);
        copyToArray(spec.includePatterns, data.includePatterns);
        copyToArray(spec.excludePatterns, data.excludePatterns);

        spec.includeSubdirectories = (data.generalFlags & kGeneralRecursiveBit) != 0;
        spec.includeHidden = (data.generalFlags & kGeneralHiddenBit) != 0;
        spec.followSymlinks = (data.generalFlags & kGeneralSymlinkBit) != 0;
        spec.stayOnSameFilesystem = (data.generalFlags & kGeneralStayOnFsBit) != 0;

        if (spec.followSymlinks)
            spec.traversalOptions.symlinkMode = TraversalFilesystemOptions::SymlinkMode::Everywhere;
        else if (spec.traversalOptions.symlinkMode == TraversalFilesystemOptions::SymlinkMode::Everywhere)
            spec.traversalOptions.symlinkMode = TraversalFilesystemOptions::SymlinkMode::Physical;

        spec.traversalOptions.stayOnFilesystem = spec.stayOnSameFilesystem;

        spec.enableTextSearch = (data.optionPrimaryFlags & kOptionTextBit) != 0;
        spec.enableNamePathTests = (data.optionPrimaryFlags & kOptionNamePathBit) != 0;
        spec.enableTimeFilters = (data.optionPrimaryFlags & kOptionTimeBit) != 0;
        spec.enableSizeFilters = (data.optionPrimaryFlags & kOptionSizeBit) != 0;
        spec.enableTypeFilters = (data.optionPrimaryFlags & kOptionTypeBit) != 0;

        spec.enablePermissionOwnership = (data.optionSecondaryFlags & kOptionPermissionBit) != 0;
        spec.enableTraversalFilters = (data.optionSecondaryFlags & kOptionTraversalBit) != 0;
        spec.enableActionOptions = (data.optionSecondaryFlags & kOptionActionBit) != 0;
    }

    TObject::destroy(dialog);
    return accepted;
}

class FindStatusLine : public TStatusLine
{
public:
    FindStatusLine(TRect r)
        : TStatusLine(r, *new TStatusDef(0, 0xFFFF, nullptr))
    {
        rebuild();
    }

private:
    void rebuild()
    {
        disposeItems(items);
        auto *newItem = new TStatusItem("~F2~ New Search", kbF2, cmNewSearch);
        auto *loadItem = new TStatusItem("~Ctrl-O~ Load Spec", kbCtrlO, cmLoadSpec);
        auto *saveItem = new TStatusItem("~Ctrl-S~ Save Spec", kbCtrlS, cmSaveSpec);
        newItem->next = loadItem;
        loadItem->next = saveItem;
        TStatusItem *tail = saveItem;
        if (ck::launcher::launchedFromCkLauncher())
        {
            auto *returnItem = new TStatusItem("~Ctrl-L~ Return", kbCtrlL, cmReturnToLauncher);
            tail->next = returnItem;
            tail = returnItem;
        }
        auto *quitItem = new TStatusItem("~Alt-X~ Quit", kbAltX, cmQuit);
        tail->next = quitItem;
        items = newItem;
        defs->items = items;
        drawView();
    }

    void disposeItems(TStatusItem *item)
    {
        while (item)
        {
            TStatusItem *next = item->next;
            delete item;
            item = next;
        }
    }
};

std::string buildPlaceholderSummary(const SearchSpecification &spec)
{
    std::ostringstream out;
    out << "Search specification captured." << '\n' << '\n';
    std::string name = bufferToString(spec.specName);
    if (!name.empty())
        out << "Name: " << name << '\n';
    std::string start = bufferToString(spec.startLocation);
    if (!start.empty())
        out << "Start: " << start << '\n';

    std::vector<std::string> traversalBits;
    traversalBits.push_back(spec.includeSubdirectories ? "recursive" : "single level");
    if (spec.includeHidden)
        traversalBits.push_back("include hidden");
    if (spec.followSymlinks)
        traversalBits.push_back("follow symlinks");
    else
    {
        switch (spec.traversalOptions.symlinkMode)
        {
        case TraversalFilesystemOptions::SymlinkMode::CommandLine:
            traversalBits.push_back("follow symlinks on command line");
            break;
        case TraversalFilesystemOptions::SymlinkMode::Everywhere:
            traversalBits.push_back("follow symlinks");
            break;
        case TraversalFilesystemOptions::SymlinkMode::Physical:
        default:
            break;
        }
    }
    if (spec.stayOnSameFilesystem || spec.traversalOptions.stayOnFilesystem)
        traversalBits.push_back("stay on filesystem");
    auto joinList = [](const std::vector<std::string> &items) {
        std::string joined;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (i)
                joined += ", ";
            joined += items[i];
        }
        return joined;
    };
    out << "Traversal: " << joinList(traversalBits) << '\n';

    std::string text = bufferToString(spec.searchText);
    if (!text.empty())
    {
        if (text.size() > 48)
            text = text.substr(0, 45) + "...";
        out << "Search text: \"" << text << "\"" << '\n';
    }

    std::vector<std::string> modules;
    if (spec.enableTextSearch)
        modules.emplace_back("text");
    if (spec.enableNamePathTests)
        modules.emplace_back("name/path");
    if (spec.enableTimeFilters)
        modules.emplace_back("time");
    if (spec.enableSizeFilters)
        modules.emplace_back("size");
    if (spec.enableTypeFilters)
        modules.emplace_back("type");
    if (spec.enablePermissionOwnership)
        modules.emplace_back("permissions/ownership");
    if (spec.enableTraversalFilters)
        modules.emplace_back("traversal");
    out << "Find modules enabled: " << (modules.empty() ? std::string("none") : joinList(modules)) << '\n';

    if (spec.enableActionOptions)
    {
        std::vector<std::string> actions;
        if (spec.actionOptions.print)
            actions.emplace_back("-print");
        if (spec.actionOptions.print0)
            actions.emplace_back("-print0");
        if (spec.actionOptions.ls)
            actions.emplace_back("-ls");
        if (spec.actionOptions.deleteMatches)
            actions.emplace_back("-delete");
        if (spec.actionOptions.quitEarly)
            actions.emplace_back("-quit");
        if (spec.actionOptions.execEnabled)
        {
            switch (spec.actionOptions.execVariant)
            {
            case ActionOptions::ExecVariant::Exec:
                actions.emplace_back(spec.actionOptions.execUsePlus ? "-exec ... +" : "-exec ... ;");
                break;
            case ActionOptions::ExecVariant::ExecDir:
                actions.emplace_back(spec.actionOptions.execUsePlus ? "-execdir ... +" : "-execdir ... ;");
                break;
            case ActionOptions::ExecVariant::Ok:
                actions.emplace_back(spec.actionOptions.execUsePlus ? "-ok ... +" : "-ok ... ;");
                break;
            case ActionOptions::ExecVariant::OkDir:
                actions.emplace_back(spec.actionOptions.execUsePlus ? "-okdir ... +" : "-okdir ... ;");
                break;
            }
        }
        if (spec.actionOptions.fprintEnabled)
            actions.emplace_back("-fprint");
        if (spec.actionOptions.fprint0Enabled)
            actions.emplace_back("-fprint0");
        if (spec.actionOptions.flsEnabled)
            actions.emplace_back("-fls");
        if (spec.actionOptions.printfEnabled)
            actions.emplace_back("-printf");
        if (spec.actionOptions.fprintfEnabled)
            actions.emplace_back("-fprintf");
        out << "Actions: " << (actions.empty() ? std::string("none") : joinList(actions)) << '\n';
    }
    else
    {
        out << "Actions: none" << '\n';
    }

    out << '\n';
    out << "Execution and persistence workflows will be implemented in future milestones.";
    return out.str();
}

bool editTextOptions(TextSearchOptions &options)
{
    struct Data
    {
        ushort mode = 0;
        ushort flags = 0;
    } data{};
    data.mode = static_cast<ushort>(options.mode);
    if (options.matchCase)
        data.flags |= 0x0001;
    if (options.searchInContents)
        data.flags |= 0x0002;
    if (options.searchInFileNames)
        data.flags |= 0x0004;
    if (options.allowMultipleTerms)
        data.flags |= 0x0008;
    if (options.treatBinaryAsText)
        data.flags |= 0x0010;

    auto *dialog = new TDialog(TRect(0, 0, 60, 16), "Text Options");
    dialog->options |= ofCentered;

    auto *modeButtons = new TRadioButtons(TRect(3, 3, 30, 8),
                                          makeItemList({"Contains te~x~t",
                                                        "Match ~w~hole word",
                                                        "Regular ~e~xpression"}));
    dialog->insert(modeButtons);
    modeButtons->setData(&data.mode);

    auto *optionBoxes = new TCheckBoxes(TRect(32, 3, 58, 9),
                                        makeItemList({"~M~atch case",
                                                      "Search file ~c~ontents",
                                                      "Search file ~n~ames",
                                                      "Allow ~m~ultiple terms",
                                                      "Treat ~b~inary as text"}));
    dialog->insert(optionBoxes);
    optionBoxes->setData(&data.flags);

    dialog->insert(new TStaticText(TRect(3, 9, 58, 12),
                                   "Use regular expressions when you need complex\n"
                                   "pattern matching. Whole-word mode respects\n"
                                   "word boundaries."));

    dialog->insert(new TButton(TRect(16, 12, 26, 14), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(28, 12, 38, 14), "Cancel", cmCancel, bfNormal));

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        modeButtons->getData(&data.mode);
        optionBoxes->getData(&data.flags);
        options.mode = static_cast<TextSearchOptions::Mode>(data.mode);
        options.matchCase = (data.flags & 0x0001) != 0;
        options.searchInContents = (data.flags & 0x0002) != 0;
        options.searchInFileNames = (data.flags & 0x0004) != 0;
        options.allowMultipleTerms = (data.flags & 0x0008) != 0;
        options.treatBinaryAsText = (data.flags & 0x0010) != 0;
    }
    TObject::destroy(dialog);
    return accepted;
}

bool editNamePathOptions(NamePathOptions &options)
{
    struct Data
    {
        ushort flags = 0;
        ushort pruneFlags = 0;
        ushort pruneMode = 0;
        char name[256]{};
        char iname[256]{};
        char path[256]{};
        char ipath[256]{};
        char regex[256]{};
        char iregex[256]{};
        char lname[256]{};
        char ilname[256]{};
        char prune[256]{};
    } data{};

    if (options.nameEnabled)
        data.flags |= 0x0001;
    if (options.inameEnabled)
        data.flags |= 0x0002;
    if (options.pathEnabled)
        data.flags |= 0x0004;
    if (options.ipathEnabled)
        data.flags |= 0x0008;
    if (options.regexEnabled)
        data.flags |= 0x0010;
    if (options.iregexEnabled)
        data.flags |= 0x0020;
    if (options.lnameEnabled)
        data.flags |= 0x0040;
    if (options.ilnameEnabled)
        data.flags |= 0x0080;
    if (options.pruneEnabled)
        data.pruneFlags |= 0x0001;
    if (options.pruneDirectoriesOnly)
        data.pruneFlags |= 0x0002;
    data.pruneMode = static_cast<ushort>(options.pruneTest);

    std::snprintf(data.name, sizeof(data.name), "%s", bufferToString(options.namePattern).c_str());
    std::snprintf(data.iname, sizeof(data.iname), "%s", bufferToString(options.inamePattern).c_str());
    std::snprintf(data.path, sizeof(data.path), "%s", bufferToString(options.pathPattern).c_str());
    std::snprintf(data.ipath, sizeof(data.ipath), "%s", bufferToString(options.ipathPattern).c_str());
    std::snprintf(data.regex, sizeof(data.regex), "%s", bufferToString(options.regexPattern).c_str());
    std::snprintf(data.iregex, sizeof(data.iregex), "%s", bufferToString(options.iregexPattern).c_str());
    std::snprintf(data.lname, sizeof(data.lname), "%s", bufferToString(options.lnamePattern).c_str());
    std::snprintf(data.ilname, sizeof(data.ilname), "%s", bufferToString(options.ilnamePattern).c_str());
    std::snprintf(data.prune, sizeof(data.prune), "%s", bufferToString(options.prunePattern).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 80, 24), "Name and Path Tests");
    dialog->options |= ofCentered;

    dialog->insert(new TStaticText(TRect(3, 2, 76, 4),
                                   "Enable matchers and provide glob or regex values.\n"
                                   "Leave a field blank to skip that test."));

    auto *patternBoxes = new TCheckBoxes(TRect(3, 4, 28, 16),
                                         makeItemList({"~N~ame (-name)",
                                                       "Case-insensitive ~n~ame (-iname)",
                                                       "~P~ath (-path)",
                                                       "Case-insensitive pa~t~h (-ipath)",
                                                       "Regular e~x~pression (-regex)",
                                                       "Case-insensitive re~g~ex (-iregex)",
                                                       "Symlink ~l~name (-lname)",
                                                       "Case-insensitive l~n~ame (-ilname)"}));
    dialog->insert(patternBoxes);
    patternBoxes->setData(&data.flags);

    auto *nameInput = new TInputLine(TRect(30, 4, 56, 5), sizeof(data.name) - 1);
    dialog->insert(new TLabel(TRect(30, 3, 56, 4), "~N~ame pattern:", nameInput));
    dialog->insert(nameInput);
    nameInput->setData(data.name);

    auto *inameInput = new TInputLine(TRect(30, 6, 56, 7), sizeof(data.iname) - 1);
    dialog->insert(new TLabel(TRect(30, 5, 56, 6), "Case-insensitive ~n~ame:", inameInput));
    dialog->insert(inameInput);
    inameInput->setData(data.iname);

    auto *pathInput = new TInputLine(TRect(30, 8, 56, 9), sizeof(data.path) - 1);
    dialog->insert(new TLabel(TRect(30, 7, 56, 8), "~P~ath glob:", pathInput));
    dialog->insert(pathInput);
    pathInput->setData(data.path);

    auto *ipathInput = new TInputLine(TRect(30, 10, 56, 11), sizeof(data.ipath) - 1);
    dialog->insert(new TLabel(TRect(30, 9, 56, 10), "Case-insensitive pa~t~h:", ipathInput));
    dialog->insert(ipathInput);
    ipathInput->setData(data.ipath);

    auto *regexInput = new TInputLine(TRect(58, 4, 78, 5), sizeof(data.regex) - 1);
    dialog->insert(new TLabel(TRect(58, 3, 78, 4), "Re~g~ex (-regex):", regexInput));
    dialog->insert(regexInput);
    regexInput->setData(data.regex);

    auto *iregexInput = new TInputLine(TRect(58, 6, 78, 7), sizeof(data.iregex) - 1);
    dialog->insert(new TLabel(TRect(58, 5, 78, 6), "Case-insensitive re~g~ex:", iregexInput));
    dialog->insert(iregexInput);
    iregexInput->setData(data.iregex);

    auto *lnameInput = new TInputLine(TRect(58, 8, 78, 9), sizeof(data.lname) - 1);
    dialog->insert(new TLabel(TRect(58, 7, 78, 8), "Symlink ~l~name:", lnameInput));
    dialog->insert(lnameInput);
    lnameInput->setData(data.lname);

    auto *ilnameInput = new TInputLine(TRect(58, 10, 78, 11), sizeof(data.ilname) - 1);
    dialog->insert(new TLabel(TRect(58, 9, 78, 10), "Case-insensitive l~n~ame:", ilnameInput));
    dialog->insert(ilnameInput);
    ilnameInput->setData(data.ilname);

    auto *pruneBoxes = new TCheckBoxes(TRect(3, 16, 28, 20),
                                       makeItemList({"Enable -p~r~une",
                                                     "Directories ~o~nly"}));
    dialog->insert(pruneBoxes);
    pruneBoxes->setData(&data.pruneFlags);

    auto *pruneModeButtons = new TRadioButtons(TRect(30, 16, 78, 21),
                                               makeItemList({"Use -name",
                                                             "Use -iname",
                                                             "Use -path",
                                                             "Use -ipath",
                                                             "Use -regex",
                                                             "Use -iregex"}));
    dialog->insert(pruneModeButtons);
    pruneModeButtons->setData(&data.pruneMode);

    auto *pruneInput = new TInputLine(TRect(30, 21, 78, 22), sizeof(data.prune) - 1);
    dialog->insert(new TLabel(TRect(30, 20, 74, 21), "-prune pattern:", pruneInput));
    dialog->insert(pruneInput);
    pruneInput->setData(data.prune);

    dialog->insert(new TButton(TRect(30, 22, 40, 24), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(42, 22, 52, 24), "Cancel", cmCancel, bfNormal));

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        patternBoxes->getData(&data.flags);
        pruneBoxes->getData(&data.pruneFlags);
        pruneModeButtons->getData(&data.pruneMode);
        nameInput->getData(data.name);
        inameInput->getData(data.iname);
        pathInput->getData(data.path);
        ipathInput->getData(data.ipath);
        regexInput->getData(data.regex);
        iregexInput->getData(data.iregex);
        lnameInput->getData(data.lname);
        ilnameInput->getData(data.ilname);
        pruneInput->getData(data.prune);

        options.nameEnabled = (data.flags & 0x0001) != 0;
        options.inameEnabled = (data.flags & 0x0002) != 0;
        options.pathEnabled = (data.flags & 0x0004) != 0;
        options.ipathEnabled = (data.flags & 0x0008) != 0;
        options.regexEnabled = (data.flags & 0x0010) != 0;
        options.iregexEnabled = (data.flags & 0x0020) != 0;
        options.lnameEnabled = (data.flags & 0x0040) != 0;
        options.ilnameEnabled = (data.flags & 0x0080) != 0;
        options.pruneEnabled = (data.pruneFlags & 0x0001) != 0;
        options.pruneDirectoriesOnly = (data.pruneFlags & 0x0002) != 0;
        options.pruneTest = static_cast<NamePathOptions::PruneTest>(data.pruneMode);

        copyToArray(options.namePattern, data.name);
        copyToArray(options.inamePattern, data.iname);
        copyToArray(options.pathPattern, data.path);
        copyToArray(options.ipathPattern, data.ipath);
        copyToArray(options.regexPattern, data.regex);
        copyToArray(options.iregexPattern, data.iregex);
        copyToArray(options.lnamePattern, data.lname);
        copyToArray(options.ilnamePattern, data.ilname);
        copyToArray(options.prunePattern, data.prune);
    }

    TObject::destroy(dialog);
    return accepted;
}

bool editTimeFilters(TimeFilterOptions &options)
{
    struct Data
    {
        ushort preset = 0;
        ushort fields = 0;
        char from[32]{};
        char to[32]{};
        char mtime[16]{};
        char mmin[16]{};
        char atime[16]{};
        char amin[16]{};
        char ctime[16]{};
        char cmin[16]{};
        char used[16]{};
        char newer[PATH_MAX]{};
        char anewer[PATH_MAX]{};
        char cnewer[PATH_MAX]{};
        char newermt[64]{};
        char newerat[64]{};
        char newerct[64]{};
    } data{};

    data.preset = static_cast<ushort>(options.preset);
    if (options.includeModified)
        data.fields |= 0x0001;
    if (options.includeCreated)
        data.fields |= 0x0002;
    if (options.includeAccessed)
        data.fields |= 0x0004;

    std::snprintf(data.from, sizeof(data.from), "%s", bufferToString(options.customFrom).c_str());
    std::snprintf(data.to, sizeof(data.to), "%s", bufferToString(options.customTo).c_str());
    std::snprintf(data.mtime, sizeof(data.mtime), "%s", bufferToString(options.mtime).c_str());
    std::snprintf(data.mmin, sizeof(data.mmin), "%s", bufferToString(options.mmin).c_str());
    std::snprintf(data.atime, sizeof(data.atime), "%s", bufferToString(options.atime).c_str());
    std::snprintf(data.amin, sizeof(data.amin), "%s", bufferToString(options.amin).c_str());
    std::snprintf(data.ctime, sizeof(data.ctime), "%s", bufferToString(options.ctime).c_str());
    std::snprintf(data.cmin, sizeof(data.cmin), "%s", bufferToString(options.cmin).c_str());
    std::snprintf(data.used, sizeof(data.used), "%s", bufferToString(options.used).c_str());
    std::snprintf(data.newer, sizeof(data.newer), "%s", bufferToString(options.newer).c_str());
    std::snprintf(data.anewer, sizeof(data.anewer), "%s", bufferToString(options.anewer).c_str());
    std::snprintf(data.cnewer, sizeof(data.cnewer), "%s", bufferToString(options.cnewer).c_str());
    std::snprintf(data.newermt, sizeof(data.newermt), "%s", bufferToString(options.newermt).c_str());
    std::snprintf(data.newerat, sizeof(data.newerat), "%s", bufferToString(options.newerat).c_str());
    std::snprintf(data.newerct, sizeof(data.newerct), "%s", bufferToString(options.newerct).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 78, 24), "Time Tests");
    dialog->options |= ofCentered;

    auto *presetButtons = new TRadioButtons(TRect(3, 3, 30, 12),
                                            makeItemList({"Any ~t~ime",
                                                          "Past ~2~4 hours",
                                                          "Past ~7~ days",
                                                          "Past ~1~ month",
                                                          "Past ~6~ months",
                                                          "Past ~1~ year",
                                                          "Past ~6~ years",
                                                          "~C~ustom range"}));
    dialog->insert(presetButtons);
    presetButtons->setData(&data.preset);

    auto *fieldBoxes = new TCheckBoxes(TRect(32, 3, 58, 7),
                                       makeItemList({"Last ~m~odified",
                                                     "~C~reation time",
                                                     "Last ~a~ccess"}));
    dialog->insert(fieldBoxes);
    fieldBoxes->setData(&data.fields);

    auto *fromInput = new TInputLine(TRect(32, 7, 58, 8), sizeof(data.from) - 1);
    dialog->insert(new TLabel(TRect(32, 6, 56, 7), "~F~rom (YYYY-MM-DD):", fromInput));
    dialog->insert(fromInput);
    fromInput->setData(data.from);

    auto *toInput = new TInputLine(TRect(32, 9, 58, 10), sizeof(data.to) - 1);
    dialog->insert(new TLabel(TRect(32, 8, 56, 9), "~T~o (YYYY-MM-DD):", toInput));
    dialog->insert(toInput);
    toInput->setData(data.to);

    dialog->insert(new TStaticText(TRect(3, 12, 74, 14),
                                   "Manual fields mirror find(1) tests. Use prefixes like +7 or -5"
                                   " and timestamp strings supported by find."));

    auto *mtimeInput = new TInputLine(TRect(18, 14, 34, 15), sizeof(data.mtime) - 1);
    dialog->insert(new TLabel(TRect(3, 14, 18, 15), "-mti~m~e:", mtimeInput));
    dialog->insert(mtimeInput);
    mtimeInput->setData(data.mtime);

    auto *mminInput = new TInputLine(TRect(18, 15, 34, 16), sizeof(data.mmin) - 1);
    dialog->insert(new TLabel(TRect(3, 15, 18, 16), "-~m~min:", mminInput));
    dialog->insert(mminInput);
    mminInput->setData(data.mmin);

    auto *atimeInput = new TInputLine(TRect(18, 16, 34, 17), sizeof(data.atime) - 1);
    dialog->insert(new TLabel(TRect(3, 16, 18, 17), "-~a~time:", atimeInput));
    dialog->insert(atimeInput);
    atimeInput->setData(data.atime);

    auto *aminInput = new TInputLine(TRect(18, 17, 34, 18), sizeof(data.amin) - 1);
    dialog->insert(new TLabel(TRect(3, 17, 18, 18), "-a~m~in:", aminInput));
    dialog->insert(aminInput);
    aminInput->setData(data.amin);

    auto *ctimeInput = new TInputLine(TRect(18, 18, 34, 19), sizeof(data.ctime) - 1);
    dialog->insert(new TLabel(TRect(3, 18, 18, 19), "-~c~time:", ctimeInput));
    dialog->insert(ctimeInput);
    ctimeInput->setData(data.ctime);

    auto *cminInput = new TInputLine(TRect(18, 19, 34, 20), sizeof(data.cmin) - 1);
    dialog->insert(new TLabel(TRect(3, 19, 18, 20), "-c~m~in:", cminInput));
    dialog->insert(cminInput);
    cminInput->setData(data.cmin);

    auto *usedInput = new TInputLine(TRect(18, 20, 34, 21), sizeof(data.used) - 1);
    dialog->insert(new TLabel(TRect(3, 20, 18, 21), "-~u~sed:", usedInput));
    dialog->insert(usedInput);
    usedInput->setData(data.used);

    auto pathLen = std::min<int>(static_cast<int>(sizeof(data.newer)) - 1, 255);
    auto *newerInput = new TInputLine(TRect(51, 14, 74, 15), pathLen);
    dialog->insert(new TLabel(TRect(36, 14, 51, 15), "-~n~ewer:", newerInput));
    dialog->insert(newerInput);
    newerInput->setData(data.newer);

    auto *anewerInput = new TInputLine(TRect(51, 15, 74, 16), pathLen);
    dialog->insert(new TLabel(TRect(36, 15, 51, 16), "-~a~newer:", anewerInput));
    dialog->insert(anewerInput);
    anewerInput->setData(data.anewer);

    auto *cnewerInput = new TInputLine(TRect(51, 16, 74, 17), pathLen);
    dialog->insert(new TLabel(TRect(36, 16, 51, 17), "-~c~newer:", cnewerInput));
    dialog->insert(cnewerInput);
    cnewerInput->setData(data.cnewer);

    auto *newermtInput = new TInputLine(TRect(51, 17, 74, 18), sizeof(data.newermt) - 1);
    dialog->insert(new TLabel(TRect(36, 17, 51, 18), "-newer~m~t:", newermtInput));
    dialog->insert(newermtInput);
    newermtInput->setData(data.newermt);

    auto *neweratInput = new TInputLine(TRect(51, 18, 74, 19), sizeof(data.newerat) - 1);
    dialog->insert(new TLabel(TRect(36, 18, 51, 19), "-newer~a~t:", neweratInput));
    dialog->insert(neweratInput);
    neweratInput->setData(data.newerat);

    auto *newerctInput = new TInputLine(TRect(51, 19, 74, 20), sizeof(data.newerct) - 1);
    dialog->insert(new TLabel(TRect(36, 19, 51, 20), "-newer~c~t:", newerctInput));
    dialog->insert(newerctInput);
    newerctInput->setData(data.newerct);

    dialog->insert(new TButton(TRect(30, 22, 40, 24), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(42, 22, 52, 24), "Cancel", cmCancel, bfNormal));

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        presetButtons->getData(&data.preset);
        fieldBoxes->getData(&data.fields);
        fromInput->getData(data.from);
        toInput->getData(data.to);
        mtimeInput->getData(data.mtime);
        mminInput->getData(data.mmin);
        atimeInput->getData(data.atime);
        aminInput->getData(data.amin);
        ctimeInput->getData(data.ctime);
        cminInput->getData(data.cmin);
        usedInput->getData(data.used);
        newerInput->getData(data.newer);
        anewerInput->getData(data.anewer);
        cnewerInput->getData(data.cnewer);
        newermtInput->getData(data.newermt);
        neweratInput->getData(data.newerat);
        newerctInput->getData(data.newerct);

        options.preset = static_cast<TimeFilterOptions::Preset>(data.preset);
        options.includeModified = (data.fields & 0x0001) != 0;
        options.includeCreated = (data.fields & 0x0002) != 0;
        options.includeAccessed = (data.fields & 0x0004) != 0;

        copyToArray(options.customFrom, data.from);
        copyToArray(options.customTo, data.to);

        copyToArray(options.mtime, data.mtime);
        options.useMTime = data.mtime[0] != '\0';
        copyToArray(options.mmin, data.mmin);
        options.useMMin = data.mmin[0] != '\0';
        copyToArray(options.atime, data.atime);
        options.useATime = data.atime[0] != '\0';
        copyToArray(options.amin, data.amin);
        options.useAMin = data.amin[0] != '\0';
        copyToArray(options.ctime, data.ctime);
        options.useCTime = data.ctime[0] != '\0';
        copyToArray(options.cmin, data.cmin);
        options.useCMin = data.cmin[0] != '\0';
        copyToArray(options.used, data.used);
        options.useUsed = data.used[0] != '\0';

        copyToArray(options.newer, data.newer);
        options.useNewer = data.newer[0] != '\0';
        copyToArray(options.anewer, data.anewer);
        options.useANewer = data.anewer[0] != '\0';
        copyToArray(options.cnewer, data.cnewer);
        options.useCNewer = data.cnewer[0] != '\0';
        copyToArray(options.newermt, data.newermt);
        options.useNewermt = data.newermt[0] != '\0';
        copyToArray(options.newerat, data.newerat);
        options.useNewerat = data.newerat[0] != '\0';
        copyToArray(options.newerct, data.newerct);
        options.useNewerct = data.newerct[0] != '\0';
    }

    TObject::destroy(dialog);
    return accepted;
}

bool editSizeFilters(SizeFilterOptions &options)
{
    struct Data
    {
        ushort flags = 0;
        char minSpec[32]{};
        char maxSpec[32]{};
        char exactSpec[32]{};
    } data{};

    if (options.minEnabled)
        data.flags |= 0x0001;
    if (options.maxEnabled)
        data.flags |= 0x0002;
    if (options.exactEnabled)
        data.flags |= 0x0004;
    if (options.rangeInclusive)
        data.flags |= 0x0008;
    if (options.includeZeroByte)
        data.flags |= 0x0010;
    if (options.treatDirectoriesAsFiles)
        data.flags |= 0x0020;
    if (options.useDecimalUnits)
        data.flags |= 0x0040;
    if (options.emptyEnabled)
        data.flags |= 0x0080;

    std::snprintf(data.minSpec, sizeof(data.minSpec), "%s", bufferToString(options.minSpec).c_str());
    std::snprintf(data.maxSpec, sizeof(data.maxSpec), "%s", bufferToString(options.maxSpec).c_str());
    std::snprintf(data.exactSpec, sizeof(data.exactSpec), "%s", bufferToString(options.exactSpec).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 72, 20), "Size Filters");
    dialog->options |= ofCentered;

    auto *flagBoxes = new TCheckBoxes(TRect(3, 3, 34, 12),
                                      makeItemList({"~M~inimum size",
                                                    "Ma~x~imum size",
                                                    "Exact -~s~ize expression",
                                                    "R~a~nge inclusive",
                                                    "Include ~0~-byte entries",
                                                    "Treat directorie~s~ as files",
                                                    "Use ~d~ecimal units",
                                                    "Match ~e~mpty entries"}));
    dialog->insert(flagBoxes);
    flagBoxes->setData(&data.flags);

    auto *minInput = new TInputLine(TRect(36, 4, 68, 5), sizeof(data.minSpec) - 1);
    dialog->insert(new TLabel(TRect(36, 3, 68, 4), "-size lower bound:", minInput));
    dialog->insert(minInput);
    minInput->setData(data.minSpec);

    auto *maxInput = new TInputLine(TRect(36, 7, 68, 8), sizeof(data.maxSpec) - 1);
    dialog->insert(new TLabel(TRect(36, 6, 68, 7), "-size upper bound:", maxInput));
    dialog->insert(maxInput);
    maxInput->setData(data.maxSpec);

    auto *exactInput = new TInputLine(TRect(36, 10, 68, 11), sizeof(data.exactSpec) - 1);
    dialog->insert(new TLabel(TRect(36, 9, 68, 10), "Exact -size expression:", exactInput));
    dialog->insert(exactInput);
    exactInput->setData(data.exactSpec);

    dialog->insert(new TStaticText(TRect(3, 12, 68, 14),
                                   "Use find syntax such as +10M, -512k, or 100c. Leave values blank\n"
                                   "to disable those tests."));

    dialog->insert(new TButton(TRect(24, 15, 34, 17), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(36, 15, 46, 17), "Cancel", cmCancel, bfNormal));

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        flagBoxes->getData(&data.flags);
        minInput->getData(data.minSpec);
        maxInput->getData(data.maxSpec);
        exactInput->getData(data.exactSpec);

        options.minEnabled = (data.flags & 0x0001) != 0;
        options.maxEnabled = (data.flags & 0x0002) != 0;
        options.exactEnabled = (data.flags & 0x0004) != 0;
        options.rangeInclusive = (data.flags & 0x0008) != 0;
        options.includeZeroByte = (data.flags & 0x0010) != 0;
        options.treatDirectoriesAsFiles = (data.flags & 0x0020) != 0;
        options.useDecimalUnits = (data.flags & 0x0040) != 0;
        options.emptyEnabled = (data.flags & 0x0080) != 0;

        copyToArray(options.minSpec, data.minSpec);
        copyToArray(options.maxSpec, data.maxSpec);
        copyToArray(options.exactSpec, data.exactSpec);
    }

    TObject::destroy(dialog);
    return accepted;
}

bool editTypeFilters(TypeFilterOptions &options)
{
    struct Data
    {
        ushort flags = 0;
        ushort typeFlags = 0;
        ushort xtypeFlags = 0;
        char extensions[256]{};
        char detectors[256]{};
    } data{};

    if (options.typeEnabled)
        data.flags |= 0x0001;
    if (options.xtypeEnabled)
        data.flags |= 0x0002;
    if (options.useExtensions)
        data.flags |= 0x0004;
    if (options.extensionCaseInsensitive)
        data.flags |= 0x0008;
    if (options.useDetectors)
        data.flags |= 0x0010;

    auto captureLetters = [](const std::string &letters, ushort &bits) {
        for (char ch : letters)
        {
            switch (ch)
            {
            case 'b':
                bits |= 0x0001;
                break;
            case 'c':
                bits |= 0x0002;
                break;
            case 'd':
                bits |= 0x0004;
                break;
            case 'p':
                bits |= 0x0008;
                break;
            case 'f':
                bits |= 0x0010;
                break;
            case 'l':
                bits |= 0x0020;
                break;
            case 's':
                bits |= 0x0040;
                break;
            case 'D':
                bits |= 0x0080;
                break;
            default:
                break;
            }
        }
    };

    captureLetters(bufferToString(options.typeLetters), data.typeFlags);
    captureLetters(bufferToString(options.xtypeLetters), data.xtypeFlags);

    std::snprintf(data.extensions, sizeof(data.extensions), "%s", bufferToString(options.extensions).c_str());
    std::snprintf(data.detectors, sizeof(data.detectors), "%s", bufferToString(options.detectorTags).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 74, 22), "Type Filters");
    dialog->options |= ofCentered;

    auto *flagBoxes = new TCheckBoxes(TRect(3, 3, 32, 12),
                                      makeItemList({"Enable -~t~ype",
                                                    "Enable -~x~type",
                                                    "Filter by ~e~xtension",
                                                    "Case-insensitive e~x~t",
                                                    "Use detector ~t~ags"}));
    dialog->insert(flagBoxes);
    flagBoxes->setData(&data.flags);

    auto *typeBoxes = new TCheckBoxes(TRect(34, 3, 50, 13),
                                      makeItemList({"Block (b)",
                                                    "Char (c)",
                                                    "Directory (d)",
                                                    "FIFO (p)",
                                                    "Regular (f)",
                                                    "Symlink (l)",
                                                    "Socket (s)",
                                                    "Door (D)"}));
    dialog->insert(typeBoxes);
    typeBoxes->setData(&data.typeFlags);

    auto *xtypeBoxes = new TCheckBoxes(TRect(52, 3, 68, 13),
                                       makeItemList({"b",
                                                     "c",
                                                     "d",
                                                     "p",
                                                     "f",
                                                     "l",
                                                     "s",
                                                     "D"}));
    dialog->insert(xtypeBoxes);
    xtypeBoxes->setData(&data.xtypeFlags);

    auto *extensionInput = new TInputLine(TRect(3, 13, 70, 14), sizeof(data.extensions) - 1);
    dialog->insert(new TLabel(TRect(3, 12, 70, 13), "Extensions (comma-separated):", extensionInput));
    dialog->insert(extensionInput);
    extensionInput->setData(data.extensions);

    auto *detectorInput = new TInputLine(TRect(3, 16, 70, 17), sizeof(data.detectors) - 1);
    dialog->insert(new TLabel(TRect(3, 15, 70, 16), "Detector tags (space/comma):", detectorInput));
    dialog->insert(detectorInput);
    detectorInput->setData(data.detectors);

    dialog->insert(new TStaticText(TRect(3, 18, 70, 20),
                                   "Select letters to OR together. -xtype evaluates after symlinks are"
                                   " resolved."));

    dialog->insert(new TButton(TRect(28, 20, 38, 22), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(40, 20, 50, 22), "Cancel", cmCancel, bfNormal));

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        flagBoxes->getData(&data.flags);
        typeBoxes->getData(&data.typeFlags);
        xtypeBoxes->getData(&data.xtypeFlags);
        extensionInput->getData(data.extensions);
        detectorInput->getData(data.detectors);

        options.typeEnabled = (data.flags & 0x0001) != 0;
        options.xtypeEnabled = (data.flags & 0x0002) != 0;
        options.useExtensions = (data.flags & 0x0004) != 0;
        options.extensionCaseInsensitive = (data.flags & 0x0008) != 0;
        options.useDetectors = (data.flags & 0x0010) != 0;

        auto buildLetters = [](ushort bits) {
            std::string out;
            if (bits & 0x0001)
                out.push_back('b');
            if (bits & 0x0002)
                out.push_back('c');
            if (bits & 0x0004)
                out.push_back('d');
            if (bits & 0x0008)
                out.push_back('p');
            if (bits & 0x0010)
                out.push_back('f');
            if (bits & 0x0020)
                out.push_back('l');
            if (bits & 0x0040)
                out.push_back('s');
            if (bits & 0x0080)
                out.push_back('D');
            return out;
        };

        auto typeString = buildLetters(data.typeFlags);
        auto xtypeString = buildLetters(data.xtypeFlags);
        copyToArray(options.typeLetters, typeString.c_str());
        copyToArray(options.xtypeLetters, xtypeString.c_str());
        copyToArray(options.extensions, data.extensions);
        copyToArray(options.detectorTags, data.detectors);
    }

    TObject::destroy(dialog);
    return accepted;
}

bool editPermissionOwnership(PermissionOwnershipOptions &options)
{
    struct Data
    {
        ushort permFlags = 0;
        ushort ownerFlags = 0;
        ushort mode = 0;
        char permSpec[16]{};
        char user[64]{};
        char uid[32]{};
        char group[64]{};
        char gid[32]{};
    } data{};

    if (options.permEnabled)
        data.permFlags |= 0x0001;
    if (options.readable)
        data.permFlags |= 0x0002;
    if (options.writable)
        data.permFlags |= 0x0004;
    if (options.executable)
        data.permFlags |= 0x0008;
    data.mode = static_cast<ushort>(options.permMode);

    if (options.userEnabled)
        data.ownerFlags |= 0x0001;
    if (options.uidEnabled)
        data.ownerFlags |= 0x0002;
    if (options.groupEnabled)
        data.ownerFlags |= 0x0004;
    if (options.gidEnabled)
        data.ownerFlags |= 0x0008;
    if (options.noUser)
        data.ownerFlags |= 0x0010;
    if (options.noGroup)
        data.ownerFlags |= 0x0020;

    std::snprintf(data.permSpec, sizeof(data.permSpec), "%s", bufferToString(options.permSpec).c_str());
    std::snprintf(data.user, sizeof(data.user), "%s", bufferToString(options.user).c_str());
    std::snprintf(data.uid, sizeof(data.uid), "%s", bufferToString(options.uid).c_str());
    std::snprintf(data.group, sizeof(data.group), "%s", bufferToString(options.group).c_str());
    std::snprintf(data.gid, sizeof(data.gid), "%s", bufferToString(options.gid).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 70, 22), "Permissions & Ownership");
    dialog->options |= ofCentered;

    auto *permBoxes = new TCheckBoxes(TRect(3, 3, 26, 9),
                                      makeItemList({"Use -~p~erm",
                                                    "-~r~eadable",
                                                    "-~w~ritable",
                                                    "-~e~xecutable"}));
    dialog->insert(permBoxes);
    permBoxes->setData(&data.permFlags);

    auto *permMode = new TRadioButtons(TRect(28, 3, 52, 9),
                                       makeItemList({"Exact match",
                                                     "All bits (-perm -mode)",
                                                     "Any bit (-perm /mode)"}));
    dialog->insert(permMode);
    permMode->setData(&data.mode);

    auto *permInput = new TInputLine(TRect(3, 9, 52, 10), sizeof(data.permSpec) - 1);
    dialog->insert(new TLabel(TRect(3, 8, 28, 9), "-perm value:", permInput));
    dialog->insert(permInput);
    permInput->setData(data.permSpec);

    auto *ownerBoxes = new TCheckBoxes(TRect(3, 11, 26, 19),
                                       makeItemList({"Filter ~u~ser (-user)",
                                                     "Match ~U~ID (-uid)",
                                                     "Filter ~g~roup (-group)",
                                                     "Match ~G~ID (-gid)",
                                                     "-~n~ouser",
                                                     "-~n~ogroup"}));
    dialog->insert(ownerBoxes);
    ownerBoxes->setData(&data.ownerFlags);

    auto *userInput = new TInputLine(TRect(28, 11, 60, 12), sizeof(data.user) - 1);
    dialog->insert(new TLabel(TRect(28, 10, 60, 11), "User name:", userInput));
    dialog->insert(userInput);
    userInput->setData(data.user);

    auto *uidInput = new TInputLine(TRect(28, 12, 60, 13), sizeof(data.uid) - 1);
    dialog->insert(new TLabel(TRect(28, 11, 60, 12), "UID:", uidInput));
    dialog->insert(uidInput);
    uidInput->setData(data.uid);

    auto *groupInput = new TInputLine(TRect(28, 14, 60, 15), sizeof(data.group) - 1);
    dialog->insert(new TLabel(TRect(28, 13, 60, 14), "Group:", groupInput));
    dialog->insert(groupInput);
    groupInput->setData(data.group);

    auto *gidInput = new TInputLine(TRect(28, 15, 60, 16), sizeof(data.gid) - 1);
    dialog->insert(new TLabel(TRect(28, 14, 60, 15), "GID:", gidInput));
    dialog->insert(gidInput);
    gidInput->setData(data.gid);

    dialog->insert(new TStaticText(TRect(3, 19, 64, 21),
                                   "Specify numeric IDs or names. Leave unused fields blank."));

    dialog->insert(new TButton(TRect(24, 20, 34, 22), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(36, 20, 46, 22), "Cancel", cmCancel, bfNormal));

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        permBoxes->getData(&data.permFlags);
        permMode->getData(&data.mode);
        ownerBoxes->getData(&data.ownerFlags);
        permInput->getData(data.permSpec);
        userInput->getData(data.user);
        uidInput->getData(data.uid);
        groupInput->getData(data.group);
        gidInput->getData(data.gid);

        options.permEnabled = (data.permFlags & 0x0001) != 0;
        options.readable = (data.permFlags & 0x0002) != 0;
        options.writable = (data.permFlags & 0x0004) != 0;
        options.executable = (data.permFlags & 0x0008) != 0;
        options.permMode = static_cast<PermissionOwnershipOptions::PermMode>(data.mode);

        options.userEnabled = (data.ownerFlags & 0x0001) != 0;
        options.uidEnabled = (data.ownerFlags & 0x0002) != 0;
        options.groupEnabled = (data.ownerFlags & 0x0004) != 0;
        options.gidEnabled = (data.ownerFlags & 0x0008) != 0;
        options.noUser = (data.ownerFlags & 0x0010) != 0;
        options.noGroup = (data.ownerFlags & 0x0020) != 0;

        copyToArray(options.permSpec, data.permSpec);
        copyToArray(options.user, data.user);
        copyToArray(options.uid, data.uid);
        copyToArray(options.group, data.group);
        copyToArray(options.gid, data.gid);
    }

    TObject::destroy(dialog);
    return accepted;
}

bool editTraversalFilters(TraversalFilesystemOptions &options)
{
    struct Data
    {
        ushort flags = 0;
        ushort valueFlags = 0;
        ushort symlinkMode = 0;
        ushort warningMode = 0;
        char maxDepth[8]{};
        char minDepth[8]{};
        char filesFrom[PATH_MAX]{};
        char fsType[64]{};
        char linkCount[16]{};
        char sameFile[PATH_MAX]{};
        char inode[32]{};
    } data{};

    if (options.depthFirst)
        data.flags |= 0x0001;
    if (options.stayOnFilesystem)
        data.flags |= 0x0002;
    if (options.assumeNoLeaf)
        data.flags |= 0x0004;
    if (options.ignoreReaddirRace)
        data.flags |= 0x0008;
    if (options.dayStart)
        data.flags |= 0x0010;

    if (options.maxDepthEnabled)
        data.valueFlags |= 0x0001;
    if (options.minDepthEnabled)
        data.valueFlags |= 0x0002;
    if (options.filesFromEnabled)
        data.valueFlags |= 0x0004;
    if (options.filesFromNullSeparated)
        data.valueFlags |= 0x0008;
    if (options.fstypeEnabled)
        data.valueFlags |= 0x0010;
    if (options.linksEnabled)
        data.valueFlags |= 0x0020;
    if (options.sameFileEnabled)
        data.valueFlags |= 0x0040;
    if (options.inumEnabled)
        data.valueFlags |= 0x0080;

    data.symlinkMode = static_cast<ushort>(options.symlinkMode);
    data.warningMode = static_cast<ushort>(options.warningMode);

    std::snprintf(data.maxDepth, sizeof(data.maxDepth), "%s", bufferToString(options.maxDepth).c_str());
    std::snprintf(data.minDepth, sizeof(data.minDepth), "%s", bufferToString(options.minDepth).c_str());
    std::snprintf(data.filesFrom, sizeof(data.filesFrom), "%s", bufferToString(options.filesFrom).c_str());
    std::snprintf(data.fsType, sizeof(data.fsType), "%s", bufferToString(options.fsType).c_str());
    std::snprintf(data.linkCount, sizeof(data.linkCount), "%s", bufferToString(options.linkCount).c_str());
    std::snprintf(data.sameFile, sizeof(data.sameFile), "%s", bufferToString(options.sameFile).c_str());
    std::snprintf(data.inode, sizeof(data.inode), "%s", bufferToString(options.inode).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 80, 24), "Traversal & Filesystem");
    dialog->options |= ofCentered;

    auto *symlinkButtons = new TRadioButtons(TRect(3, 3, 30, 9),
                                             makeItemList({"Physical (-P)",
                                                           "Follow args (-H)",
                                                           "Follow all (-L)"}));
    dialog->insert(symlinkButtons);
    symlinkButtons->setData(&data.symlinkMode);

    auto *warningButtons = new TRadioButtons(TRect(32, 3, 64, 8),
                                             makeItemList({"Default warnings",
                                                           "Always warn (-warn)",
                                                           "Suppress (-nowarn)"}));
    dialog->insert(warningButtons);
    warningButtons->setData(&data.warningMode);

    auto *flagBoxes = new TCheckBoxes(TRect(3, 9, 30, 17),
                                      makeItemList({"Use -~d~epth",
                                                    "Stay on file~s~ystem",
                                                    "Assume -nolea~f~",
                                                    "Ignore readdir race",
                                                    "Use -day~s~tart"}));
    dialog->insert(flagBoxes);
    flagBoxes->setData(&data.flags);

    auto *valueBoxes = new TCheckBoxes(TRect(32, 9, 60, 17),
                                       makeItemList({"Limit ~m~ax depth",
                                                     "Limit mi~n~ depth",
                                                     "Paths from ~f~ile",
                                                     "List is ~N~UL separated",
                                                     "Filter ~f~stype",
                                                     "Match ~l~inks",
                                                     "Match ~s~amefile",
                                                     "Match ~i~node"}));
    dialog->insert(valueBoxes);
    valueBoxes->setData(&data.valueFlags);

    auto *maxInput = new TInputLine(TRect(62, 9, 72, 10), sizeof(data.maxDepth) - 1);
    dialog->insert(new TLabel(TRect(60, 9, 62, 10), "Max:", maxInput));
    dialog->insert(maxInput);
    maxInput->setData(data.maxDepth);

    auto *minInput = new TInputLine(TRect(62, 10, 72, 11), sizeof(data.minDepth) - 1);
    dialog->insert(new TLabel(TRect(60, 10, 62, 11), "Min:", minInput));
    dialog->insert(minInput);
    minInput->setData(data.minDepth);

    auto *linksInput = new TInputLine(TRect(62, 11, 72, 12), sizeof(data.linkCount) - 1);
    dialog->insert(new TLabel(TRect(60, 11, 62, 12), "Links:", linksInput));
    dialog->insert(linksInput);
    linksInput->setData(data.linkCount);

    auto *inodeInput = new TInputLine(TRect(62, 12, 72, 13), sizeof(data.inode) - 1);
    dialog->insert(new TLabel(TRect(60, 12, 62, 13), "Inode:", inodeInput));
    dialog->insert(inodeInput);
    inodeInput->setData(data.inode);

    auto *filesFromInput = new TInputLine(TRect(3, 17, 74, 18), sizeof(data.filesFrom) - 1);
    dialog->insert(new TLabel(TRect(3, 16, 30, 17), "--files-from path:", filesFromInput));
    dialog->insert(filesFromInput);
    filesFromInput->setData(data.filesFrom);

    auto *fsTypeInput = new TInputLine(TRect(3, 18, 40, 19), sizeof(data.fsType) - 1);
    dialog->insert(new TLabel(TRect(3, 17, 24, 18), "-fstype:", fsTypeInput));
    dialog->insert(fsTypeInput);
    fsTypeInput->setData(data.fsType);

    auto *sameFileInput = new TInputLine(TRect(3, 20, 74, 21), sizeof(data.sameFile) - 1);
    dialog->insert(new TLabel(TRect(3, 19, 28, 20), "-samefile path:", sameFileInput));
    dialog->insert(sameFileInput);
    sameFileInput->setData(data.sameFile);

    dialog->insert(new TStaticText(TRect(3, 21, 74, 23),
                                   "Use --files-from for large start lists. Options map directly to"
                                   " GNU find switches."));

    dialog->insert(new TButton(TRect(30, 22, 40, 24), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(42, 22, 52, 24), "Cancel", cmCancel, bfNormal));

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        symlinkButtons->getData(&data.symlinkMode);
        warningButtons->getData(&data.warningMode);
        flagBoxes->getData(&data.flags);
        valueBoxes->getData(&data.valueFlags);
        maxInput->getData(data.maxDepth);
        minInput->getData(data.minDepth);
        filesFromInput->getData(data.filesFrom);
        fsTypeInput->getData(data.fsType);
        linksInput->getData(data.linkCount);
        sameFileInput->getData(data.sameFile);
        inodeInput->getData(data.inode);

        options.symlinkMode = static_cast<TraversalFilesystemOptions::SymlinkMode>(data.symlinkMode);
        options.warningMode = static_cast<TraversalFilesystemOptions::WarningMode>(data.warningMode);
        options.depthFirst = (data.flags & 0x0001) != 0;
        options.stayOnFilesystem = (data.flags & 0x0002) != 0;
        options.assumeNoLeaf = (data.flags & 0x0004) != 0;
        options.ignoreReaddirRace = (data.flags & 0x0008) != 0;
        options.dayStart = (data.flags & 0x0010) != 0;

        options.maxDepthEnabled = (data.valueFlags & 0x0001) != 0;
        options.minDepthEnabled = (data.valueFlags & 0x0002) != 0;
        options.filesFromEnabled = (data.valueFlags & 0x0004) != 0;
        options.filesFromNullSeparated = (data.valueFlags & 0x0008) != 0;
        options.fstypeEnabled = (data.valueFlags & 0x0010) != 0;
        options.linksEnabled = (data.valueFlags & 0x0020) != 0;
        options.sameFileEnabled = (data.valueFlags & 0x0040) != 0;
        options.inumEnabled = (data.valueFlags & 0x0080) != 0;

        copyToArray(options.maxDepth, data.maxDepth);
        copyToArray(options.minDepth, data.minDepth);
        copyToArray(options.filesFrom, data.filesFrom);
        copyToArray(options.fsType, data.fsType);
        copyToArray(options.linkCount, data.linkCount);
        copyToArray(options.sameFile, data.sameFile);
        copyToArray(options.inode, data.inode);
    }

    TObject::destroy(dialog);
    return accepted;
}

bool editActionOptions(ActionOptions &options)
{
    struct Data
    {
        ushort flags = 0;
        ushort appendFlags = 0;
        ushort execVariant = 0;
        char execCommand[512]{};
        char fprintFile[PATH_MAX]{};
        char fprint0File[PATH_MAX]{};
        char flsFile[PATH_MAX]{};
        char printfFormat[256]{};
        char fprintfFile[PATH_MAX]{};
        char fprintfFormat[256]{};
    } data{};

    if (options.print)
        data.flags |= 0x0001;
    if (options.print0)
        data.flags |= 0x0002;
    if (options.ls)
        data.flags |= 0x0004;
    if (options.deleteMatches)
        data.flags |= 0x0008;
    if (options.quitEarly)
        data.flags |= 0x0010;
    if (options.execEnabled)
        data.flags |= 0x0020;
    if (options.execUsePlus)
        data.flags |= 0x0040;

    if (options.fprintAppend)
        data.appendFlags |= 0x0001;
    if (options.fprint0Append)
        data.appendFlags |= 0x0002;
    if (options.flsAppend)
        data.appendFlags |= 0x0004;
    if (options.fprintfAppend)
        data.appendFlags |= 0x0008;

    data.execVariant = static_cast<ushort>(options.execVariant);

    std::snprintf(data.execCommand, sizeof(data.execCommand), "%s", bufferToString(options.execCommand).c_str());
    std::snprintf(data.fprintFile, sizeof(data.fprintFile), "%s", bufferToString(options.fprintFile).c_str());
    std::snprintf(data.fprint0File, sizeof(data.fprint0File), "%s", bufferToString(options.fprint0File).c_str());
    std::snprintf(data.flsFile, sizeof(data.flsFile), "%s", bufferToString(options.flsFile).c_str());
    std::snprintf(data.printfFormat, sizeof(data.printfFormat), "%s", bufferToString(options.printfFormat).c_str());
    std::snprintf(data.fprintfFile, sizeof(data.fprintfFile), "%s", bufferToString(options.fprintfFile).c_str());
    std::snprintf(data.fprintfFormat, sizeof(data.fprintfFormat), "%s", bufferToString(options.fprintfFormat).c_str());

    auto *dialog = new TDialog(TRect(0, 0, 78, 24), "Actions & Output");
    dialog->options |= ofCentered;

    auto *actionBoxes = new TCheckBoxes(TRect(3, 3, 30, 10),
                                        makeItemList({"-~p~rint",
                                                      "-print~0~",
                                                      "-~l~s",
                                                      "-~d~elete",
                                                      "-~q~uit",
                                                      "-~e~xec / -ok",
                                                      "Use '+' terminator"}));
    dialog->insert(actionBoxes);
    actionBoxes->setData(&data.flags);

    auto *execButtons = new TRadioButtons(TRect(32, 3, 60, 9),
                                          makeItemList({"-exec",
                                                        "-execdir",
                                                        "-ok",
                                                        "-okdir"}));
    dialog->insert(execButtons);
    execButtons->setData(&data.execVariant);

    auto *execInput = new TInputLine(TRect(3, 10, 74, 11), sizeof(data.execCommand) - 1);
    dialog->insert(new TLabel(TRect(3, 9, 36, 10), "Command ({} for path):", execInput));
    dialog->insert(execInput);
    execInput->setData(data.execCommand);

    auto *appendBoxes = new TCheckBoxes(TRect(3, 12, 30, 18),
                                        makeItemList({"Append -fprint",
                                                      "Append -fprint0",
                                                      "Append -fls",
                                                      "Append -fprintf"}));
    dialog->insert(appendBoxes);
    appendBoxes->setData(&data.appendFlags);

    auto pathLen = std::min<int>(static_cast<int>(sizeof(data.fprintFile)) - 1, 255);
    auto *fprintInput = new TInputLine(TRect(32, 12, 74, 13), pathLen);
    dialog->insert(new TLabel(TRect(32, 11, 58, 12), "-fprint file:", fprintInput));
    dialog->insert(fprintInput);
    fprintInput->setData(data.fprintFile);

    auto *fprint0Input = new TInputLine(TRect(32, 13, 74, 14), pathLen);
    dialog->insert(new TLabel(TRect(32, 12, 58, 13), "-fprint0 file:", fprint0Input));
    dialog->insert(fprint0Input);
    fprint0Input->setData(data.fprint0File);

    auto *flsInput = new TInputLine(TRect(32, 14, 74, 15), pathLen);
    dialog->insert(new TLabel(TRect(32, 13, 58, 14), "-fls file:", flsInput));
    dialog->insert(flsInput);
    flsInput->setData(data.flsFile);

    auto *printfInput = new TInputLine(TRect(32, 15, 74, 16), sizeof(data.printfFormat) - 1);
    dialog->insert(new TLabel(TRect(32, 14, 66, 15), "-printf format:", printfInput));
    dialog->insert(printfInput);
    printfInput->setData(data.printfFormat);

    auto *fprintfFileInput = new TInputLine(TRect(32, 16, 74, 17), pathLen);
    dialog->insert(new TLabel(TRect(32, 15, 66, 16), "-fprintf file:", fprintfFileInput));
    dialog->insert(fprintfFileInput);
    fprintfFileInput->setData(data.fprintfFile);

    auto *fprintfFormatInput = new TInputLine(TRect(32, 17, 74, 18), sizeof(data.fprintfFormat) - 1);
    dialog->insert(new TLabel(TRect(32, 16, 66, 17), "-fprintf format:", fprintfFormatInput));
    dialog->insert(fprintfFormatInput);
    fprintfFormatInput->setData(data.fprintfFormat);

    dialog->insert(new TStaticText(TRect(3, 18, 74, 20),
                                   "Commands use {} for the current path. Output files are optional;"
                                   " leave blank to skip."));

    dialog->insert(new TButton(TRect(30, 20, 40, 22), "O~K~", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(42, 20, 52, 22), "Cancel", cmCancel, bfNormal));

    ushort result = TProgram::application->executeDialog(dialog, &data);
    bool accepted = (result == cmOK);
    if (accepted)
    {
        actionBoxes->getData(&data.flags);
        appendBoxes->getData(&data.appendFlags);
        execButtons->getData(&data.execVariant);
        execInput->getData(data.execCommand);
        fprintInput->getData(data.fprintFile);
        fprint0Input->getData(data.fprint0File);
        flsInput->getData(data.flsFile);
        printfInput->getData(data.printfFormat);
        fprintfFileInput->getData(data.fprintfFile);
        fprintfFormatInput->getData(data.fprintfFormat);

        options.print = (data.flags & 0x0001) != 0;
        options.print0 = (data.flags & 0x0002) != 0;
        options.ls = (data.flags & 0x0004) != 0;
        options.deleteMatches = (data.flags & 0x0008) != 0;
        options.quitEarly = (data.flags & 0x0010) != 0;
        options.execEnabled = (data.flags & 0x0020) != 0;
        options.execUsePlus = (data.flags & 0x0040) != 0;
        options.execVariant = static_cast<ActionOptions::ExecVariant>(data.execVariant);

        options.fprintAppend = (data.appendFlags & 0x0001) != 0;
        options.fprint0Append = (data.appendFlags & 0x0002) != 0;
        options.flsAppend = (data.appendFlags & 0x0004) != 0;
        options.fprintfAppend = (data.appendFlags & 0x0008) != 0;

        copyToArray(options.execCommand, data.execCommand);
        copyToArray(options.fprintFile, data.fprintFile);
        copyToArray(options.fprint0File, data.fprint0File);
        copyToArray(options.flsFile, data.flsFile);
        copyToArray(options.printfFormat, data.printfFormat);
        copyToArray(options.fprintfFile, data.fprintfFile);
        copyToArray(options.fprintfFormat, data.fprintfFormat);

        options.fprintEnabled = data.fprintFile[0] != '\0';
        options.fprint0Enabled = data.fprint0File[0] != '\0';
        options.flsEnabled = data.flsFile[0] != '\0';
        options.printfEnabled = data.printfFormat[0] != '\0';
        options.fprintfEnabled = data.fprintfFile[0] != '\0' || data.fprintfFormat[0] != '\0';
    }

    TObject::destroy(dialog);
    return accepted;
}

class FindApp : public TApplication
{
public:
    FindApp(int, char **)
        : TProgInit(&FindApp::initStatusLine, &FindApp::initMenuBar, &TApplication::initDeskTop),
          TApplication()
    {
        m_spec = makeDefaultSpecification();
    }

    void handleEvent(TEvent &event) override
    {
        TApplication::handleEvent(event);
        if (event.what == evCommand)
        {
            switch (event.message.command)
            {
            case cmNewSearch:
                newSearch();
                break;
            case cmLoadSpec:
                messageBox("Loading saved specifications will arrive in a future milestone.", mfInformation | mfOKButton);
                break;
            case cmSaveSpec:
                messageBox("Saving specifications will arrive in a future milestone.", mfInformation | mfOKButton);
                break;
            case cmReturnToLauncher:
                std::exit(ck::launcher::kReturnToLauncherExitCode);
                break;
            case cmAbout:
            {
                const auto &info = toolInfo();
                ck::ui::showAboutDialog(info.executable, CK_FIND_VERSION, info.aboutDescription);
                break;
            }
            default:
                return;
            }
            clearEvent(event);
        }
    }

    static TMenuBar *initMenuBar(TRect r)
    {
        r.b.y = r.a.y + 1;
        TSubMenu &fileMenu = *new TSubMenu("~F~ile", hcNoContext) +
                             *new TMenuItem("~N~ew Search...", cmNewSearch, kbF2, hcNoContext, "F2") +
                             *new TMenuItem("~L~oad Search Spec...", cmLoadSpec, kbCtrlO, hcNoContext, "Ctrl-O") +
                             *new TMenuItem("~S~ave Search Spec...", cmSaveSpec, kbCtrlS, hcNoContext, "Ctrl-S") +
                             newLine();
        if (ck::launcher::launchedFromCkLauncher())
            fileMenu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher, kbCtrlL, hcNoContext, "Ctrl-L");
        fileMenu + *new TMenuItem("E~x~it", cmQuit, kbAltX, hcNoContext, "Alt-X");

        TMenuItem &menuChain = fileMenu +
                               *new TSubMenu("~H~elp", hcNoContext) +
                                   *new TMenuItem("~A~bout", cmAbout, kbF1, hcNoContext, "F1");

        return new TMenuBar(r, static_cast<TSubMenu &>(menuChain));
    }

    static TStatusLine *initStatusLine(TRect r)
    {
        r.a.y = r.b.y - 1;
        return new FindStatusLine(r);
    }

private:
    SearchSpecification m_spec{};

    void newSearch()
    {
        SearchSpecification candidate = m_spec;
        if (configureSearchSpecification(candidate))
        {
            m_spec = candidate;
            std::string summary = buildPlaceholderSummary(m_spec);
            messageBox(summary.c_str(), mfInformation | mfOKButton);
        }
    }
};

} // namespace

int main(int argc, char **argv)
{
    FindApp app(argc, argv);
    app.run();
    return 0;
}
