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
};

class PlaceholderPage : public ck::ui::TabPageView
{
public:
    PlaceholderPage(const TRect &bounds, const char *message)
        : ck::ui::TabPageView(bounds)
    {
        TRect textRect = bounds;
        textRect.a.x += 2;
        textRect.a.y += 2;
        textRect.b.x = std::max<short>(textRect.a.x + 32, textRect.b.x - 2);
        textRect.b.y = std::max<short>(textRect.a.y + 2, textRect.b.y - 2);
        insert(new TStaticText(textRect, message));
    }
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
    insert(new TLabel(TRect(1, 3, 27, 4), "Start ~l~ocation:", m_startInput));
    insert(m_startInput);
    insert(new TButton(TRect(61, 4, 77, 6), "~B~rowse...", cmBrowseStart, bfNormal));

    m_searchTextInput = new TInputLine(TRect(2, 6, 77, 7), sizeof(m_state.searchText) - 1);
    insert(new TLabel(TRect(1, 5, 25, 6), "Te~x~t to find:", m_searchTextInput));
    insert(m_searchTextInput);

    m_includeInput = new TInputLine(TRect(2, 8, 38, 9), sizeof(m_state.includePatterns) - 1);
    insert(new TLabel(TRect(1, 7, 28, 8), "Include patterns:", m_includeInput));
    insert(m_includeInput);

    m_excludeInput = new TInputLine(TRect(40, 8, 77, 9), sizeof(m_state.excludePatterns) - 1);
    insert(new TLabel(TRect(39, 7, 76, 8), "Exclude patterns:", m_excludeInput));
    insert(m_excludeInput);

    m_generalBoxes = new TCheckBoxes(TRect(2, 10, 32, 15),
                                     makeItemList({"~R~ecursive",
                                                   "Include ~h~idden",
                                                   "Follow s~y~mlinks",
                                                   "Stay on same file ~s~ystem"}));
    insert(m_generalBoxes);

    m_primaryBoxes = new TCheckBoxes(TRect(34, 10, 56, 15),
                                     makeItemList({"~T~ext search",
                                                   "Name/~P~ath",
                                                   "~T~ime filters",
                                                   "Si~z~e filters",
                                                   "File ~t~ype filters"}));
    insert(m_primaryBoxes);

    m_secondaryBoxes = new TCheckBoxes(TRect(58, 10, 77, 15),
                                       makeItemList({"~P~ermissions",
                                                     "T~r~aversal",
                                                     "~A~ctions"}));
    insert(m_secondaryBoxes);

    insert(new TButton(TRect(2, 16, 18, 18), "Text ~O~ptions...", cmTextOptions, bfNormal));
    insert(new TButton(TRect(20, 16, 36, 18), "Name/~P~ath...", cmNamePathOptions, bfNormal));
    insert(new TButton(TRect(38, 16, 54, 18), "Time ~T~ests...", cmTimeFilters, bfNormal));
    insert(new TButton(TRect(56, 16, 72, 18), "Si~z~e Filters...", cmSizeFilters, bfNormal));

    insert(new TButton(TRect(2, 18, 18, 20), "File ~T~ypes...", cmTypeFilters, bfNormal));
    insert(new TButton(TRect(20, 18, 38, 20), "~P~ermissions...", cmPermissionOwnership, bfNormal));
    insert(new TButton(TRect(40, 18, 58, 20), "T~r~aversal...", cmTraversalFilters, bfNormal));
    insert(new TButton(TRect(60, 18, 78, 20), "~A~ctions...", cmActionOptions, bfNormal));

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

    SearchSpecification &m_spec;
    SearchNotebookState &m_state;
    ck::ui::TabControl *m_tabControl = nullptr;
    QuickStartPage *m_quickStartPage = nullptr;
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

    auto createPlaceholder = [&](const char *title, const char *message, unsigned short command) {
        auto *page = m_tabControl->createTab(title, command);
        if (page)
        {
            TRect textBounds(2, 2, 78, 18);
            page->insert(new TStaticText(textBounds, message));
        }
    };

    createPlaceholder("Content", "Content & Names tab coming soon.", cmTabContentNames);
    createPlaceholder("Dates", "Dates & Sizes tab coming soon.", cmTabDatesSizes);
    createPlaceholder("Types", "Types & Ownership tab coming soon.", cmTabTypesOwnership);
    createPlaceholder("Traverse", "Traversal tab coming soon.", cmTabTraversal);
    createPlaceholder("Actions", "Actions tab coming soon.", cmTabActions);

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
            }
            clearEvent(event);
            return;
        case cmNamePathOptions:
            if (editNamePathOptions(m_spec.namePathOptions))
            {
                m_state.optionPrimaryFlags |= kOptionNamePathBit;
                if (m_quickStartPage)
                    m_quickStartPage->syncOptionFlags();
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

    m_spec.enableTextSearch = (m_state.optionPrimaryFlags & kOptionTextBit) != 0;
    m_spec.enableNamePathTests = (m_state.optionPrimaryFlags & kOptionNamePathBit) != 0;
    m_spec.enableTimeFilters = (m_state.optionPrimaryFlags & kOptionTimeBit) != 0;
    m_spec.enableSizeFilters = (m_state.optionPrimaryFlags & kOptionSizeBit) != 0;
    m_spec.enableTypeFilters = (m_state.optionPrimaryFlags & kOptionTypeBit) != 0;

    m_spec.enablePermissionOwnership = (m_state.optionSecondaryFlags & kOptionPermissionBit) != 0;
    m_spec.enableTraversalFilters = (m_state.optionSecondaryFlags & kOptionTraversalBit) != 0;
    m_spec.enableActionOptions = (m_state.optionSecondaryFlags & kOptionActionBit) != 0;
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

    auto *dialog = new SearchNotebookDialog(spec, state);
    unsigned short result = TProgram::application->executeDialog(dialog);
    return result == cmOK;
}

} // namespace ck::find
