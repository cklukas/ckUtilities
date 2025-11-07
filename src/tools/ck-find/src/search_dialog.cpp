#include "ck/find/search_dialogs.hpp"

#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/dialog_utils.hpp"
#include "ck/find/search_backend.hpp"
#include "ck/find/guided_search.hpp"

#include "command_ids.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define Uses_MsgBox
#define Uses_TButton
#define Uses_TCheckBoxes
#define Uses_TChDirDialog
#define Uses_TDialog
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TListBox
#define Uses_TScrollBar
#define Uses_TStringCollection
#define Uses_TRadioButtons
#define Uses_TProgram
#define Uses_TParamText
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

TRect lineRect(short left, short top, short right)
{
    return TRect(left, top, right, static_cast<short>(top + 1));
}

constexpr unsigned short kLocationSubfoldersBit = 0x0001;
constexpr unsigned short kLocationHiddenBit = 0x0002;
constexpr unsigned short kLocationSymlinkBit = 0x0004;
constexpr unsigned short kLocationStayFsBit = 0x0008;

constexpr unsigned short kTextFlagMatchCaseBit = 0x0001;
constexpr unsigned short kTextFlagAllowMultipleBit = 0x0002;
constexpr unsigned short kTextFlagTreatBinaryBit = 0x0004;

constexpr unsigned short kActionPreviewBit = 0x0001;
constexpr unsigned short kActionListBit = 0x0002;
constexpr unsigned short kActionDeleteBit = 0x0004;
constexpr unsigned short kActionCommandBit = 0x0008;

constexpr unsigned short kFilterAdvancedPermBit = 0x0001;
constexpr unsigned short kFilterAdvancedTraversalBit = 0x0002;

constexpr unsigned short cmShowPopularPresets = 0xf300;
constexpr unsigned short cmShowExpertRecipes = 0xf301;

constexpr unsigned short cmDeleteSavedSpecification = 0xf302;

class PresetPickerDialog : public TDialog
{
public:
    PresetPickerDialog(std::string_view title,
                       std::span<const GuidedSearchPreset> presets)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 60, 18), title.data())
    {
        options |= ofCentered;

        m_list = new TListBox(TRect(2, 2, 28, 13), 1, nullptr);
        insert(m_list);

        m_summary = new TParamText(TRect(30, 2, 58, 13));
        insert(m_summary);

        insert(new TButton(TRect(16, 14, 28, 16), "~U~se preset", cmOK, bfDefault));
        insert(new TButton(TRect(30, 14, 42, 16), "Cancel", cmCancel, bfNormal));

        for (const auto &preset : presets)
        {
            m_entries.push_back(&preset);
        }

        refreshList();
        updateSummary();
    }

    int selection() const
    {
        return m_list ? m_list->focused : -1;
    }

    const GuidedSearchPreset *selectedPreset() const
    {
        int index = selection();
        if (index < 0 || index >= static_cast<int>(m_entries.size()))
            return nullptr;
        return m_entries[static_cast<std::size_t>(index)];
    }

protected:
    void handleEvent(TEvent &event) override
    {
        TDialog::handleEvent(event);
        if (event.what == evBroadcast && event.message.command == cmListItemSelected)
            updateSummary();
        else if (event.what == evCommand && (event.message.command == cmOK || event.message.command == cmCancel))
            updateSummary();
    }

private:
    void refreshList()
    {
        auto *collection = new TStringCollection(10, 5);
        for (const auto *entry : m_entries)
        {
            collection->insert(newStr(entry->title.data()));
        }
        m_list->newList(collection);
        if (!m_entries.empty())
            m_list->focusItem(0);
    }

    void updateSummary()
    {
        const GuidedSearchPreset *entry = selectedPreset();
        if (!entry)
        {
            m_summary->setText("%s", "Select a preset to see details.");
            return;
        }
        std::snprintf(m_summaryBuffer.data(),
                      m_summaryBuffer.size(),
                      "%s",
                      entry->subtitle.data());
        m_summary->setText("%s", m_summaryBuffer.data());
    }

    std::vector<const GuidedSearchPreset *> m_entries;
    TListBox *m_list = nullptr;
    TParamText *m_summary = nullptr;
    std::array<char, 128> m_summaryBuffer{};
};

class RecipePickerDialog : public TDialog
{
public:
    RecipePickerDialog(std::string_view title,
                       std::span<const GuidedRecipe> recipes)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 62, 18), title.data())
    {
        options |= ofCentered;

        m_list = new TListBox(TRect(2, 2, 30, 13), 1, nullptr);
        insert(m_list);

        m_summary = new TParamText(TRect(32, 2, 60, 13));
        insert(m_summary);

        insert(new TButton(TRect(18, 14, 30, 16), "~R~un recipe", cmOK, bfDefault));
        insert(new TButton(TRect(32, 14, 44, 16), "Cancel", cmCancel, bfNormal));

        for (const auto &recipe : recipes)
            m_entries.push_back(&recipe);

        refreshList();
        updateSummary();
    }

    const GuidedRecipe *selectedRecipe() const
    {
        int index = m_list ? m_list->focused : -1;
        if (index < 0 || index >= static_cast<int>(m_entries.size()))
            return nullptr;
        return m_entries[static_cast<std::size_t>(index)];
    }

