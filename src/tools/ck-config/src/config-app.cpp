#include "ck/options.hpp"
#include "ck/about_dialog.hpp"
#include "ck/app_info.hpp"
#include "ck/commands/ck_config.hpp"
#include "ck/hotkeys.hpp"
#include "ck/launcher.hpp"
#include "disk_usage_options.hpp"

#define Uses_TApplication
#define Uses_TButton
#define Uses_TDeskTop
#define Uses_TDialog
#define Uses_TFileDialog
#define Uses_TInputLine
#define Uses_TKeys
#define Uses_TLabel
#define Uses_TListViewer
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TMessageBox
#define Uses_TScrollBar
#define Uses_TPalette
#define Uses_TStaticText
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#define Uses_TWindow
#define Uses_MsgBox
#include <tvision/tv.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

namespace config = ck::config;

namespace
{

constexpr std::string_view kToolId = "ck-config";

static constexpr ushort cmEditHotkeys = 3400;
static constexpr ushort cmHotkeyEditCommand = 3401;
static constexpr ushort cmHotkeyClearCommand = 3402;

const ck::appinfo::ToolInfo &toolInfo()
{
    return ck::appinfo::requireTool(kToolId);
}

using RegisterFn = void (*)(config::OptionRegistry &);

struct ApplicationInfo
{
    std::string id;
    std::string name;
    RegisterFn registerFn = nullptr;
};

struct ApplicationEntry
{
    ApplicationInfo info;
    bool known = false;
    bool hasDefaults = false;
};

enum class CliAction
{
    None,
    ListApps,
    ListProfiles,
    Show,
    Clear,
    Reset,
    Export,
    Import,
    Set,
    ConfigRoot
};

struct CliOptions
{
    CliAction action = CliAction::None;
    std::string appId;
    std::string key;
    std::string value;
    std::filesystem::path file;
};

const std::vector<ApplicationInfo> &knownApplications()
{
    static std::vector<ApplicationInfo> apps;
    static bool initialized = false;
    if (!initialized)
    {
        auto toolSpan = ck::appinfo::tools();
        apps.reserve(toolSpan.size());
        for (const auto &tool : toolSpan)
        {
            RegisterFn reg = nullptr;
            if (tool.id == "ck-du")
                reg = &ck::du::registerDiskUsageOptions;
            apps.push_back(ApplicationInfo{std::string(tool.id), std::string(tool.displayName), reg});
        }
        initialized = true;
    }
    return apps;
}

const ApplicationInfo *findKnownApplication(const std::string &id)
{
    const auto &apps = knownApplications();
    auto it = std::find_if(apps.begin(), apps.end(), [&](const ApplicationInfo &info) { return info.id == id; });
    if (it != apps.end())
        return &*it;
    return nullptr;
}

std::string trim(const std::string &value)
{
    std::size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
        ++start;
    std::size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
        --end;
    return value.substr(start, end - start);
}

std::vector<std::string> splitList(const std::string &value)
{
    std::vector<std::string> result;
    std::string normalized = value;
    std::replace(normalized.begin(), normalized.end(), ';', ',');
    std::stringstream ss(normalized);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        std::string cleaned = trim(token);
        if (!cleaned.empty())
            result.push_back(cleaned);
    }
    return result;
}

std::string optionValueToString(const config::OptionDefinition &def, const config::OptionValue &value)
{
    switch (def.kind)
    {
    case config::OptionKind::Boolean:
        return value.toBool() ? "true" : "false";
    case config::OptionKind::Integer:
        return value.toString();
    case config::OptionKind::String:
        return value.toString();
    case config::OptionKind::StringList:
    {
        std::vector<std::string> list = value.toStringList();
        if (list.empty())
            return "[]";
        std::ostringstream out;
        out << "[";
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            if (i > 0)
                out << ", ";
            out << list[i];
        }
        out << "]";
        return out.str();
    }
    }
    return value.toString();
}

std::vector<ApplicationEntry> gatherApplicationEntries()
{
    std::vector<ApplicationEntry> entries;
    std::vector<std::string> profiles = config::OptionRegistry::availableProfiles();
    std::unordered_set<std::string> savedProfiles(profiles.begin(), profiles.end());
    for (const auto &info : knownApplications())
    {
        bool has = savedProfiles.find(info.id) != savedProfiles.end();
        entries.push_back({info, true, has});
    }
    std::sort(entries.begin(), entries.end(), [](const ApplicationEntry &a, const ApplicationEntry &b) {
        return a.info.id < b.info.id;
    });
    return entries;
}

