#include "ck/find/search_dialogs.hpp"

#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/dialog_utils.hpp"
#include "ck/find/search_model.hpp"

#include "command_ids.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#define Uses_MsgBox
#define Uses_TButton
#define Uses_TCheckBoxes
#define Uses_TDialog
#define Uses_TChDirDialog
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

struct SearchDialogData
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

class SearchDialog : public TDialog
{
public:
    SearchDialog(SearchSpecification &spec, SearchDialogData &data)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 83, 25), "New Search"),
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

    void setStartInput(TInputLine *input)
    {
        m_startInput = input;
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
                browseStartLocation();
                clearEvent(event);
                return;
            default:
                break;
            }
        }
        TDialog::handleEvent(event);
    }

private:
    void browseStartLocation()
    {
        if (!m_startInput)
            return;

        char location[PATH_MAX]{};
        std::snprintf(location, sizeof(location), "%s", m_data.startLocation[0] ? m_data.startLocation : ".");

        auto *dialog = new TChDirDialog(cdNormal, 1);
        dialog->setData(location);
        unsigned short result = TProgram::application->executeDialog(dialog, location);
        TObject::destroy(dialog);
        if (result == cmCancel)
            return;

        std::snprintf(m_data.startLocation, sizeof(m_data.startLocation), "%s", location);
        m_startInput->setData(location);
    }

    SearchSpecification &m_spec;
    SearchDialogData &m_data;
    TCheckBoxes *m_primaryBoxes = nullptr;
    TCheckBoxes *m_secondaryBoxes = nullptr;
    TInputLine *m_startInput = nullptr;
};

} // namespace

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

    dialog->insert(new TStaticText(TRect(3, 3, 79, 5),
                                   "Start with directories and optional text. Advanced buttons\n"
                                   "map directly to sensible find(1) switches."));

    auto *startInput = new TInputLine(TRect(3, 6, 60, 7), sizeof(data.startLocation) - 1);
    dialog->insert(new TLabel(TRect(2, 5, 26, 6), "Start ~l~ocation:", startInput));
    dialog->insert(startInput);
    startInput->setData(data.startLocation);
    dialog->setStartInput(startInput);
    dialog->insert(new TButton(TRect(61, 6, 75, 8), "~B~rowse...", cmBrowseStart, bfNormal));

    auto *textInput = new TInputLine(TRect(3, 8, 75, 9), sizeof(data.searchText) - 1);
    dialog->insert(new TLabel(TRect(2, 7, 24, 8), "Te~x~t to find:", textInput));
    dialog->insert(textInput);
    textInput->setData(data.searchText);

    auto *includeInput = new TInputLine(TRect(3, 10, 38, 11), sizeof(data.includePatterns) - 1);
    dialog->insert(new TLabel(TRect(2, 9, 28, 10), "Include patterns:", includeInput));
    dialog->insert(includeInput);
    includeInput->setData(data.includePatterns);

    auto *excludeInput = new TInputLine(TRect(40, 10, 79, 11), sizeof(data.excludePatterns) - 1);
    dialog->insert(new TLabel(TRect(39, 9, 79, 10), "Exclude patterns:", excludeInput));
    dialog->insert(excludeInput);
    excludeInput->setData(data.excludePatterns);

    auto *generalBoxes = new TCheckBoxes(TRect(3, 11, 38, 17),
                                         makeItemList({"~R~ecursive",
                                                       "Include ~h~idden",
                                                       "Follow s~y~mlinks (-L)",
                                                       "Stay on same file ~s~ystem"}));
    dialog->insert(generalBoxes);
    generalBoxes->setData(&data.generalFlags);

    auto *primaryBoxes = new TCheckBoxes(TRect(39, 11, 61, 17),
                                         makeItemList({"~T~ext search",
                                                       "Name/~P~ath tests",
                                                       "~T~ime tests",
                                                       "Si~z~e filters",
                                                       "File ~t~ype filters"}));
    dialog->insert(primaryBoxes);
    primaryBoxes->setData(&data.optionPrimaryFlags);
    dialog->setPrimaryBoxes(primaryBoxes);

    auto *secondaryBoxes = new TCheckBoxes(TRect(62, 11, 81, 17),
                                           makeItemList({"~P~ermissions & owners",
                                                         "T~r~aversal / FS",
                                                         "~A~ctions & output"}));
    dialog->insert(secondaryBoxes);
    secondaryBoxes->setData(&data.optionSecondaryFlags);
    dialog->setSecondaryBoxes(secondaryBoxes);

    dialog->insert(new TButton(TRect(3, 18, 21, 20), "Text ~O~ptions...", cmTextOptions, bfNormal));
    dialog->insert(new TButton(TRect(23, 18, 41, 20), "Name/~P~ath...", cmNamePathOptions, bfNormal));
    dialog->insert(new TButton(TRect(43, 18, 61, 20), "Time ~T~ests...", cmTimeFilters, bfNormal));
    dialog->insert(new TButton(TRect(63, 18, 81, 20), "Si~z~e Filters...", cmSizeFilters, bfNormal));

    dialog->insert(new TButton(TRect(3, 20, 21, 22), "File ~T~ypes...", cmTypeFilters, bfNormal));
    dialog->insert(new TButton(TRect(23, 20, 45, 22), "~P~ermissions...", cmPermissionOwnership, bfNormal));
    dialog->insert(new TButton(TRect(47, 20, 71, 22), "T~r~aversal / FS...", cmTraversalFilters, bfNormal));

    dialog->insert(new TButton(TRect(3, 22, 21, 24), "~A~ctions...", cmActionOptions, bfNormal));
    dialog->insert(new TButton(TRect(23, 22, 37, 24), "~L~oad Spec...", cmDialogLoadSpec, bfNormal));
    dialog->insert(new TButton(TRect(39, 22, 53, 24), "Sa~v~e Spec...", cmDialogSaveSpec, bfNormal));

    dialog->insert(new TButton(TRect(55, 22, 69, 24), "~S~earch", cmOK, bfDefault));
    dialog->insert(new TButton(TRect(71, 22, 81, 24), "Cancel", cmCancel, bfNormal));

    dialog->selectNext(False);

    unsigned short result = TProgram::application->executeDialog(dialog, &data);
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

} // namespace ck::find