protected:
    void handleEvent(TEvent &event) override
    {
        TDialog::handleEvent(event);
        if (event.what == evBroadcast && event.message.command == cmListItemSelected)
            updateSummary();
    }

private:
    void refreshList()
    {
        auto *collection = new TStringCollection(10, 5);
        for (const auto *entry : m_entries)
            collection->insert(newStr(entry->title.data()));
        m_list->newList(collection);
        if (!m_entries.empty())
            m_list->focusItem(0);
    }

    void updateSummary()
    {
        const GuidedRecipe *entry = selectedRecipe();
        if (!entry)
        {
            m_summary->setText("%s", "Pick a recipe to see details.");
            return;
        }
        std::snprintf(m_summaryBuffer.data(),
                      m_summaryBuffer.size(),
                      "%s",
                      entry->description.data());
        m_summary->setText("%s", m_summaryBuffer.data());
    }

    std::vector<const GuidedRecipe *> m_entries;
    TListBox *m_list = nullptr;
    TParamText *m_summary = nullptr;
    std::array<char, 160> m_summaryBuffer{};
};

class SavedSearchDialog : public TDialog
{
public:
    SavedSearchDialog(std::vector<SavedSpecification> specs)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 70, 20), "Saved searches"),
          m_specs(std::move(specs))
    {
        options |= ofCentered;

        m_list = new TListBox(TRect(2, 2, 36, 14), 1, nullptr);
        insert(m_list);
        m_summary = new TParamText(TRect(38, 2, 68, 14));
        insert(m_summary);

        m_loadButton = new TButton(TRect(10, 15, 22, 17), "~L~oad", cmOK, bfDefault);
        insert(m_loadButton);
        m_deleteButton = new TButton(TRect(24, 15, 36, 17), "~D~elete", cmDeleteSavedSpecification, bfNormal);
        insert(m_deleteButton);
        insert(new TButton(TRect(38, 15, 50, 17), "Cancel", cmCancel, bfNormal));

        refreshList();
        updateSummary();
    }

    const SavedSpecification *selectedSpecification() const
    {
        int index = m_list ? m_list->focused : -1;
        if (index < 0 || index >= static_cast<int>(m_specs.size()))
            return nullptr;
        return &m_specs[static_cast<std::size_t>(index)];
    }

protected:
    void handleEvent(TEvent &event) override
    {
        if (event.what == evCommand && event.message.command == cmDeleteSavedSpecification)
        {
            deleteSelection();
            clearEvent(event);
            return;
        }

        TDialog::handleEvent(event);

        if (event.what == evBroadcast && event.message.command == cmListItemSelected)
            updateSummary();
        else
            updateButtons();
    }

private:
    void refreshList()
    {
        std::sort(m_specs.begin(), m_specs.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.name < rhs.name;
        });

        auto *collection = new TStringCollection(10, 5);
        for (const auto &spec : m_specs)
            collection->insert(newStr(spec.name.c_str()));
        m_list->newList(collection);
        if (!m_specs.empty())
            m_list->focusItem(0);
        updateButtons();
    }

    void updateSummary()
    {
        const SavedSpecification *entry = selectedSpecification();
        if (!entry)
        {
            m_summary->setText("%s", "No saved search selected.");
            return;
        }
        std::snprintf(m_summaryBuffer.data(),
                      m_summaryBuffer.size(),
                      "%s",
                      entry->path.string().c_str());
        m_summary->setText("%s", m_summaryBuffer.data());
    }

    void updateButtons()
    {
        bool hasSelection = (selectedSpecification() != nullptr);
        m_loadButton->setState(sfDisabled, hasSelection ? False : True);
        m_deleteButton->setState(sfDisabled, hasSelection ? False : True);
    }

    void deleteSelection()
    {
        const SavedSpecification *entry = selectedSpecification();
        if (!entry)
            return;
        char message[256];
        std::snprintf(message, sizeof(message), "Remove saved search \"%s\"?", entry->name.c_str());
        if (messageBox(message, mfConfirmation | mfYesButton | mfNoButton) != cmYes)
            return;
        if (!removeSpecification(entry->slug))
        {
            messageBox("Could not delete saved search.", mfError | mfOKButton);
            return;
        }
        int focused = m_list ? m_list->focused : -1;
        if (focused >= 0 && focused < static_cast<int>(m_specs.size()))
            m_specs.erase(m_specs.begin() + focused);
        refreshList();
        updateSummary();
    }

    std::vector<SavedSpecification> m_specs;
    TListBox *m_list = nullptr;
    TParamText *m_summary = nullptr;
    TButton *m_loadButton = nullptr;
    TButton *m_deleteButton = nullptr;
    std::array<char, 256> m_summaryBuffer{};
};

class SaveSearchDialog : public TDialog
{
public:
    explicit SaveSearchDialog(const char *initialName)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 50, 9), "Save search")
    {
        options |= ofCentered;

        m_nameInput = new TInputLine(TRect(4, 3, 46, 4), static_cast<int>(m_buffer.size() - 1));
        insert(m_nameInput);
        insert(new TLabel(lineRect(4, 2, 16), "Name:", m_nameInput));

        insert(new TButton(TRect(12, 5, 24, 7), "~S~ave", cmOK, bfDefault));
        insert(new TButton(TRect(26, 5, 38, 7), "Cancel", cmCancel, bfNormal));

        if (initialName && initialName[0] != '\0')
            std::snprintf(m_buffer.data(), m_buffer.size(), "%s", initialName);
        m_nameInput->setData(m_buffer.data());
    }

    std::string name() const
    {
        return std::string(m_buffer.data());
    }

    void collect()
    {
        if (m_nameInput)
            m_nameInput->getData(m_buffer.data());
    }