void printUsage()
{
    const auto &info = toolInfo();
    std::cout << info.executable << " - " << info.shortDescription << "\n\n"
              << "Usage: " << info.executable << " [options]\n"
              << "  --list-apps             List known applications\n"
              << "  --list-profiles         List profiles with saved defaults\n"
              << "  --config-root           Print the configuration root path\n"
              << "  --show APP              Display saved defaults for APP\n"
              << "  --clear APP             Remove saved defaults for APP\n"
              << "  --reset APP             Reset APP to built-in defaults\n"
              << "  --export APP FILE       Export APP defaults to FILE\n"
              << "  --import APP FILE       Import defaults for APP from FILE\n"
              << "  --set APP KEY VALUE     Set KEY to VALUE for APP\n"
              << "  --hotkeys SCHEME       Use the specified hotkey scheme for this run\n"
              << "  --help                  Show this help message\n"
              << "\nAvailable schemes: linux, mac, windows, custom.\n"
              << "Set CK_HOTKEY_SCHEME to choose a default hotkey scheme." << std::endl;
}

int listApps()
{
    auto entries = gatherApplicationEntries();
    if (entries.empty())
        std::cout << "(no applications found)" << std::endl;
    for (const auto &entry : entries)
    {
        std::cout << entry.info.id << "\t" << entry.info.name;
        if (entry.hasDefaults)
            std::cout << "\t[saved]";
        std::cout << std::endl;
    }
    return 0;
}

int listProfiles()
{
    auto profiles = config::OptionRegistry::availableProfiles();
    if (profiles.empty())
        std::cout << "(no profiles found)" << std::endl;
    for (const auto &id : profiles)
        std::cout << id << std::endl;
    return 0;
}

int showApplication(const CliOptions &opts)
{
    const ApplicationInfo *info = findKnownApplication(opts.appId);
    config::OptionRegistry registry(opts.appId);
    if (info && info->registerFn)
    {
        info->registerFn(registry);
        if (!registry.loadDefaults())
        {
            std::cerr << "ck-config: no saved defaults for '" << opts.appId << "'" << std::endl;
            return 1;
        }
        std::cout << "Application: " << info->name << " (" << info->id << ")" << std::endl;
        auto defs = registry.listRegisteredOptions();
        for (const auto &def : defs)
        {
            config::OptionValue value = registry.get(def.key);
            std::cout << def.key << " = " << optionValueToString(def, value) << std::endl;
        }
        return 0;
    }
    std::filesystem::path path = registry.defaultOptionsPath();
    std::ifstream in(path);
    if (!in)
    {
        std::cerr << "ck-config: no saved defaults for '" << opts.appId << "' at " << path << std::endl;
        return 1;
    }
    std::cout << in.rdbuf();
    if (!in.good())
        return 1;
    return 0;
}

int clearApplication(const CliOptions &opts)
{
    config::OptionRegistry registry(opts.appId);
    std::filesystem::path path = registry.defaultOptionsPath();
    if (registry.clearDefaults())
    {
        std::cout << "Cleared defaults for '" << opts.appId << "' (" << path << ")" << std::endl;
        return 0;
    }
    std::cerr << "ck-config: failed to clear defaults at " << path << std::endl;
    return 1;
}

int resetApplication(const CliOptions &opts)
{
    const ApplicationInfo *info = findKnownApplication(opts.appId);
    if (!info || !info->registerFn)
    {
        std::cerr << "ck-config: application '" << opts.appId << "' does not support reset" << std::endl;
        return 1;
    }
    config::OptionRegistry registry(opts.appId);
    info->registerFn(registry);
    registry.resetToDefaults();
    if (!registry.saveDefaults())
    {
        std::cerr << "ck-config: failed to save defaults for '" << opts.appId << "'" << std::endl;
        return 1;
    }
    std::cout << "Defaults reset for '" << opts.appId << "' (" << registry.defaultOptionsPath() << ")" << std::endl;
    return 0;
}

int exportApplication(const CliOptions &opts)
{
    config::OptionRegistry registry(opts.appId);
    std::filesystem::path source = registry.defaultOptionsPath();
    std::error_code ec;
    if (!std::filesystem::exists(source, ec))
    {
        std::cerr << "ck-config: no saved defaults for '" << opts.appId << "'" << std::endl;
        return 1;
    }
    std::filesystem::create_directories(opts.file.parent_path(), ec);
    if (ec)
    {
        std::cerr << "ck-config: failed to prepare export directory: " << ec.message() << std::endl;
        return 1;
    }
    if (!std::filesystem::copy_file(source, opts.file, std::filesystem::copy_options::overwrite_existing, ec))
    {
        std::cerr << "ck-config: failed to export defaults: " << ec.message() << std::endl;
        return 1;
    }
    std::cout << "Exported defaults for '" << opts.appId << "' to " << opts.file << std::endl;
    return 0;
}

