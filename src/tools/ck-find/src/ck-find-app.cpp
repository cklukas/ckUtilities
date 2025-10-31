#include "ck/about_dialog.hpp"
#include "ck/app_info.hpp"
#include "ck/find/cli_buffer_utils.hpp"
#include "ck/find/search_backend.hpp"
#include "ck/find/search_dialogs.hpp"
#include "ck/find/search_model.hpp"
#include "ck/hotkeys.hpp"
#include "ck/launcher.hpp"
#include "ck/ui/clock_aware_application.hpp"
#include "ck/ui/clock_view.hpp"
#include "ck/ui/status_line.hpp"

#include "command_ids.hpp"

#define Uses_MsgBox
#define Uses_TApplication
#define Uses_TDeskTop
#define Uses_TKeys
#define Uses_TInputLine
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TMessageBox
#define Uses_TProgram
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#include <tvision/tv.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

constexpr std::string_view kToolId = "ck-find";

const ck::appinfo::ToolInfo &toolInfo()
{
    return ck::appinfo::requireTool(kToolId);
}

using ck::find::ActionOptions;
using ck::find::SavedSpecification;
using ck::find::SearchExecutionOptions;
using ck::find::SearchSpecification;
using ck::find::TraversalFilesystemOptions;
using ck::find::copyToArray;
using ck::find::bufferToString;
using ck::find::configureSearchSpecification;
using ck::find::executeSpecification;
using ck::find::listSavedSpecifications;
using ck::find::loadSpecification;
using ck::find::makeDefaultSpecification;
using ck::find::normaliseSpecificationName;
using ck::find::saveSpecification;