private:
    TInputLine *m_nameInput = nullptr;
    std::array<char, 128> m_buffer{};
};

const char *extensionsForPreset(GuidedTypePreset preset)
{
    switch (preset)
    {
    case GuidedTypePreset::Documents:
        return "pdf,doc,docx,txt,md,rtf";
    case GuidedTypePreset::Images:
        return "jpg,jpeg,png,gif,svg,webp,bmp";
    case GuidedTypePreset::Audio:
        return "mp3,wav,flac,aac,ogg";
    case GuidedTypePreset::Archives:
        return "zip,tar,tar.gz,tgz,rar,7z";
    case GuidedTypePreset::Code:
        return "c,cpp,h,hpp,py,js,ts,java,rb,rs,go,swift,cs";
    case GuidedTypePreset::All:
    case GuidedTypePreset::Custom:
    default:
        return "";
    }
}

class GuidedSearchDialog : public TDialog
{
public:
    GuidedSearchDialog(SearchSpecification &spec, GuidedSearchState &state);

protected:
    void handleEvent(TEvent &event) override;
    Boolean valid(ushort command) override;

private:
    void populateFromState();
    void collectIntoState();
    void updateDynamicControls();
    void updateTypeSummary();
    void updateDateControls();
    void updateSizeControls();
    void updateActionControls();
    void browseStartLocation();
    void applyStateToSpecification();
    void syncStateFromSpecification();
    void openAdvancedDialog(unsigned short command);
    void showPopularPresets();
    void showExpertRecipes();
    void loadSavedSearch();
    void saveCurrentSearch();
    void applyPreset(const GuidedSearchPreset &preset);
    void applyRecipe(const GuidedRecipe &recipe);
    void applySpecificationToDialog(const SearchSpecification &spec);

    SearchSpecification &m_spec;
    GuidedSearchState &m_state;

    TInputLine *m_specNameInput = nullptr;
    TInputLine *m_startInput = nullptr;
    TCheckBoxes *m_locationChecks = nullptr;
    TInputLine *m_searchTextInput = nullptr;
    TRadioButtons *m_scopeButtons = nullptr;
    TRadioButtons *m_textModeButtons = nullptr;
    TCheckBoxes *m_textFlagChecks = nullptr;
    TInputLine *m_includeInput = nullptr;
    TInputLine *m_excludeInput = nullptr;
    TRadioButtons *m_typePresetButtons = nullptr;
    TParamText *m_typeSummary = nullptr;
    TRadioButtons *m_datePresetButtons = nullptr;
    TInputLine *m_dateFromInput = nullptr;
    TInputLine *m_dateToInput = nullptr;
    TRadioButtons *m_sizePresetButtons = nullptr;
    TInputLine *m_sizePrimaryInput = nullptr;
    TInputLine *m_sizeSecondaryInput = nullptr;
    TCheckBoxes *m_filterAdvancedChecks = nullptr;
    TCheckBoxes *m_actionChecks = nullptr;
    TInputLine *m_commandInput = nullptr;

    std::array<char, 96> m_typeSummaryBuffer{};
};