int importApplication(const CliOptions &opts)
{
    const ApplicationInfo *info = findKnownApplication(opts.appId);
    std::error_code ec;
    if (!std::filesystem::exists(opts.file, ec))
    {
        std::cerr << "ck-config: import file not found: " << opts.file << std::endl;
        return 1;
    }
    config::OptionRegistry registry(opts.appId);
    if (info && info->registerFn)
    {
        info->registerFn(registry);
        if (!registry.loadFromFile(opts.file))
        {
            std::cerr << "ck-config: failed to parse options from '" << opts.file << "'" << std::endl;
            return 1;
        }
        if (!registry.saveDefaults())
        {
            std::cerr << "ck-config: failed to save defaults for '" << opts.appId << "'" << std::endl;
            return 1;
        }
    }
    else
    {
        std::filesystem::path dest = registry.defaultOptionsPath();
        std::filesystem::create_directories(dest.parent_path(), ec);
        if (ec)
        {
            std::cerr << "ck-config: failed to prepare configuration directory: " << ec.message() << std::endl;
            return 1;
        }
        if (!std::filesystem::copy_file(opts.file, dest, std::filesystem::copy_options::overwrite_existing, ec))
        {
            std::cerr << "ck-config: failed to import defaults: " << ec.message() << std::endl;
            return 1;
        }
    }
    std::cout << "Imported defaults for '" << opts.appId << "'" << std::endl;
    return 0;
}

int setApplicationOption(const CliOptions &opts)
{
    const ApplicationInfo *info = findKnownApplication(opts.appId);
    if (!info || !info->registerFn)
    {
        std::cerr << "ck-config: application '" << opts.appId << "' does not support option editing" << std::endl;
        return 1;
    }
    config::OptionRegistry registry(opts.appId);
    info->registerFn(registry);
    const config::OptionDefinition *definition = registry.definition(opts.key);
    if (!definition)
    {
        std::cerr << "ck-config: unknown option '" << opts.key << "'" << std::endl;
        return 1;
    }
    switch (definition->kind)
    {
    case config::OptionKind::Boolean:
    case config::OptionKind::Integer:
    case config::OptionKind::String:
        registry.set(definition->key, config::OptionValue(opts.value));
        break;
    case config::OptionKind::StringList:
    {
        std::vector<std::string> list = splitList(opts.value);
        registry.set(definition->key, config::OptionValue(list));
        break;
    }
    }
    if (!registry.saveDefaults())
    {
        std::cerr << "ck-config: failed to save defaults for '" << opts.appId << "'" << std::endl;
        return 1;
    }
    config::OptionValue updated = registry.get(definition->key);
    std::cout << definition->key << " = " << optionValueToString(*definition, updated) << std::endl;
    return 0;
}

int executeCliAction(const CliOptions &opts)
{
    switch (opts.action)
    {
    case CliAction::ListApps:
        return listApps();
    case CliAction::ListProfiles:
        return listProfiles();
    case CliAction::Show:
        return showApplication(opts);
    case CliAction::Clear:
        return clearApplication(opts);
    case CliAction::Reset:
        return resetApplication(opts);
    case CliAction::Export:
        return exportApplication(opts);
    case CliAction::Import:
        return importApplication(opts);
    case CliAction::Set:
        return setApplicationOption(opts);
    case CliAction::ConfigRoot:
        std::cout << config::OptionRegistry::configRoot() << std::endl;
        return 0;
    case CliAction::None:
    default:
        break;
    }
    return -1;
}