class FindStatusLine : public ck::ui::CommandAwareStatusLine
{
public:
    FindStatusLine(TRect r)
        : ck::ui::CommandAwareStatusLine(r, *new TStatusDef(0, 0xFFFF, nullptr))
    {
        rebuild();
    }

private:
    void rebuild()
    {
        disposeItems(items);
        auto *newItem = new TStatusItem("New Search", kbNoKey, cmNewSearch);
        ck::hotkeys::configureStatusItem(*newItem, "New Search");
        auto *loadItem = new TStatusItem("Load Spec", kbNoKey, cmLoadSpec);
        ck::hotkeys::configureStatusItem(*loadItem, "Load Spec");
        auto *saveItem = new TStatusItem("Save Spec", kbNoKey, cmSaveSpec);
        ck::hotkeys::configureStatusItem(*saveItem, "Save Spec");
        newItem->next = loadItem;
        loadItem->next = saveItem;
        TStatusItem *tail = saveItem;
        auto addTabItem = [&](const char *title, unsigned short command) {
            auto *item = new TStatusItem(title, kbNoKey, command);
            ck::hotkeys::configureStatusItem(*item, title);
            tail->next = item;
            tail = item;
        };
        addTabItem("Quick Tab", cmTabQuickStart);
        addTabItem("Content Tab", cmTabContentNames);
        addTabItem("Dates Tab", cmTabDatesSizes);
        addTabItem("Types Tab", cmTabTypesOwnership);
        addTabItem("Traversal Tab", cmTabTraversal);
        addTabItem("Actions Tab", cmTabActions);
        auto *previewItem = new TStatusItem("Preview", kbNoKey, cmTogglePreview);
        ck::hotkeys::configureStatusItem(*previewItem, "Preview");
        tail->next = previewItem;
        tail = previewItem;
        if (ck::launcher::launchedFromCkLauncher())
        {
            auto *returnItem = new TStatusItem("Return", kbNoKey, cmReturnToLauncher);
            ck::hotkeys::configureStatusItem(*returnItem, "Return");
            tail->next = returnItem;
            tail = returnItem;
        }
        auto *quitItem = new TStatusItem("Quit", kbNoKey, cmQuit);
        ck::hotkeys::configureStatusItem(*quitItem, "Quit");
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
        const ActionOptions &actionsOpt = spec.actionOptions;
        if (actionsOpt.print)
            actions.emplace_back("-print");
        if (actionsOpt.print0)
            actions.emplace_back("-print0");
        if (actionsOpt.ls)
            actions.emplace_back("-ls");
        if (actionsOpt.deleteMatches)
            actions.emplace_back("-delete");
        if (actionsOpt.quitEarly)
            actions.emplace_back("-quit");
        if (actionsOpt.execEnabled)
        {
            switch (actionsOpt.execVariant)
            {
            case ActionOptions::ExecVariant::Exec:
                actions.emplace_back(actionsOpt.execUsePlus ? "-exec ... +" : "-exec ... ;");
                break;
            case ActionOptions::ExecVariant::ExecDir:
                actions.emplace_back(actionsOpt.execUsePlus ? "-execdir ... +" : "-execdir ... ;");
                break;
            case ActionOptions::ExecVariant::Ok:
                actions.emplace_back(actionsOpt.execUsePlus ? "-ok ... +" : "-ok ... ;");
                break;
            case ActionOptions::ExecVariant::OkDir:
                actions.emplace_back(actionsOpt.execUsePlus ? "-okdir ... +" : "-okdir ... ;");
                break;
            }
        }
        if (actionsOpt.fprintEnabled)
            actions.emplace_back("-fprint");
        if (actionsOpt.fprint0Enabled)
            actions.emplace_back("-fprint0");
        if (actionsOpt.flsEnabled)
            actions.emplace_back("-fls");
        if (actionsOpt.printfEnabled)
            actions.emplace_back("-printf");
        if (actionsOpt.fprintfEnabled)
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

class FindApp : public ck::ui::ClockAwareApplication
{
public:
    FindApp(int, char **)
        : TProgInit(&FindApp::initStatusLine, &FindApp::initMenuBar, &TApplication::initDeskTop),
          ck::ui::ClockAwareApplication()
    {
        insertMenuClock();

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
                loadSavedSpecification();
                break;
            case cmSaveSpec:
                saveCurrentSpecification();
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
                             *new TMenuItem("~N~ew Search...", cmNewSearch, kbNoKey, hcNoContext) +
                             *new TMenuItem("~L~oad Search Spec...", cmLoadSpec, kbNoKey, hcNoContext) +
                             *new TMenuItem("~S~ave Search Spec...", cmSaveSpec, kbNoKey, hcNoContext) +
                             newLine();
        if (ck::launcher::launchedFromCkLauncher())
            fileMenu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher, kbNoKey, hcNoContext);
        fileMenu + *new TMenuItem("E~x~it", cmQuit, kbNoKey, hcNoContext);

        TMenuItem &menuChain = fileMenu +
                               *new TSubMenu("~H~elp", hcNoContext) +
                                   *new TMenuItem("~A~bout", cmAbout, kbNoKey, hcNoContext);

        ck::hotkeys::configureMenuTree(menuChain);
        return new TMenuBar(r, static_cast<TSubMenu &>(menuChain));
    }

    static TStatusLine *initStatusLine(TRect r)
    {
        r.a.y = r.b.y - 1;
        return new FindStatusLine(r);
    }

private:
    SearchSpecification m_spec{};

    void newSearch();
    void saveCurrentSpecification();
    void loadSavedSpecification();
};

void FindApp::newSearch()
{
    SearchSpecification candidate = m_spec;
    if (configureSearchSpecification(candidate))
    {
        m_spec = candidate;
        std::string summary = buildPlaceholderSummary(m_spec);
        messageBox(summary.c_str(), mfInformation | mfOKButton);
    }
}

void FindApp::saveCurrentSpecification()
{
    char buffer[128];
    std::string currentName = normaliseSpecificationName(bufferToString(m_spec.specName));
    if (currentName.empty())
        currentName = "Unnamed";
    std::snprintf(buffer, sizeof(buffer), "%s", currentName.c_str());
    if (inputBox("Save Search", "Specification name:", buffer, sizeof(buffer) - 1) == cmCancel)
        return;

    std::string desiredName = normaliseSpecificationName(buffer);
    if (desiredName.empty())
    {
        messageBox("Name cannot be empty.", mfError | mfOKButton);
        return;
    }

    SearchSpecification specToSave = m_spec;
    copyToArray(specToSave.specName, desiredName.c_str());
    if (saveSpecification(specToSave, desiredName))
    {
        copyToArray(m_spec.specName, desiredName.c_str());
        std::string message = "Saved search specification '" + desiredName + "'.";
        messageBox(message.c_str(), mfInformation | mfOKButton);
    }
    else
    {
        messageBox("Failed to save search specification.", mfError | mfOKButton);
    }
}

void FindApp::loadSavedSpecification()
{
    auto specs = listSavedSpecifications();
    if (specs.empty())
    {
        messageBox("No saved search specifications found.", mfInformation | mfOKButton);
        return;
    }

    std::ostringstream list;
    list << "Saved searches:" << '\n';
    for (const auto &spec : specs)
        list << "  - " << spec.name << '\n';
    std::string listText = list.str();
    messageBox(listText.c_str(), mfInformation | mfOKButton);

    char buffer[128] = "";
    std::snprintf(buffer, sizeof(buffer), "%s", specs.front().name.c_str());
    if (inputBox("Load Search", "Specification name:", buffer, sizeof(buffer) - 1) == cmCancel)
        return;

    std::string desiredName = normaliseSpecificationName(buffer);
    if (desiredName.empty())
    {
        messageBox("Name cannot be empty.", mfError | mfOKButton);
        return;
    }

    if (auto loaded = loadSpecification(desiredName))
    {
        m_spec = *loaded;
        std::string message = "Loaded search specification '" + desiredName + "'.";
        messageBox(message.c_str(), mfInformation | mfOKButton);
    }
    else
    {
        std::string message = "No saved specification named '" + desiredName + "'.";
        messageBox(message.c_str(), mfError | mfOKButton);
    }
}

} // namespace

int main(int argc, char **argv)
{
    ck::hotkeys::registerDefaultSchemes();
    ck::hotkeys::initializeFromEnvironment();
    ck::hotkeys::applyCommandLineScheme(argc, argv);

    bool listSpecsOnly = false;
    std::optional<std::string> searchName;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--search")
        {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--search requires a specification name.\n");
                return EXIT_FAILURE;
            }
            searchName = std::string(argv[++i]);
        }
        else if (arg == "--list-specs")
        {
            listSpecsOnly = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            const char *binaryName = (argc > 0 && argv[0]) ? argv[0] : "ck-find";
            std::printf("Usage: %s [--search NAME] [--list-specs] [--hotkeys SCHEME]\n", binaryName);
            return 0;
        }
    }

    if (listSpecsOnly)
    {
        auto specs = listSavedSpecifications();
        for (const auto &spec : specs)
            std::cout << spec.name << std::endl;
        return 0;
    }

    if (searchName)
    {
        auto loaded = loadSpecification(normaliseSpecificationName(*searchName));
        if (!loaded)
        {
            std::fprintf(stderr, "No saved specification named '%s'.\n", searchName->c_str());
            return EXIT_FAILURE;
        }

        SearchExecutionOptions options;
        options.includeActions = false;
        options.captureMatches = true;
        options.filterContent = true;
        auto result = executeSpecification(*loaded, options, &std::cout, &std::cerr);
        return result.exitCode;
    }

    FindApp app(argc, argv);
    app.run();
    return 0;
}