GuidedSearchDialog::GuidedSearchDialog(SearchSpecification &spec, GuidedSearchState &state)
    : TWindowInit(&TDialog::initFrame),
      TDialog(TRect(0, 0, 84, 36), "Guided Search"),
      m_spec(spec),
      m_state(state)
{
    options |= ofCentered;

    m_specNameInput = new TInputLine(lineRect(18, 1, 60), static_cast<int>(m_state.specName.size() - 1));
    insert(m_specNameInput);
    insert(new TLabel(lineRect(2, 1, 18), "Search ~n~ame:", m_specNameInput));

    insert(new TButton(TRect(62, 1, 74, 3), "~P~resets…", cmShowPopularPresets, bfNormal));
    insert(new TButton(TRect(74, 1, 82, 3), "Reci~p~es…", cmShowExpertRecipes, bfNormal));
    insert(new TButton(TRect(62, 2, 74, 4), "Sa~v~ed…", cmDialogLoadSpec, bfNormal));
    insert(new TButton(TRect(74, 2, 82, 4), "Sa~v~e…", cmDialogSaveSpec, bfNormal));

    insert(new TStaticText(lineRect(2, 3, 18), "Location"));

    m_startInput = new TInputLine(lineRect(18, 4, 64), static_cast<int>(m_state.startLocation.size() - 1));
    insert(m_startInput);
    insert(new TLabel(lineRect(4, 4, 18), "Start ~i~n:", m_startInput));
    insert(new TButton(TRect(65, 4, 82, 6), "~B~rowse…", cmBrowseStart, bfNormal));

    m_locationChecks = new TCheckBoxes(TRect(4, 5, 44, 9),
                                       makeItemList({"Search sub~f~olders",
                                                     "Include hidden system files",
                                                     "Follow symbolic links",
                                                     "Stay on current filesystem"}));
    insert(m_locationChecks);

    insert(new TStaticText(lineRect(2, 9, 18), "What"));

    m_searchTextInput = new TInputLine(lineRect(18, 10, 82), static_cast<int>(m_state.searchText.size() - 1));
    insert(m_searchTextInput);
    insert(new TLabel(lineRect(4, 10, 18), "~L~ook for:", m_searchTextInput));

    m_scopeButtons = new TRadioButtons(TRect(4, 11, 34, 14),
                                       makeItemList({"Contents and names",
                                                     "Contents only",
                                                     "Names only"}));
    insert(m_scopeButtons);

    m_textModeButtons = new TRadioButtons(TRect(36, 11, 66, 14),
                                          makeItemList({"Contains text",
                                                        "Whole words",
                                                        "Regular expression"}));
    insert(m_textModeButtons);

    m_textFlagChecks = new TCheckBoxes(TRect(4, 14, 34, 17),
                                       makeItemList({"~M~atch case",
                                                     "Allow multiple terms",
                                                     "Treat binary as text"}));
    insert(m_textFlagChecks);

    m_includeInput = new TInputLine(lineRect(24, 17, 82), static_cast<int>(m_state.includePatterns.size() - 1));
    insert(m_includeInput);
    insert(new TLabel(lineRect(4, 17, 24), "~I~nclude patterns:", m_includeInput));

    m_excludeInput = new TInputLine(lineRect(24, 18, 82), static_cast<int>(m_state.excludePatterns.size() - 1));
    insert(m_excludeInput);
    insert(new TLabel(lineRect(4, 18, 24), "E~x~clude patterns:", m_excludeInput));

    insert(new TButton(TRect(4, 19, 32, 21), "Fine-tune ~t~ext…", cmTextOptions, bfNormal));
    insert(new TButton(TRect(33, 19, 60, 21), "Fine-tune ~n~ames…", cmNamePathOptions, bfNormal));

    insert(new TStaticText(lineRect(2, 21, 18), "Filters"));

    m_typePresetButtons = new TRadioButtons(TRect(4, 22, 24, 28),
                                            makeItemList({"All files",
                                                          "Documents",
                                                          "Images",
                                                          "Audio",
                                                          "Archives",
                                                          "Code",
                                                          "Custom"}));
    insert(m_typePresetButtons);

    m_typeSummary = new TParamText(lineRect(4, 28, 44));
    insert(m_typeSummary);

    m_datePresetButtons = new TRadioButtons(TRect(26, 22, 46, 29),
                                            makeItemList({"Any time",
                                                          "Last 24 hours",
                                                          "Last 7 days",
                                                          "Last 30 days",
                                                          "Last 6 months",
                                                          "Past year",
                                                          "Custom range"}));
    insert(m_datePresetButtons);

    m_dateFromInput = new TInputLine(TRect(34, 29, 48, 30), static_cast<int>(m_state.dateFrom.size() - 1));
    insert(m_dateFromInput);
    insert(new TLabel(lineRect(26, 29, 34), "From:", m_dateFromInput));

    m_dateToInput = new TInputLine(TRect(52, 29, 66, 30), static_cast<int>(m_state.dateTo.size() - 1));
    insert(m_dateToInput);
    insert(new TLabel(lineRect(48, 29, 52), "To:", m_dateToInput));

    m_sizePresetButtons = new TRadioButtons(TRect(48, 22, 82, 28),
                                            makeItemList({"Any size",
                                                          "Larger than…",
                                                          "Smaller than…",
                                                          "Between…",
                                                          "Exactly…",
                                                          "Empty only"}));
    insert(m_sizePresetButtons);

    m_sizePrimaryInput = new TInputLine(TRect(60, 30, 74, 31), static_cast<int>(m_state.sizePrimary.size() - 1));
    insert(m_sizePrimaryInput);
    insert(new TLabel(lineRect(48, 30, 60), "Value:", m_sizePrimaryInput));

    m_sizeSecondaryInput = new TInputLine(TRect(76, 30, 82, 31), static_cast<int>(m_state.sizeSecondary.size() - 1));
    insert(m_sizeSecondaryInput);
    insert(new TLabel(lineRect(74, 30, 76), "to", m_sizeSecondaryInput));

    m_filterAdvancedChecks = new TCheckBoxes(TRect(4, 30, 34, 32),
                                             makeItemList({"Permission checks",
                                                           "Traversal controls"}));
    insert(m_filterAdvancedChecks);
    insert(new TButton(TRect(36, 30, 56, 32), "Permissions…", cmPermissionOwnership, bfNormal));
    insert(new TButton(TRect(57, 30, 82, 32), "Traversal…", cmTraversalFilters, bfNormal));
    insert(new TButton(TRect(36, 31, 56, 33), "Fine-tune ~f~ile types…", cmTypeFilters, bfNormal));
    insert(new TButton(TRect(57, 31, 82, 33), "Fine-tune ~d~ates…", cmTimeFilters, bfNormal));
    insert(new TButton(TRect(36, 32, 56, 34), "Fine-tune si~z~e…", cmSizeFilters, bfNormal));

    insert(new TStaticText(lineRect(2, 32, 18), "Actions"));

    m_actionChecks = new TCheckBoxes(TRect(4, 32, 34, 36),
                                     makeItemList({"Preview matches",
                                                   "List matching paths",
                                                   "Delete matches",
                                                   "Run command"}));
    insert(m_actionChecks);

    m_commandInput = new TInputLine(lineRect(52, 32, 82), static_cast<int>(m_state.customCommand.size() - 1));
    insert(m_commandInput);
    insert(new TLabel(lineRect(36, 32, 52), "Command:", m_commandInput));

    insert(new TButton(TRect(36, 33, 56, 35), "Fine-tune ~a~ctions…", cmActionOptions, bfNormal));
    insert(new TButton(TRect(36, 34, 56, 36), "Preview ~c~ommand", cmTogglePreview, bfNormal));
    insert(new TButton(TRect(58, 34, 70, 36), "~S~earch", cmOK, bfDefault));
    insert(new TButton(TRect(71, 34, 82, 36), "Cancel", cmCancel, bfNormal));

    populateFromState();
    updateDynamicControls();
}