int runCli(int argc, char **argv)
{
    CliOptions opts;
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return 0;
        }
        else if (arg == "--hotkeys")
        {
            if (i + 1 < argc)
                ++i;
            continue;
        }
        else if (arg.rfind("--hotkeys=", 0) == 0)
        {
            continue;
        }
        else if (arg == "--list-apps")
        {
            if (opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::ListApps;
        }
        else if (arg == "--list-profiles")
        {
            if (opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::ListProfiles;
        }
        else if (arg == "--config-root")
        {
            if (opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::ConfigRoot;
        }
        else if (arg == "--show")
        {
            if (i + 1 >= argc || opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::Show;
            opts.appId = argv[++i];
        }
        else if (arg == "--clear")
        {
            if (i + 1 >= argc || opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::Clear;
            opts.appId = argv[++i];
        }
        else if (arg == "--reset")
        {
            if (i + 1 >= argc || opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::Reset;
            opts.appId = argv[++i];
        }
        else if (arg == "--export")
        {
            if (i + 2 >= argc || opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::Export;
            opts.appId = argv[++i];
            opts.file = argv[++i];
        }
        else if (arg == "--import")
        {
            if (i + 2 >= argc || opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::Import;
            opts.appId = argv[++i];
            opts.file = argv[++i];
        }
        else if (arg == "--set")
        {
            if (i + 3 >= argc || opts.action != CliAction::None)
            {
                printUsage();
                return 1;
            }
            opts.action = CliAction::Set;
            opts.appId = argv[++i];
            opts.key = argv[++i];
            opts.value = argv[++i];
        }
        else
        {
            printUsage();
            return 1;
        }
    }
    if (opts.action == CliAction::None)
        return -1;
    return executeCliAction(opts);
}

// UI components

static constexpr ushort cmReloadApps = ck::commands::config::ReloadApps;
static constexpr ushort cmEditApp = ck::commands::config::EditApp;
static constexpr ushort cmResetApp = ck::commands::config::ResetApp;
static constexpr ushort cmClearApp = ck::commands::config::ClearApp;
static constexpr ushort cmExportApp = ck::commands::config::ExportApp;
static constexpr ushort cmImportApp = ck::commands::config::ImportApp;
static constexpr ushort cmOpenConfigDir = ck::commands::config::OpenConfigDir;
static constexpr ushort cmAbout = ck::commands::config::About;

static constexpr ushort cmOptionEdit = ck::commands::config::OptionEdit;
static constexpr ushort cmOptionResetValue = ck::commands::config::OptionResetValue;
static constexpr ushort cmOptionResetAll = ck::commands::config::OptionResetAll;
static constexpr ushort cmPatternAdd = ck::commands::config::PatternAdd;
static constexpr ushort cmPatternEdit = ck::commands::config::PatternEdit;
static constexpr ushort cmPatternDelete = ck::commands::config::PatternDelete;
static constexpr ushort cmReturnToLauncher = ck::commands::config::ReturnToLauncher;

class AppListViewer : public TListViewer
{
public:
    AppListViewer(const TRect &bounds, std::vector<ApplicationEntry> &entriesRef, TScrollBar *vScroll)
        : TListViewer(bounds, 1, nullptr, vScroll), entries(&entriesRef)
    {
        growMode = gfGrowHiX | gfGrowHiY;
        updateRange();
    }

    void updateRange()
    {
        if (!entries)
            return;
        setRange(static_cast<short>(entries->size()));
    }

    short currentIndex() const { return focused; }

    virtual void getText(char *dest, short item, short maxChars) override
    {
        if (!entries || item < 0 || item >= static_cast<short>(entries->size()))
        {
            if (maxChars > 0)
                dest[0] = '\0';
            return;
        }
        const ApplicationEntry &entry = (*entries)[static_cast<std::size_t>(item)];
        std::string label;
        if (!entry.info.name.empty())
            label = entry.info.name + " (" + entry.info.id + ")";
        else
            label = entry.info.id;
        std::snprintf(dest, static_cast<std::size_t>(maxChars), "%s", label.c_str());
    }

    virtual void handleEvent(TEvent &event) override
    {
        TListViewer::handleEvent(event);
        if (event.what == evKeyDown && event.keyDown.keyCode == kbEnter)
        {
            message(owner, evCommand, cmEditApp, this);
            clearEvent(event);
        }
    }

private:
    std::vector<ApplicationEntry> *entries;
};

class AppBrowserWindow : public TWindow
{
public:
    AppBrowserWindow()
        : TWindowInit(&TWindow::initFrame),
          TWindow(TRect(0, 0, 66, 20), "Applications", wnNoNumber)
    {
        flags |= wfGrow;
        growMode = gfGrowHiX | gfGrowHiY;
        TRect r = getExtent();
        r.grow(-1, -1);
        if (r.b.x <= r.a.x + 2 || r.b.y <= r.a.y + 2)
            r = TRect(0, 0, 64, 18);
        vScroll = new TScrollBar(TRect(r.b.x - 1, r.a.y, r.b.x, r.b.y));
        vScroll->growMode = gfGrowHiY;
        insert(vScroll);
        listView = new AppListViewer(TRect(r.a.x, r.a.y, r.b.x - 1, r.b.y), entries, vScroll);
        listView->growMode = gfGrowHiX | gfGrowHiY;
        insert(listView);
    }

    virtual TPalette &getPalette() const override
    {
        static TPalette palette(cpGrayDialog, sizeof(cpGrayDialog) - 1);
        return palette;
    }

    void setEntries(std::vector<ApplicationEntry> newEntries)
    {
        entries = std::move(newEntries);
        if (listView)
        {
            listView->updateRange();
            listView->drawView();
        }
        if (vScroll)
            vScroll->drawView();
    }

    std::optional<ApplicationEntry> selectedEntry() const
    {
        if (!listView)
            return std::nullopt;
        short index = listView->currentIndex();
        if (index < 0 || index >= static_cast<short>(entries.size()))
            return std::nullopt;
        return entries[static_cast<std::size_t>(index)];
    }

private:
    std::vector<ApplicationEntry> entries;
    AppListViewer *listView = nullptr;
    TScrollBar *vScroll = nullptr;
};

struct OptionItem
{
    config::OptionDefinition definition;
    config::OptionValue value;
};

class OptionListViewer : public TListViewer
{
public:
    OptionListViewer(const TRect &bounds, std::vector<OptionItem> &itemsRef, TScrollBar *vScroll)
        : TListViewer(bounds, 1, nullptr, vScroll), items(&itemsRef)
    {
        growMode = gfGrowHiX | gfGrowHiY;
        updateRange();
    }

    void updateRange()
    {
        if (!items)
            return;
        setRange(static_cast<short>(items->size()));
    }

    short currentIndex() const { return focused; }

    virtual void getText(char *dest, short item, short maxChars) override
    {
        if (!items || item < 0 || item >= static_cast<short>(items->size()))
        {
            if (maxChars > 0)
                dest[0] = '\0';
            return;
        }
        const OptionItem &opt = (*items)[static_cast<std::size_t>(item)];
        std::string text = opt.definition.displayName + " = " + optionValueToString(opt.definition, opt.value);
        std::snprintf(dest, static_cast<std::size_t>(maxChars), "%s", text.c_str());
    }

    virtual void handleEvent(TEvent &event) override
    {
        TListViewer::handleEvent(event);
        if (event.what == evKeyDown && event.keyDown.keyCode == kbEnter)
        {
            message(owner, evCommand, cmOptionEdit, this);
            clearEvent(event);
        }
    }

private:
    std::vector<OptionItem> *items;
};

class StringListViewer : public TListViewer
{
public:
    StringListViewer(const TRect &bounds, std::vector<std::string> &valuesRef, TScrollBar *vScroll)
        : TListViewer(bounds, 1, nullptr, vScroll), values(&valuesRef)
    {
        growMode = gfGrowHiX | gfGrowHiY;
        updateRange();
    }

    void updateRange()
    {
        if (!values)
            return;
        setRange(static_cast<short>(values->size()));
    }

    short currentIndex() const { return focused; }

    virtual void getText(char *dest, short item, short maxChars) override
    {
        if (!values || item < 0 || item >= static_cast<short>(values->size()))
        {
            if (maxChars > 0)
                dest[0] = '\0';
            return;
        }
        std::snprintf(dest, static_cast<std::size_t>(maxChars), "%s",
                      (*values)[static_cast<std::size_t>(item)].c_str());
    }

    virtual void handleEvent(TEvent &event) override
    {
        TListViewer::handleEvent(event);
        if (event.what == evKeyDown)
        {
            switch (event.keyDown.keyCode)
            {
            case kbEnter:
                message(owner, evCommand, cmPatternEdit, this);
                clearEvent(event);
                break;
            case kbIns:
                message(owner, evCommand, cmPatternAdd, this);
                clearEvent(event);
                break;
            case kbDel:
                message(owner, evCommand, cmPatternDelete, this);
                clearEvent(event);
                break;
            default:
                break;
            }
        }
    }

private:
    std::vector<std::string> *values;
};

class StringListDialog : public TDialog
{
public:
    StringListDialog(const std::string &title, const std::string &description, const std::vector<std::string> &initial)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 70, 20), title.c_str()),
          values(initial)
    {
        options |= ofCentered;
        insert(new TStaticText(TRect(2, 2, 68, 4), description.c_str()));
        vScroll = new TScrollBar(TRect(66, 4, 67, 15));
        vScroll->growMode = gfGrowHiY;
        insert(vScroll);
        listView = new StringListViewer(TRect(3, 4, 66, 15), values, vScroll);
        listView->growMode = gfGrowHiX | gfGrowHiY;
        insert(listView);
        insert(new TButton(TRect(3, 15, 15, 17), "~A~dd", cmPatternAdd, bfNormal));
        insert(new TButton(TRect(17, 15, 29, 17), "~E~dit", cmPatternEdit, bfNormal));
        insert(new TButton(TRect(31, 15, 43, 17), "~R~emove", cmPatternDelete, bfNormal));
        insert(new TButton(TRect(45, 15, 57, 17), "O~K~", cmOK, bfDefault));
        insert(new TButton(TRect(59, 15, 67, 17), "Cancel", cmCancel, bfNormal));
    }

    std::vector<std::string> result() const { return values; }

protected:
    virtual void handleEvent(TEvent &event) override
    {
        TDialog::handleEvent(event);
        if (event.what == evCommand)
        {
            switch (event.message.command)
            {
            case cmPatternAdd:
                addValue();
                break;
            case cmPatternEdit:
                editValue();
                break;
            case cmPatternDelete:
                removeValue();
                break;
            default:
                return;
            }
            clearEvent(event);
        }
    }

private:
    StringListViewer *listView = nullptr;
    TScrollBar *vScroll = nullptr;
    std::vector<std::string> values;

    void refresh()
    {
        if (listView)
        {
            listView->updateRange();
            listView->drawView();
        }
        if (vScroll)
            vScroll->drawView();
    }

    bool promptValue(const std::string &title, const std::string &initial, std::string &out)
    {
        struct Data
        {
            char buffer[256];
        } data{};
        std::snprintf(data.buffer, sizeof(data.buffer), "%s", initial.c_str());
        while (true)
        {
            auto *dialog = new TDialog(TRect(0, 0, 60, 12), title.c_str());
            dialog->options |= ofCentered;
            auto *input = new TInputLine(TRect(3, 5, 57, 6), sizeof(data.buffer) - 1);
            dialog->insert(new TLabel(TRect(2, 4, 12, 5), "~V~alue:", input));
            dialog->insert(input);
            dialog->insert(new TButton(TRect(14, 8, 24, 10), "O~K~", cmOK, bfDefault));
            dialog->insert(new TButton(TRect(26, 8, 36, 10), "Cancel", cmCancel, bfNormal));
            ushort code = TProgram::application->executeDialog(dialog, &data);
            if (code != cmOK)
                return false;
            std::string candidate = trim(data.buffer);
            if (candidate.empty())
            {
                messageBox("Value cannot be empty", mfError | mfOKButton);
                continue;
            }
            out = candidate;
            return true;
        }
    }

    void addValue()
    {
        std::string value;
        if (!promptValue("Add Value", std::string(), value))
            return;
        values.push_back(value);
        refresh();
    }

    void editValue()
    {
        if (!listView)
            return;
        short index = listView->currentIndex();
        if (index < 0 || index >= static_cast<short>(values.size()))
        {
            messageBox("Select a value to edit", mfInformation | mfOKButton);
            return;
        }
        std::string value;
        if (!promptValue("Edit Value", values[static_cast<std::size_t>(index)], value))
            return;
        values[static_cast<std::size_t>(index)] = value;
        refresh();
    }

    void removeValue()
    {
        if (!listView)
            return;
        short index = listView->currentIndex();
        if (index < 0 || index >= static_cast<short>(values.size()))
        {
            messageBox("Select a value to remove", mfInformation | mfOKButton);
            return;
        }
        std::string prompt = "Remove value?\n" + values[static_cast<std::size_t>(index)];
        if (messageBox(prompt.c_str(), mfYesNoCancel | mfConfirmation) != cmYes)
            return;
        values.erase(values.begin() + index);
        refresh();
    }
};

class OptionEditorDialog : public TDialog
{
public:
    OptionEditorDialog(const ApplicationInfo &info)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 81, 22), (info.name + " Options").c_str()),
          appInfo(info), registry(info.id)
    {
        options |= ofCentered;
        if (info.registerFn)
            info.registerFn(registry);
        registry.loadDefaults();

        insert(new TStaticText(TRect(2, 2, 79, 4),
                               "Edit options and press Save to persist as defaults."));

        vScroll = new TScrollBar(TRect(77, 4, 78, 17));
        vScroll->growMode = gfGrowHiY;
        insert(vScroll);

        listView = new OptionListViewer(TRect(3, 4, 77, 17), items, vScroll);
        listView->growMode = gfGrowHiX | gfGrowHiY;
        insert(listView);

        insert(new TButton(TRect(3, 18, 15, 20), "~E~dit", cmOptionEdit, bfNormal));
        insert(new TButton(TRect(17, 18, 33, 20), "~R~eset Value", cmOptionResetValue, bfNormal));
        insert(new TButton(TRect(35, 18, 51, 20), "Reset ~A~ll", cmOptionResetAll, bfNormal));
        insert(new TButton(TRect(53, 18, 65, 20), "~S~ave", cmOK, bfDefault));
        insert(new TButton(TRect(66, 18, 77, 20), "Cancel", cmCancel, bfNormal));

        refreshItems();
    }

protected:
    virtual void handleEvent(TEvent &event) override
    {
        if (event.what == evCommand)
        {
            switch (event.message.command)
            {
            case cmOptionEdit:
                editCurrent();
                clearEvent(event);
                return;
            case cmOptionResetValue:
                resetCurrent();
                clearEvent(event);
                return;
            case cmOptionResetAll:
                resetAll();
                clearEvent(event);
                return;
            case cmOK:
                if (!saveChanges())
                    clearEvent(event);
                break;
            default:
                break;
            }
        }
        TDialog::handleEvent(event);
    }

private:
    ApplicationInfo appInfo;
    config::OptionRegistry registry;
    std::vector<OptionItem> items;
    OptionListViewer *listView = nullptr;
    TScrollBar *vScroll = nullptr;

    void refreshItems()
    {
        items.clear();
        auto defs = registry.listRegisteredOptions();
        for (const auto &def : defs)
        {
            OptionItem item{def, registry.get(def.key)};
            items.push_back(item);
        }
        if (listView)
        {
            listView->updateRange();
            listView->drawView();
        }
        if (vScroll)
            vScroll->drawView();
    }

    OptionItem *currentItem()
    {
        if (!listView)
            return nullptr;
        short index = listView->currentIndex();
        if (index < 0 || index >= static_cast<short>(items.size()))
            return nullptr;
        return &items[static_cast<std::size_t>(index)];
    }

    void editCurrent()
    {
        OptionItem *item = currentItem();
        if (!item)
        {
            messageBox("Select an option to edit", mfInformation | mfOKButton);
            return;
        }
        switch (item->definition.kind)
        {
        case config::OptionKind::Boolean:
        {
            bool value = !item->value.toBool();
            item->value = config::OptionValue(value);
            registry.set(item->definition.key, item->value);
            refreshItems();
            break;
        }
        case config::OptionKind::Integer:
        case config::OptionKind::String:
        {
            std::string initial = item->value.toString();
            struct Data
            {
                char buffer[256];
            } data{};
            std::snprintf(data.buffer, sizeof(data.buffer), "%s", initial.c_str());
            std::string title = "Edit " + item->definition.displayName;
            auto *dialog = new TDialog(TRect(0, 0, 60, 12), title.c_str());
            dialog->options |= ofCentered;
            auto *input = new TInputLine(TRect(3, 5, 57, 6), sizeof(data.buffer) - 1);
            dialog->insert(new TLabel(TRect(2, 4, 20, 5), "~V~alue:", input));
            dialog->insert(input);
            dialog->insert(new TButton(TRect(14, 8, 24, 10), "O~K~", cmOK, bfDefault));
            dialog->insert(new TButton(TRect(26, 8, 36, 10), "Cancel", cmCancel, bfNormal));
            if (TProgram::application->executeDialog(dialog, &data) != cmOK)
                return;
            std::string value = trim(data.buffer);
            if (item->definition.kind == config::OptionKind::Integer && value.empty())
                value = "0";
            registry.set(item->definition.key, config::OptionValue(value));
            item->value = registry.get(item->definition.key);
            refreshItems();
            break;
        }
        case config::OptionKind::StringList:
        {
            auto *dialog = new StringListDialog("Edit " + item->definition.displayName,
                                               "Use Insert/Delete keys to manage entries.",
                                               item->value.toStringList());
            if (TProgram::application->executeDialog(dialog, nullptr) != cmOK)
                return;
            std::vector<std::string> list = dialog->result();
            registry.set(item->definition.key, config::OptionValue(list));
            item->value = registry.get(item->definition.key);
            refreshItems();
            break;
        }
        }
    }

    void resetCurrent()
    {
        OptionItem *item = currentItem();
        if (!item)
        {
            messageBox("Select an option to reset", mfInformation | mfOKButton);
            return;
        }
        registry.reset(item->definition.key);
        item->value = registry.get(item->definition.key);
        refreshItems();
    }

    void resetAll()
    {
        registry.resetToDefaults();
        refreshItems();
    }

    bool saveChanges()
    {
        if (registry.saveDefaults())
        {
            messageBox("Defaults saved", mfInformation | mfOKButton);
            return true;
        }
        messageBox("Failed to save defaults", mfError | mfOKButton);
        return false;
    }
};

class ConfigStatusLine : public TStatusLine
{
public:
    ConfigStatusLine(TRect r)
        : TStatusLine(r, *new TStatusDef(0, 0xFFFF, nullptr))
    {
        rebuild();
    }

private:
    void rebuild()
    {
        disposeItems(items);
        auto *edit = new TStatusItem("Edit", kbNoKey, cmEditApp);
        ck::hotkeys::configureStatusItem(*edit, "Edit");
        auto *reload = new TStatusItem("Reload", kbNoKey, cmReloadApps);
        ck::hotkeys::configureStatusItem(*reload, "Reload");
        edit->next = reload;
        TStatusItem *tail = reload;
        if (ck::launcher::launchedFromCkLauncher())
        {
            auto *returnItem = new TStatusItem("Return", kbNoKey, cmReturnToLauncher);
            ck::hotkeys::configureStatusItem(*returnItem, "Return");
            tail->next = returnItem;
            tail = returnItem;
        }
        auto *quit = new TStatusItem("Quit", kbNoKey, cmQuit);
        ck::hotkeys::configureStatusItem(*quit, "Quit");
        tail->next = quit;
        items = edit;
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

class ConfigApp : public TApplication
{
public:
    ConfigApp()
        : TProgInit(&ConfigApp::initStatusLine, &ConfigApp::initMenuBar, &TApplication::initDeskTop),
          TApplication()
    {
        auto *window = new AppBrowserWindow();
        appWindow = window;
        deskTop->insert(window);
        reloadApplications();
    }

    virtual void handleEvent(TEvent &event) override
    {
        TApplication::handleEvent(event);
        if (event.what == evCommand)
        {
            switch (event.message.command)
            {
            case cmReloadApps:
                reloadApplications();
                break;
            case cmEditApp:
                editSelected();
                break;
            case cmResetApp:
                resetSelected();
                break;
            case cmClearApp:
                clearSelected();
                break;
            case cmExportApp:
                exportSelected();
                break;
            case cmImportApp:
                importSelected();
                break;
            case cmOpenConfigDir:
                showConfigDirectory();
                break;
            case cmReturnToLauncher:
                std::exit(ck::launcher::kReturnToLauncherExitCode);
                break;
        case cmAbout:
        {
            const auto &info = toolInfo();
            ck::ui::showAboutDialog(info.executable, CK_CONFIG_VERSION, info.aboutDescription);
            break;
        }
            default:
                return;
            }
            clearEvent(event);
        }
    }

    virtual void idle() override
    {
        TApplication::idle();
    }

    static TMenuBar *initMenuBar(TRect r)
    {
        r.b.y = r.a.y + 1;
        TSubMenu &fileMenu = *new TSubMenu("~F~ile", hcNoContext) +
                             *new TMenuItem("~R~eload", cmReloadApps, kbNoKey, hcNoContext) +
                             newLine();
        if (ck::launcher::launchedFromCkLauncher())
            fileMenu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher, kbNoKey, hcNoContext);
        fileMenu + *new TMenuItem("E~x~it", cmQuit, kbNoKey, hcExit);

        TMenuItem &menuChain = fileMenu +
                               *new TSubMenu("~P~rofile", hcNoContext) +
                               *new TMenuItem("~E~dit Options", cmEditApp, kbNoKey, hcNoContext) +
                               *new TMenuItem("Reset to ~D~efaults", cmResetApp, kbNoKey, hcNoContext) +
                               *new TMenuItem("~C~lear Saved Defaults", cmClearApp, kbNoKey, hcNoContext) +
                               newLine() +
                               *new TMenuItem("~E~xport...", cmExportApp, kbNoKey, hcNoContext) +
                               *new TMenuItem("~I~mport...", cmImportApp, kbNoKey, hcNoContext) +
                               *new TMenuItem("Open Config ~D~ir", cmOpenConfigDir, kbNoKey, hcNoContext) +
                               *new TSubMenu("~H~elp", hcNoContext) +
                               *new TMenuItem("~A~bout", cmAbout, kbNoKey, hcNoContext);

        ck::hotkeys::configureMenuTree(menuChain);
        return new TMenuBar(r, static_cast<TSubMenu &>(menuChain));
    }

    static TStatusLine *initStatusLine(TRect r)
    {
        r.a.y = r.b.y - 1;
        return new ConfigStatusLine(r);
    }

private:
    AppBrowserWindow *appWindow = nullptr;

    void reloadApplications()
    {
        if (!appWindow)
            return;
        appWindow->setEntries(gatherApplicationEntries());
    }

    std::optional<ApplicationEntry> currentSelection() const
    {
        if (!appWindow)
            return std::nullopt;
        return appWindow->selectedEntry();
    }

    void editSelected()
    {
        auto entry = currentSelection();
        if (!entry)
        {
            messageBox("No application selected", mfInformation | mfOKButton);
            return;
        }
        if (!entry->known || !entry->info.registerFn)
        {
            messageBox("Editing is not supported for unknown applications", mfError | mfOKButton);
            return;
        }
        auto *dialog = new OptionEditorDialog(entry->info);
        TProgram::application->executeDialog(dialog, nullptr);
        reloadApplications();
    }

    void resetSelected()
    {
        auto entry = currentSelection();
        if (!entry)
        {
            messageBox("No application selected", mfInformation | mfOKButton);
            return;
        }
        CliOptions opts;
        opts.action = CliAction::Reset;
        opts.appId = entry->info.id;
        resetApplication(opts);
        reloadApplications();
    }

    void clearSelected()
    {
        auto entry = currentSelection();
        if (!entry)
        {
            messageBox("No application selected", mfInformation | mfOKButton);
            return;
        }
        CliOptions opts;
        opts.action = CliAction::Clear;
        opts.appId = entry->info.id;
        clearApplication(opts);
        reloadApplications();
    }

    void exportSelected()
    {
        auto entry = currentSelection();
        if (!entry)
        {
            messageBox("No application selected", mfInformation | mfOKButton);
            return;
        }
        char name[PATH_MAX] = {};
        std::snprintf(name, sizeof(name), "%s.json", entry->info.id.c_str());
        auto *dialog = new TFileDialog(name, "Export Options", "~N~ame", fdOKButton, 1);
        if (TProgram::application->executeDialog(dialog, name) == cmCancel)
            return;
        std::filesystem::path chosen = name;
        std::error_code ec;
        if (std::filesystem::exists(chosen, ec))
        {
            std::string prompt = "Overwrite existing file?\n" + chosen.string();
            if (messageBox(prompt.c_str(), mfYesNoCancel | mfConfirmation) != cmYes)
                return;
        }
        CliOptions opts;
        opts.action = CliAction::Export;
        opts.appId = entry->info.id;
        opts.file = name;
        exportApplication(opts);
    }

    void importSelected()
    {
        auto entry = currentSelection();
        if (!entry)
        {
            messageBox("No application selected", mfInformation | mfOKButton);
            return;
        }
        char name[PATH_MAX] = {};
        auto *dialog = new TFileDialog("*.json", "Import Options", "~N~ame", fdOpenButton, 1);
        if (TProgram::application->executeDialog(dialog, name) == cmCancel)
            return;
        CliOptions opts;
        opts.action = CliAction::Import;
        opts.appId = entry->info.id;
        opts.file = name;
        importApplication(opts);
        reloadApplications();
    }

    void showConfigDirectory()
    {
        std::string path = config::OptionRegistry::configRoot().string();
        std::string message = "Configuration files are stored in:\n" + path;
        messageBox(message.c_str(), mfInformation | mfOKButton);
    }
};

} // namespace

int main(int argc, char **argv)
{
    ck::hotkeys::registerDefaultSchemes();
    ck::hotkeys::initializeFromEnvironment();
    ck::hotkeys::applyCommandLineScheme(argc, argv);

    int cliResult = runCli(argc, argv);
    if (cliResult >= 0)
        return cliResult;

    ConfigApp app;
    app.run();
    return 0;
}