void GuidedSearchDialog::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmBrowseStart:
            browseStartLocation();
            clearEvent(event);
            return;
        case cmShowPopularPresets:
            showPopularPresets();
            clearEvent(event);
            return;
        case cmShowExpertRecipes:
            showExpertRecipes();
            clearEvent(event);
            return;
        case cmDialogLoadSpec:
            loadSavedSearch();
            clearEvent(event);
            return;
        case cmDialogSaveSpec:
            saveCurrentSearch();
            clearEvent(event);
            return;
        case cmTextOptions:
        case cmNamePathOptions:
        case cmTimeFilters:
        case cmSizeFilters:
        case cmTypeFilters:
        case cmPermissionOwnership:
        case cmTraversalFilters:
        case cmActionOptions:
            openAdvancedDialog(event.message.command);
            clearEvent(event);
            return;
        case cmTogglePreview:
            if (m_actionChecks)
            {
                unsigned short flags = 0;
                m_actionChecks->getData(&flags);
                flags ^= kActionPreviewBit;
                m_actionChecks->setData(&flags);
                collectIntoState();
                updateActionControls();
            }
            clearEvent(event);
            return;
        default:
            break;
        }
    }

    TDialog::handleEvent(event);
    updateDynamicControls();
}

Boolean GuidedSearchDialog::valid(ushort command)
{
    if (command == cmOK)
    {
        collectIntoState();
        applyStateToSpecification();
    }
    return TDialog::valid(command);
}

void GuidedSearchDialog::populateFromState()
{
    if (m_specNameInput)
        m_specNameInput->setData(m_state.specName.data());
    if (m_startInput)
        m_startInput->setData(m_state.startLocation.data());
    if (m_searchTextInput)
        m_searchTextInput->setData(m_state.searchText.data());
    if (m_includeInput)
        m_includeInput->setData(m_state.includePatterns.data());
    if (m_excludeInput)
        m_excludeInput->setData(m_state.excludePatterns.data());
    if (m_commandInput)
        m_commandInput->setData(m_state.customCommand.data());

    if (m_locationChecks)
    {
        unsigned short flags = 0;
        if (m_state.includeSubdirectories)
            flags |= kLocationSubfoldersBit;
        if (m_state.includeHidden)
            flags |= kLocationHiddenBit;
        if (m_state.followSymlinks)
            flags |= kLocationSymlinkBit;
        if (m_state.stayOnSameFilesystem)
            flags |= kLocationStayFsBit;
        m_locationChecks->setData(&flags);
    }

    if (m_scopeButtons)
    {
        unsigned short mode = 0;
        if (m_state.searchFileContents && m_state.searchFileNames)
            mode = 0;
        else if (m_state.searchFileContents)
            mode = 1;
        else
            mode = 2;
        m_scopeButtons->setData(&mode);
    }

    if (m_textModeButtons)
    {
        unsigned short mode = static_cast<unsigned short>(m_state.textMode);
        m_textModeButtons->setData(&mode);
    }

    if (m_textFlagChecks)
    {
        unsigned short flags = 0;
        if (m_state.textMatchCase)
            flags |= kTextFlagMatchCaseBit;
        if (m_state.textAllowMultipleTerms)
            flags |= kTextFlagAllowMultipleBit;
        if (m_state.textTreatBinaryAsText)
            flags |= kTextFlagTreatBinaryBit;
        m_textFlagChecks->setData(&flags);
    }

    if (m_typePresetButtons)
    {
        unsigned short preset = static_cast<unsigned short>(m_state.typePreset);
        m_typePresetButtons->setData(&preset);
    }

    if (m_datePresetButtons)
    {
        unsigned short preset = static_cast<unsigned short>(m_state.datePreset);
        m_datePresetButtons->setData(&preset);
    }
    if (m_dateFromInput)
        m_dateFromInput->setData(m_state.dateFrom.data());
    if (m_dateToInput)
        m_dateToInput->setData(m_state.dateTo.data());

    if (m_sizePresetButtons)
    {
        unsigned short preset = static_cast<unsigned short>(m_state.sizePreset);
        m_sizePresetButtons->setData(&preset);
    }
    if (m_sizePrimaryInput)
        m_sizePrimaryInput->setData(m_state.sizePrimary.data());
    if (m_sizeSecondaryInput)
        m_sizeSecondaryInput->setData(m_state.sizeSecondary.data());

    if (m_filterAdvancedChecks)
    {
        unsigned short flags = 0;
        if (m_state.includePermissionAudit)
            flags |= kFilterAdvancedPermBit;
        if (m_state.includeTraversalFineTune)
            flags |= kFilterAdvancedTraversalBit;
        m_filterAdvancedChecks->setData(&flags);
    }

    if (m_actionChecks)
    {
        unsigned short flags = 0;
        if (m_state.previewResults)
            flags |= kActionPreviewBit;
        if (m_state.listMatches)
            flags |= kActionListBit;
        if (m_state.deleteMatches)
            flags |= kActionDeleteBit;
        if (m_state.runCommand)
            flags |= kActionCommandBit;
        m_actionChecks->setData(&flags);
    }

    updateTypeSummary();
}

void GuidedSearchDialog::collectIntoState()
{
    if (m_specNameInput)
        m_specNameInput->getData(m_state.specName.data());
    if (m_startInput)
        m_startInput->getData(m_state.startLocation.data());
    if (m_searchTextInput)
        m_searchTextInput->getData(m_state.searchText.data());
    if (m_includeInput)
        m_includeInput->getData(m_state.includePatterns.data());
    if (m_excludeInput)
        m_excludeInput->getData(m_state.excludePatterns.data());
    if (m_commandInput)
        m_commandInput->getData(m_state.customCommand.data());

    if (m_locationChecks)
    {
        unsigned short flags = 0;
        m_locationChecks->getData(&flags);
        m_state.includeSubdirectories = (flags & kLocationSubfoldersBit) != 0;
        m_state.includeHidden = (flags & kLocationHiddenBit) != 0;
        m_state.followSymlinks = (flags & kLocationSymlinkBit) != 0;
        m_state.stayOnSameFilesystem = (flags & kLocationStayFsBit) != 0;
    }

    if (m_scopeButtons)
    {
        unsigned short mode = 0;
        m_scopeButtons->getData(&mode);
        if (mode == 0)
        {
            m_state.searchFileContents = true;
            m_state.searchFileNames = true;
        }
        else if (mode == 1)
        {
            m_state.searchFileContents = true;
            m_state.searchFileNames = false;
        }
        else
        {
            m_state.searchFileContents = false;
            m_state.searchFileNames = true;
        }
    }

    if (m_textModeButtons)
    {
        unsigned short mode = 0;
        m_textModeButtons->getData(&mode);
        m_state.textMode = static_cast<TextSearchOptions::Mode>(mode);
    }

    if (m_textFlagChecks)
    {
        unsigned short flags = 0;
        m_textFlagChecks->getData(&flags);
        m_state.textMatchCase = (flags & kTextFlagMatchCaseBit) != 0;
        m_state.textAllowMultipleTerms = (flags & kTextFlagAllowMultipleBit) != 0;
        m_state.textTreatBinaryAsText = (flags & kTextFlagTreatBinaryBit) != 0;
    }

    if (m_typePresetButtons)
    {
        unsigned short preset = 0;
        m_typePresetButtons->getData(&preset);
        m_state.typePreset = static_cast<GuidedTypePreset>(preset);
        if (m_state.typePreset != GuidedTypePreset::Custom)
        {
            std::snprintf(m_state.typeCustomExtensions.data(), m_state.typeCustomExtensions.size(), "%s", extensionsForPreset(m_state.typePreset));
            m_state.typeCustomDetectors[0] = '\0';
        }
    }

    if (m_datePresetButtons)
    {
        unsigned short preset = 0;
        m_datePresetButtons->getData(&preset);
        m_state.datePreset = static_cast<GuidedDatePreset>(preset);
    }
    if (m_dateFromInput)
        m_dateFromInput->getData(m_state.dateFrom.data());
    if (m_dateToInput)
        m_dateToInput->getData(m_state.dateTo.data());

    if (m_sizePresetButtons)
    {
        unsigned short preset = 0;
        m_sizePresetButtons->getData(&preset);
        m_state.sizePreset = static_cast<GuidedSizePreset>(preset);
    }
    if (m_sizePrimaryInput)
        m_sizePrimaryInput->getData(m_state.sizePrimary.data());
    if (m_sizeSecondaryInput)
        m_sizeSecondaryInput->getData(m_state.sizeSecondary.data());

    if (m_filterAdvancedChecks)
    {
        unsigned short flags = 0;
        m_filterAdvancedChecks->getData(&flags);
        m_state.includePermissionAudit = (flags & kFilterAdvancedPermBit) != 0;
        m_state.includeTraversalFineTune = (flags & kFilterAdvancedTraversalBit) != 0;
    }

    if (m_actionChecks)
    {
        unsigned short flags = 0;
        m_actionChecks->getData(&flags);
        m_state.previewResults = (flags & kActionPreviewBit) != 0;
        m_state.listMatches = (flags & kActionListBit) != 0;
        m_state.deleteMatches = (flags & kActionDeleteBit) != 0;
        m_state.runCommand = (flags & kActionCommandBit) != 0;
    }
}

void GuidedSearchDialog::updateDynamicControls()
{
    updateTypeSummary();
    updateDateControls();
    updateSizeControls();
    updateActionControls();
}

void GuidedSearchDialog::updateTypeSummary()
{
    if (!m_typeSummary || !m_typePresetButtons)
        return;
    unsigned short preset = 0;
    m_typePresetButtons->getData(&preset);
    auto choice = static_cast<GuidedTypePreset>(preset);
    const char *text = nullptr;
    if (choice == GuidedTypePreset::Custom)
    {
        if (m_state.typeCustomExtensions[0] != '\0')
        {
            std::snprintf(m_typeSummaryBuffer.data(),
                          m_typeSummaryBuffer.size(),
                          "Custom: %s",
                          m_state.typeCustomExtensions.data());
        }
        else
        {
            std::snprintf(m_typeSummaryBuffer.data(),
                          m_typeSummaryBuffer.size(),
                          "Custom: configure extensions via Fine-tune file types…");
        }
        text = m_typeSummaryBuffer.data();
    }
    else
    {
        const char *raw = extensionsForPreset(choice);
        if (!raw || raw[0] == '\0')
        {
            std::snprintf(m_typeSummaryBuffer.data(),
                          m_typeSummaryBuffer.size(),
                          "All file types");
        }
        else
        {
            std::string friendly;
            for (char ch : std::string_view(raw))
            {
                if (ch == ',')
                    friendly.append(", ");
                else
                    friendly.push_back(ch);
            }
            std::snprintf(m_typeSummaryBuffer.data(),
                          m_typeSummaryBuffer.size(),
                          "Includes: %s",
                          friendly.c_str());
        }
        text = m_typeSummaryBuffer.data();
    }
    m_typeSummary->setText("%s", text ? text : "");
}

void GuidedSearchDialog::updateDateControls()
{
    if (!m_datePresetButtons)
        return;
    unsigned short preset = 0;
    m_datePresetButtons->getData(&preset);
    const bool custom = static_cast<GuidedDatePreset>(preset) == GuidedDatePreset::CustomRange;
    if (m_dateFromInput)
        m_dateFromInput->setState(sfDisabled, custom ? False : True);
    if (m_dateToInput)
        m_dateToInput->setState(sfDisabled, custom ? False : True);
}

void GuidedSearchDialog::updateSizeControls()
{
    if (!m_sizePresetButtons)
        return;
    unsigned short preset = 0;
    m_sizePresetButtons->getData(&preset);
    const auto choice = static_cast<GuidedSizePreset>(preset);
    const bool needsPrimary = choice != GuidedSizePreset::AnySize && choice != GuidedSizePreset::EmptyOnly;
    const bool needsSecondary = choice == GuidedSizePreset::Between;
    if (m_sizePrimaryInput)
        m_sizePrimaryInput->setState(sfDisabled, needsPrimary ? False : True);
    if (m_sizeSecondaryInput)
        m_sizeSecondaryInput->setState(sfDisabled, needsSecondary ? False : True);
}

void GuidedSearchDialog::updateActionControls()
{
    if (!m_actionChecks)
        return;
    unsigned short flags = 0;
    m_actionChecks->getData(&flags);
    const bool runCommand = (flags & kActionCommandBit) != 0;
    if (m_commandInput)
        m_commandInput->setState(sfDisabled, runCommand ? False : True);
}

void GuidedSearchDialog::browseStartLocation()
{
    char location[PATH_MAX]{};
    std::snprintf(location, sizeof(location), "%s", m_state.startLocation[0] ? m_state.startLocation.data() : ".");

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
    std::snprintf(m_state.startLocation.data(), m_state.startLocation.size(), "%s", newDir.c_str());
    if (m_startInput)
        m_startInput->setData(m_state.startLocation.data());
}

void GuidedSearchDialog::applyStateToSpecification()
{
    applyGuidedStateToSpecification(m_state, m_spec);
}

void GuidedSearchDialog::syncStateFromSpecification()
{
    m_state = guidedStateFromSpecification(m_spec);
    populateFromState();
    updateDynamicControls();
}

void GuidedSearchDialog::openAdvancedDialog(unsigned short command)
{
    collectIntoState();
    applyGuidedStateToSpecification(m_state, m_spec);

    bool accepted = false;
    switch (command)
    {
    case cmTextOptions:
        accepted = editTextOptions(m_spec.textOptions);
        break;
    case cmNamePathOptions:
        accepted = editNamePathOptions(m_spec.namePathOptions);
        break;
    case cmTimeFilters:
        accepted = editTimeFilters(m_spec.timeOptions);
        break;
    case cmSizeFilters:
        accepted = editSizeFilters(m_spec.sizeOptions);
        break;
    case cmTypeFilters:
        accepted = editTypeFilters(m_spec.typeOptions);
        break;
    case cmPermissionOwnership:
        accepted = editPermissionOwnership(m_spec.permissionOptions);
        break;
    case cmTraversalFilters:
        accepted = editTraversalFilters(m_spec.traversalOptions);
        break;
    case cmActionOptions:
        accepted = editActionOptions(m_spec.actionOptions);
        break;
    default:
        break;
    }

    if (accepted)
        syncStateFromSpecification();
    else
        applyGuidedStateToSpecification(m_state, m_spec);
}

void GuidedSearchDialog::showPopularPresets()
{
    auto presets = popularSearchPresets();
    if (presets.empty())
    {
        messageBox("No popular presets defined.", mfInformation | mfOKButton);
        return;
    }

    auto *dialog = new PresetPickerDialog("Popular searches", presets);
    unsigned short result = TProgram::application->executeDialog(dialog);
    const GuidedSearchPreset *selected = nullptr;
    if (result == cmOK)
        selected = dialog->selectedPreset();
    TObject::destroy(dialog);
    if (result != cmOK || !selected)
        return;
    collectIntoState();
    applyPreset(*selected);
}

void GuidedSearchDialog::showExpertRecipes()
{
    auto recipes = expertSearchRecipes();
    if (recipes.empty())
    {
        messageBox("No expert recipes available yet.", mfInformation | mfOKButton);
        return;
    }

    auto *dialog = new RecipePickerDialog("Expert recipes", recipes);
    unsigned short result = TProgram::application->executeDialog(dialog);
    const GuidedRecipe *selected = nullptr;
    if (result == cmOK)
        selected = dialog->selectedRecipe();
    TObject::destroy(dialog);
    if (result != cmOK || !selected)
        return;
    collectIntoState();
    applyRecipe(*selected);
}

void GuidedSearchDialog::loadSavedSearch()
{
    auto specs = listSavedSpecifications();
    if (specs.empty())
    {
        messageBox("No saved searches yet.", mfInformation | mfOKButton);
        return;
    }

    auto *dialog = new SavedSearchDialog(std::move(specs));
    unsigned short result = TProgram::application->executeDialog(dialog);
    const SavedSpecification *selected = nullptr;
    if (result == cmOK)
        selected = dialog->selectedSpecification();
    TObject::destroy(dialog);
    if (result != cmOK || !selected)
        return;
    auto loaded = loadSpecification(selected->slug);
    if (!loaded)
    {
        messageBox("Failed to load the saved search.", mfError | mfOKButton);
        return;
    }
    applySpecificationToDialog(*loaded);
}

void GuidedSearchDialog::saveCurrentSearch()
{
    collectIntoState();
    applyStateToSpecification();

    std::string currentName = bufferToString(m_spec.specName);
    auto *dialog = new SaveSearchDialog(currentName.c_str());
    unsigned short result = TProgram::application->executeDialog(dialog);
    std::string name;
    if (result == cmOK)
    {
        dialog->collect();
        name = dialog->name();
    }
    TObject::destroy(dialog);
    if (result != cmOK)
        return;
    if (name.empty())
    {
        messageBox("Please enter a name for the saved search.", mfError | mfOKButton);
        return;
    }

    std::snprintf(m_state.specName.data(), m_state.specName.size(), "%s", name.c_str());
    applyStateToSpecification();

    if (!saveSpecification(m_spec, name))
    {
        messageBox("Could not save the search specification.", mfError | mfOKButton);
        return;
    }

    messageBox("Search saved for quick access.", mfInformation | mfOKButton);
    syncStateFromSpecification();
}

void GuidedSearchDialog::applyPreset(const GuidedSearchPreset &preset)
{
    if (preset.apply)
        preset.apply(m_state);
    if (m_state.specName[0] == '\0')
        std::snprintf(m_state.specName.data(), m_state.specName.size(), "%s", preset.title.data());
    applyStateToSpecification();
    syncStateFromSpecification();
}

void GuidedSearchDialog::applyRecipe(const GuidedRecipe &recipe)
{
    if (recipe.apply)
        recipe.apply(m_state);
    std::snprintf(m_state.specName.data(), m_state.specName.size(), "%s", recipe.title.data());
    applyStateToSpecification();

    if (recipe.id == "owned-root")
    {
        m_spec.enablePermissionOwnership = true;
        PermissionOwnershipOptions &perm = m_spec.permissionOptions;
        perm.permEnabled = true;
        perm.permMode = PermissionOwnershipOptions::PermMode::AllBits;
        perm.readable = false;
        perm.writable = false;
        perm.executable = false;
        copyToArray(perm.permSpec, "0020");
        perm.userEnabled = true;
        perm.uidEnabled = false;
        perm.groupEnabled = false;
        perm.gidEnabled = false;
        perm.noUser = false;
        perm.noGroup = false;
        copyToArray(perm.user, "root");
        perm.uid[0] = '\0';
        perm.group[0] = '\0';
        perm.gid[0] = '\0';
    }
    else if (recipe.id == "new-symlinks")
    {
        m_spec.enableTypeFilters = true;
        TypeFilterOptions &type = m_spec.typeOptions;
        type.typeEnabled = true;
        type.useExtensions = false;
        type.useDetectors = false;
        type.typeLetters[0] = 'l';
        type.typeLetters[1] = '\0';
        m_spec.traversalOptions.symlinkMode = TraversalFilesystemOptions::SymlinkMode::Everywhere;
    }

    syncStateFromSpecification();
}

void GuidedSearchDialog::applySpecificationToDialog(const SearchSpecification &spec)
{
    m_spec = spec;
    m_state = guidedStateFromSpecification(m_spec);
    populateFromState();
    updateDynamicControls();
}

} // namespace

bool configureSearchSpecification(SearchSpecification &spec)
{
    GuidedSearchState state = guidedStateFromSpecification(spec);
    auto *dialog = new GuidedSearchDialog(spec, state);
    unsigned short result = TProgram::application->executeDialog(dialog);
    bool accepted = (result == cmOK);
    if (accepted)
        applyGuidedStateToSpecification(state, spec);
    return accepted;
}

} // namespace ck::find
