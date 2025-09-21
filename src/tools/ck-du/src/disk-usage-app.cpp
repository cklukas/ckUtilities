#include "disk_usage_core.hpp"
#include "disk_usage_options.hpp"

#define Uses_TApplication
#define Uses_TDeskTop
#define Uses_TDialog
#define Uses_TKeys
#define Uses_TInputLine
#define Uses_TLabel
#define Uses_TButton
#define Uses_TStaticText
#define Uses_TParamText
#define Uses_TListViewer
#define Uses_TColorAttr
#define Uses_TDrawBuffer
#define Uses_TEvent
#define Uses_TPalette
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TOutline
#define Uses_TScrollBar
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#define Uses_TWindow
#define Uses_MsgBox
#include <tvision/tv.h>

#include "ck/about_dialog.hpp"
#include "ck/options.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <deque>
#include <functional>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <limits>
#include <limits.h>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ck::du;
namespace config = ck::config;

static constexpr const char *kAboutDescription =
    "Analyze directory and file storage utilization.";

static const ushort cmViewFiles = 2001;
static const ushort cmViewFilesRecursive = 2002;
static const ushort cmAbout = 2100;
static const ushort cmUnitAuto = 2200;
static const ushort cmUnitBytes = 2201;
static const ushort cmUnitKB = 2202;
static const ushort cmUnitMB = 2203;
static const ushort cmUnitGB = 2204;
static const ushort cmUnitTB = 2205;
static const ushort cmUnitBlocks = 2206;
static const ushort cmSortUnsorted = 2300;
static const ushort cmSortNameAsc = 2301;
static const ushort cmSortNameDesc = 2302;
static const ushort cmSortSizeDesc = 2303;
static const ushort cmSortSizeAsc = 2304;
static const ushort cmSortModifiedDesc = 2305;
static const ushort cmSortModifiedAsc = 2306;
static const ushort cmOptionFollowNever = 2400;
static const ushort cmOptionFollowCommandLine = 2401;
static const ushort cmOptionFollowAll = 2402;
static const ushort cmOptionToggleHardLinks = 2403;
static const ushort cmOptionToggleNodump = 2404;
static const ushort cmOptionToggleErrors = 2405;
static const ushort cmOptionToggleOneFs = 2406;
static const ushort cmOptionEditIgnores = 2407;
static const ushort cmOptionEditThreshold = 2408;
static const ushort cmOptionLoad = 2409;
static const ushort cmOptionSave = 2410;
static const ushort cmOptionSaveDefaults = 2411;
static const ushort cmPatternAdd = 2500;
static const ushort cmPatternEdit = 2501;
static const ushort cmPatternDelete = 2502;

namespace
{
const char *const kOptionSymlinkPolicy = "symlinkPolicy";
const char *const kOptionHardLinks = "countHardLinksMultiple";
const char *const kOptionIgnoreNodump = "ignoreNodump";
const char *const kOptionReportErrors = "reportErrors";
const char *const kOptionThreshold = "threshold";
const char *const kOptionStayOnFilesystem = "stayOnFilesystem";
const char *const kOptionIgnorePatterns = "ignorePatterns";

struct DuOptions
{
    BuildDirectoryTreeOptions::SymlinkPolicy symlinkPolicy =
        BuildDirectoryTreeOptions::SymlinkPolicy::Never;
    bool followCommandLineSymlinks = false;
    bool countHardLinksMultipleTimes = false;
    bool ignoreNodump = false;
    bool reportErrors = true;
    std::int64_t threshold = 0;
    bool stayOnFilesystem = false;
    std::vector<std::string> ignorePatterns;
};

BuildDirectoryTreeOptions::SymlinkPolicy policyFromString(const std::string &value)
{
    if (value == "always")
        return BuildDirectoryTreeOptions::SymlinkPolicy::Always;
    if (value == "command-line")
        return BuildDirectoryTreeOptions::SymlinkPolicy::CommandLineOnly;
    return BuildDirectoryTreeOptions::SymlinkPolicy::Never;
}

std::string policyToString(BuildDirectoryTreeOptions::SymlinkPolicy policy)
{
    switch (policy)
    {
    case BuildDirectoryTreeOptions::SymlinkPolicy::Always:
        return "always";
    case BuildDirectoryTreeOptions::SymlinkPolicy::CommandLineOnly:
        return "command-line";
    case BuildDirectoryTreeOptions::SymlinkPolicy::Never:
    default:
        return "never";
    }
}

std::string trim(const std::string &text)
{
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(start, end - start);
}

std::optional<std::int64_t> parseThresholdValue(const std::string &input)
{
    std::string trimmed = trim(input);
    if (trimmed.empty())
        return static_cast<std::int64_t>(0);

    bool negative = false;
    std::size_t pos = 0;
    if (trimmed[pos] == '+' || trimmed[pos] == '-')
    {
        negative = trimmed[pos] == '-';
        ++pos;
    }
    if (pos >= trimmed.size() || !std::isdigit(static_cast<unsigned char>(trimmed[pos])))
        return std::nullopt;

    std::uint64_t value = 0;
    while (pos < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[pos])))
    {
        unsigned digit = static_cast<unsigned>(trimmed[pos] - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
        ++pos;
    }

    std::uint64_t multiplier = 1;
    if (pos < trimmed.size())
    {
        char suffix = static_cast<char>(std::tolower(static_cast<unsigned char>(trimmed[pos])));
        switch (suffix)
        {
        case 'k':
            multiplier = 1024ull;
            break;
        case 'm':
            multiplier = 1024ull * 1024ull;
            break;
        case 'g':
            multiplier = 1024ull * 1024ull * 1024ull;
            break;
        case 't':
            multiplier = 1024ull * 1024ull * 1024ull * 1024ull;
            break;
        case 'b':
            multiplier = 1;
            break;
        default:
            return std::nullopt;
        }
        ++pos;
    }

    if (pos != trimmed.size())
        return std::nullopt;

    if (multiplier != 1 && value > std::numeric_limits<std::uint64_t>::max() / multiplier)
        return std::nullopt;
    std::uint64_t bytes = value * multiplier;
    if (bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;

    std::int64_t result = static_cast<std::int64_t>(bytes);
    if (negative)
        result = -result;
    return result;
}

std::string formatThresholdLabel(std::int64_t threshold)
{
    const std::string base = "Size ~T~hreshold...";
    if (threshold == 0)
        return base + " (Off)";
    bool less = threshold < 0;
    std::uintmax_t magnitude = static_cast<std::uintmax_t>(std::llabs(threshold));
    std::string formatted = formatSize(magnitude, SizeUnit::Auto);
    return base + " (" + (less ? "≤ " : "≥ ") + formatted + ")";
}

std::string ignoreMenuLabel(const DuOptions &options)
{
    const std::string base = "Ignore ~P~atterns...";
    if (options.ignorePatterns.empty())
        return base;
    if (options.ignorePatterns.size() == 1)
        return base + " (" + options.ignorePatterns.front() + ")";
    return base + " (" + std::to_string(options.ignorePatterns.size()) + ")";
}

DuOptions optionsFromRegistry(const config::OptionRegistry &registry)
{
    DuOptions opts;
    opts.symlinkPolicy = policyFromString(registry.getString(kOptionSymlinkPolicy, "never"));
    opts.followCommandLineSymlinks = opts.symlinkPolicy != BuildDirectoryTreeOptions::SymlinkPolicy::Never;
    opts.countHardLinksMultipleTimes = registry.getBool(kOptionHardLinks, false);
    opts.ignoreNodump = registry.getBool(kOptionIgnoreNodump, false);
    opts.reportErrors = registry.getBool(kOptionReportErrors, true);
    opts.threshold = registry.getInteger(kOptionThreshold, 0);
    opts.stayOnFilesystem = registry.getBool(kOptionStayOnFilesystem, false);
    opts.ignorePatterns = registry.getStringList(kOptionIgnorePatterns);
    return opts;
}

BuildDirectoryTreeOptions makeScanOptions(const DuOptions &options)
{
    BuildDirectoryTreeOptions scan;
    scan.symlinkPolicy = options.symlinkPolicy;
    scan.followCommandLineSymlinks = options.followCommandLineSymlinks;
    scan.countHardLinksMultipleTimes = options.countHardLinksMultipleTimes;
    scan.ignoreNodumpFlag = options.ignoreNodump;
    scan.reportErrors = options.reportErrors;
    scan.threshold = options.threshold;
    scan.stayOnFilesystem = options.stayOnFilesystem;
    scan.ignoreMasks = options.ignorePatterns;
    return scan;
}

std::array<TMenuItem *, 7> gUnitMenuItems{};
}

namespace
{
std::array<TMenuItem *, 7> gSortMenuItems{};
}

namespace
{
std::array<TMenuItem *, 3> gSymlinkMenuItems{};
TMenuItem *gHardLinkMenuItem = nullptr;
TMenuItem *gNodumpMenuItem = nullptr;
TMenuItem *gErrorsMenuItem = nullptr;
TMenuItem *gOneFsMenuItem = nullptr;
TMenuItem *gIgnoreMenuItem = nullptr;
TMenuItem *gThresholdMenuItem = nullptr;
}

namespace
{
class PatternListViewer : public TListViewer
{
public:
    PatternListViewer(const TRect &bounds, std::vector<std::string> &items, TScrollBar *vScroll)
        : TListViewer(bounds, 1, nullptr, vScroll), patterns(&items)
    {
        growMode = gfGrowHiX | gfGrowHiY;
        setRange(static_cast<short>(patterns->size()));
    }

    void updateRange()
    {
        setRange(static_cast<short>(patterns->size()));
    }

    short currentIndex() const { return focused; }

    virtual void getText(char *dest, short item, short maxChars) override
    {
        if (!patterns || item < 0 || item >= static_cast<short>(patterns->size()))
        {
            if (maxChars > 0)
                dest[0] = '\0';
            return;
        }
        std::snprintf(dest, static_cast<std::size_t>(maxChars), "%s", (*patterns)[static_cast<std::size_t>(item)].c_str());
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
    std::vector<std::string> *patterns;
};

class PatternEditorDialog : public TDialog
{
public:
    explicit PatternEditorDialog(const std::vector<std::string> &initialPatterns)
        : TWindowInit(&TDialog::initFrame),
          TDialog(TRect(0, 0, 74, 21), "Ignore Patterns"),
          patterns(initialPatterns)
    {
        options |= ofCentered;

        insert(new TStaticText(TRect(2, 2, 72, 4),
                               "Manage wildcard masks. Use '*' and '?' for matching."
                               " Use Insert/Delete keys for quick edits."));

        vScroll = new TScrollBar(TRect(70, 4, 71, 16));
        vScroll->growMode = gfGrowHiY;
        insert(vScroll);

        listView = new PatternListViewer(TRect(3, 4, 70, 16), patterns, vScroll);
        listView->growMode = gfGrowHiX | gfGrowHiY;
        insert(listView);

        insert(new TButton(TRect(3, 16, 15, 18), "~A~dd", cmPatternAdd, bfNormal));
        insert(new TButton(TRect(17, 16, 29, 18), "~E~dit", cmPatternEdit, bfNormal));
        insert(new TButton(TRect(31, 16, 43, 18), "~R~emove", cmPatternDelete, bfNormal));
        insert(new TButton(TRect(45, 16, 57, 18), "O~K~", cmOK, bfDefault));
        insert(new TButton(TRect(59, 16, 71, 18), "Cancel", cmCancel, bfNormal));
    }

    std::vector<std::string> result() const { return patterns; }

protected:
    virtual void handleEvent(TEvent &event) override
    {
        TDialog::handleEvent(event);
        if (event.what == evCommand)
        {
            switch (event.message.command)
            {
            case cmPatternAdd:
                addPattern();
                break;
            case cmPatternEdit:
                editPattern();
                break;
            case cmPatternDelete:
                deletePattern();
                break;
            default:
                return;
            }
            clearEvent(event);
        }
    }

private:
    PatternListViewer *listView = nullptr;
    TScrollBar *vScroll = nullptr;
    std::vector<std::string> patterns;

    void refreshList()
    {
        if (listView)
        {
            listView->updateRange();
            listView->drawView();
        }
        if (vScroll)
            vScroll->drawView();
    }

    bool promptForPattern(const char *title, const char *label, const std::string &initial, std::string &output)
    {
        struct Data
        {
            char buffer[256];
        } data{};
        std::snprintf(data.buffer, sizeof(data.buffer), "%s", initial.c_str());

        while (true)
        {
            auto *dialog = new TDialog(TRect(0, 0, 64, 12), title);
            dialog->options |= ofCentered;
            auto *input = new TInputLine(TRect(3, 5, 60, 6), sizeof(data.buffer) - 1);
            dialog->insert(new TLabel(TRect(2, 4, 20, 5), label, input));
            dialog->insert(input);
            dialog->insert(new TButton(TRect(18, 8, 28, 10), "O~K~", cmOK, bfDefault));
            dialog->insert(new TButton(TRect(30, 8, 40, 10), "Cancel", cmCancel, bfNormal));

            ushort code = TProgram::application->executeDialog(dialog, &data);
            if (code != cmOK)
                return false;

            std::string value = trim(data.buffer);
            if (value.empty())
            {
                messageBox("Pattern cannot be empty", mfError | mfOKButton);
                continue;
            }
            output = value;
            return true;
        }
    }

    void addPattern()
    {
        std::string value;
        if (!promptForPattern("Add Pattern", "~P~attern:", std::string(), value))
            return;
        patterns.push_back(value);
        refreshList();
    }

    void editPattern()
    {
        if (!listView)
            return;
        short index = listView->currentIndex();
        if (index < 0 || index >= static_cast<short>(patterns.size()))
        {
            messageBox("Select a pattern to edit", mfInformation | mfOKButton);
            return;
        }
        std::string value;
        if (!promptForPattern("Edit Pattern", "~P~attern:", patterns[static_cast<std::size_t>(index)], value))
            return;
        patterns[static_cast<std::size_t>(index)] = value;
        refreshList();
    }

    void deletePattern()
    {
        if (!listView)
            return;
        short index = listView->currentIndex();
        if (index < 0 || index >= static_cast<short>(patterns.size()))
        {
            messageBox("Select a pattern to remove", mfInformation | mfOKButton);
            return;
        }
        std::string label = "Remove pattern?\n" + patterns[static_cast<std::size_t>(index)];
        if (messageBox(label.c_str(), mfYesNoCancel | mfConfirmation) != cmYes)
            return;
        patterns.erase(patterns.begin() + index);
        refreshList();
    }
};
}

namespace
{
std::string directoryLabel(const DirectoryNode *node)
{
    std::string name;
    if (node->parent == nullptr)
        name = node->path.string();
    else
        name = node->path.filename().string();
    if (name.empty())
        name = node->path.string();

    std::ostringstream out;
    out << name << "  [" << formatSize(node->stats.totalSize, getCurrentUnit()) << "]";
    out << "  " << node->stats.fileCount << " files";
    if (node->stats.directoryCount > 0)
        out << ", " << node->stats.directoryCount << " dirs";
    return out.str();
}

std::string directorySortName(const DirectoryNode *node)
{
    if (!node)
        return std::string();
    std::string name = node->path.filename().string();
    if (name.empty())
        name = node->path.string();
    return name;
}

std::vector<DirectoryNode *> orderedChildren(DirectoryNode *node)
{
    std::vector<DirectoryNode *> order;
    if (!node)
        return order;
    order.reserve(node->children.size());
    for (auto &child : node->children)
        order.push_back(child.get());

    auto key = getCurrentSortKey();
    auto nameLess = [](DirectoryNode *a, DirectoryNode *b) {
        return directorySortName(a) < directorySortName(b);
    };
    auto nameGreater = [](DirectoryNode *a, DirectoryNode *b) {
        return directorySortName(a) > directorySortName(b);
    };

    switch (key)
    {
    case SortKey::Unsorted:
        break;
    case SortKey::NameAscending:
        std::stable_sort(order.begin(), order.end(), nameLess);
        break;
    case SortKey::NameDescending:
        std::stable_sort(order.begin(), order.end(), nameGreater);
        break;
    case SortKey::SizeDescending:
        std::stable_sort(order.begin(), order.end(), [&](DirectoryNode *a, DirectoryNode *b) {
            if (a->stats.totalSize == b->stats.totalSize)
                return nameLess(a, b);
            return a->stats.totalSize > b->stats.totalSize;
        });
        break;
    case SortKey::SizeAscending:
        std::stable_sort(order.begin(), order.end(), [&](DirectoryNode *a, DirectoryNode *b) {
            if (a->stats.totalSize == b->stats.totalSize)
                return nameLess(a, b);
            return a->stats.totalSize < b->stats.totalSize;
        });
        break;
    case SortKey::ModifiedDescending:
        std::stable_sort(order.begin(), order.end(), [&](DirectoryNode *a, DirectoryNode *b) {
            if (a->modifiedTime == b->modifiedTime)
                return nameLess(a, b);
            return a->modifiedTime > b->modifiedTime;
        });
        break;
    case SortKey::ModifiedAscending:
        std::stable_sort(order.begin(), order.end(), [&](DirectoryNode *a, DirectoryNode *b) {
            if (a->modifiedTime == b->modifiedTime)
                return nameLess(a, b);
            return a->modifiedTime < b->modifiedTime;
        });
        break;
    }

    return order;
}

void applySortToFiles(std::vector<FileEntry> &entries)
{
    auto key = getCurrentSortKey();
    auto nameLess = [](const FileEntry &a, const FileEntry &b) {
        return a.displayPath < b.displayPath;
    };
    auto nameGreater = [](const FileEntry &a, const FileEntry &b) {
        return a.displayPath > b.displayPath;
    };

    switch (key)
    {
    case SortKey::Unsorted:
        break;
    case SortKey::NameAscending:
        std::stable_sort(entries.begin(), entries.end(), nameLess);
        break;
    case SortKey::NameDescending:
        std::stable_sort(entries.begin(), entries.end(), nameGreater);
        break;
    case SortKey::SizeDescending:
        std::stable_sort(entries.begin(), entries.end(), [&](const FileEntry &a, const FileEntry &b) {
            if (a.size == b.size)
                return nameLess(a, b);
            return a.size > b.size;
        });
        break;
    case SortKey::SizeAscending:
        std::stable_sort(entries.begin(), entries.end(), [&](const FileEntry &a, const FileEntry &b) {
            if (a.size == b.size)
                return nameLess(a, b);
            return a.size < b.size;
        });
        break;
    case SortKey::ModifiedDescending:
        std::stable_sort(entries.begin(), entries.end(), [&](const FileEntry &a, const FileEntry &b) {
            if (a.modifiedTime == b.modifiedTime)
                return nameLess(a, b);
            return a.modifiedTime > b.modifiedTime;
        });
        break;
    case SortKey::ModifiedAscending:
        std::stable_sort(entries.begin(), entries.end(), [&](const FileEntry &a, const FileEntry &b) {
            if (a.modifiedTime == b.modifiedTime)
                return nameLess(a, b);
            return a.modifiedTime < b.modifiedTime;
        });
        break;
    }
}

} // namespace

class DirTNode : public TNode
{
public:
    DirectoryNode *dirNode;
    DirTNode *parent = nullptr;
    DirTNode(DirectoryNode *node, TStringView text, DirTNode *children = nullptr,
             DirTNode *next = nullptr, Boolean exp = True) noexcept
        : TNode(text, children, next, exp), dirNode(node) {}
};

class DirectoryWindow;

class DirectoryOutline : public TOutline
{
public:
    DirectoryOutline(TRect bounds, TScrollBar *h, TScrollBar *v, DirTNode *rootNode, DirectoryWindow &owner);

    DirTNode *focusedNode();
    void focusNode(DirTNode *target);
    void syncExpanded();

    virtual void handleEvent(TEvent &event) override;

private:
    DirectoryWindow &ownerWindow;
};

class FileListWindow;
class FileListHeaderView;

class FileListView : public TListViewer
{
public:
    FileListView(const TRect &bounds, TScrollBar *h, TScrollBar *v,
                 std::vector<FileEntry> &entries);

    virtual void getText(char *dest, short item, short maxLen) override;
    virtual void changeBounds(const TRect &bounds) override;
    virtual void handleEvent(TEvent &event) override;

    void refreshMetrics();
    void setHeader(FileListHeaderView *headerView);
    std::string headerLine() const;
    int horizontalOffset() const;
    ushort headerColorIndex() const;

private:
    static constexpr std::size_t kSeparatorWidth = 2;
    static constexpr std::size_t kSeparatorCount = 5;

    std::vector<FileEntry> &files;
    FileListHeaderView *header = nullptr;
    std::size_t maxLineWidth = 0;
    std::size_t nameWidth = 0;
    std::size_t ownerWidth = 0;
    std::size_t groupWidth = 0;
    std::size_t sizeWidth = 0;
    std::size_t createdWidth = 0;
    std::size_t modifiedWidth = 0;

    void computeWidths();
    std::size_t totalLineWidth() const;
    void updateMaxLineWidth();
    std::string formatRow(const std::string &name, const std::string &owner,
                          const std::string &group, const std::string &size,
                          const std::string &created, const std::string &modified) const;
    void notifyHeader();
};

class FileListHeaderView : public TView
{
public:
    FileListHeaderView(const TRect &bounds, FileListView &listView);

    virtual void draw() override;
    void refresh();

private:
    FileListView &listView;
};

class FileListWindow : public TWindow
{
public:
    FileListWindow(const std::string &title, std::vector<FileEntry> files, bool recursive, class DiskUsageApp &app);
    ~FileListWindow();

    void refreshUnits();
    void refreshSort();

private:
    class DiskUsageApp &app;
    std::vector<FileEntry> baseEntries;
    std::vector<FileEntry> entries;
    FileListView *listView = nullptr;
    TScrollBar *hScroll = nullptr;
    TScrollBar *vScroll = nullptr;
    FileListHeaderView *headerView = nullptr;
    bool recursiveMode = false;

    void buildView();
};

class DirectoryWindow : public TWindow
{
public:
    DirectoryWindow(const std::filesystem::path &path, std::unique_ptr<DirectoryNode> rootNode, DuOptions options,
                    class DiskUsageApp &app);
    ~DirectoryWindow();

    DirectoryNode *focusedNode() const;
    void refreshLabels();
    void refreshSort();
    const DuOptions &scanOptions() const { return options; }
    std::filesystem::path rootPath() const;

private:
    class DiskUsageApp &app;
    std::unique_ptr<DirectoryNode> root;
    DuOptions options;
    DirectoryOutline *outline = nullptr;
    TScrollBar *hScroll = nullptr;
    TScrollBar *vScroll = nullptr;
    std::unordered_map<DirectoryNode *, DirTNode *> nodeMap;

    DirTNode *buildNodes(DirectoryNode *node);
    void buildOutline();
    friend class DirectoryOutline;
};

class ScanProgressDialog : public TDialog
{
public:
    ScanProgressDialog();

    void setCancelHandler(std::function<void()> handler);
    void updatePath(const std::string &path);

    virtual void handleEvent(TEvent &event) override;

private:
    void setPathText(const std::string &text);

    TParamText *pathText = nullptr;
    std::function<void()> cancelHandler;
    std::string lastDisplay;
};

class DiskUsageStatusLine : public TStatusLine
{
public:
    DiskUsageStatusLine(TRect r)
        : TStatusLine(r, *new TStatusDef(0, 0xFFFF, nullptr))
    {
        rebuild();
    }

private:
    void rebuild()
    {
        disposeItems(items);
        auto *open = new TStatusItem("~F2~ Open", kbF2, cmOpen);
        auto *files = new TStatusItem("~F3~ Files", kbF3, cmViewFiles);
        auto *recursive = new TStatusItem("~Shift-F3~ Files+Sub", kbShiftF3, cmViewFilesRecursive);
        auto *sortName = new TStatusItem("~Ctrl-N~ Sort Name", kbCtrlN, cmSortNameAsc);
        auto *sortSize = new TStatusItem("~Ctrl-S~ Sort Size", kbCtrlS, cmSortSizeDesc);
        auto *sortModified = new TStatusItem("~Ctrl-M~ Sort Modified", kbCtrlM, cmSortModifiedDesc);
        auto *quit = new TStatusItem("~Alt-X~ Quit", kbAltX, cmQuit);
        open->next = files;
        files->next = recursive;
        recursive->next = sortName;
        sortName->next = sortSize;
        sortSize->next = sortModified;
        sortModified->next = quit;
        items = open;
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

class DiskUsageApp : public TApplication
{
public:
    DiskUsageApp(const std::vector<std::filesystem::path> &paths,
                 std::shared_ptr<config::OptionRegistry> registry);
    ~DiskUsageApp();

    virtual void handleEvent(TEvent &event) override;
    virtual void idle() override;

    static TMenuBar *initMenuBar(TRect r);
    static TStatusLine *initStatusLine(TRect r);

    void registerDirectoryWindow(DirectoryWindow *window);
    void unregisterDirectoryWindow(DirectoryWindow *window);
    void registerFileWindow(FileListWindow *window);
    void unregisterFileWindow(FileListWindow *window);

    void notifyUnitsChanged();
    void notifySortChanged();

private:
    std::vector<DirectoryWindow *> directoryWindows;
    std::vector<FileListWindow *> fileWindows;
    std::unordered_map<SizeUnit, TMenuItem *> unitMenuItems;
    std::unordered_map<SizeUnit, std::string> unitBaseLabels;
    std::unordered_map<SortKey, TMenuItem *> sortMenuItems;
    std::unordered_map<SortKey, std::string> sortBaseLabels;

    std::array<TMenuItem *, 3> symlinkMenuItems{};
    std::array<std::string, 3> symlinkBaseLabels{};
    std::string hardLinkBaseLabel = "Count ~H~ard Links Multiple Times";
    std::string nodumpBaseLabel = "Ignore ~N~odump Flag";
    std::string errorsBaseLabel = "Report ~E~rrors";
    std::string oneFsBaseLabel = "Stay on One ~F~ile System";
    TMenuItem *hardLinkMenuItem = nullptr;
    TMenuItem *nodumpMenuItem = nullptr;
    TMenuItem *errorsMenuItem = nullptr;
    TMenuItem *oneFsMenuItem = nullptr;
    TMenuItem *ignoreMenuItem = nullptr;
    TMenuItem *thresholdMenuItem = nullptr;
    std::shared_ptr<config::OptionRegistry> optionRegistry;
    DuOptions currentOptions;
    bool rescanRequested = false;
    bool rescanInProgress = false;

    struct DirectoryScanTask
    {
        std::filesystem::path rootPath;
        std::thread worker;
        std::mutex mutex;
        std::unique_ptr<DirectoryNode> result;
        std::string currentPath;
        std::string errorMessage;
        bool cancelled = false;
        bool failed = false;
        DuOptions optionState;
        BuildDirectoryTreeOptions scanOptions;
        std::vector<std::string> errors;
        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> finished{false};
        ScanProgressDialog *dialog = nullptr;
    };

    std::unique_ptr<DirectoryScanTask> activeScan;
    std::deque<std::filesystem::path> pendingScanQueue;

    void promptOpenDirectory();
    void openDirectory(const std::filesystem::path &path);
    void viewFiles(bool recursive);
    DirectoryWindow *activeDirectoryWindow() const;
    void updateUnitMenu();
    void applyUnit(SizeUnit unit);
    void updateSortMenu();
    void applySortMode(SortKey key);
    void updateOptionsMenu();
    void updateSymlinkMenu();
    void updateToggleMenuItem(TMenuItem *item, bool enabled, const std::string &baseLabel);
    void optionsChanged(bool triggerRescan);
    void requestRescanAllDirectories();
    void processRescanRequests();
    void performRescanAllDirectories();
    void applySymlinkPolicy(BuildDirectoryTreeOptions::SymlinkPolicy policy);
    void toggleHardLinks();
    void toggleNodump();
    void toggleErrors();
    void toggleOneFilesystem();
    void editIgnorePatterns();
    void editThreshold();
    void loadOptionsFromFile();
    void saveOptionsToFile();
    void saveDefaultOptions();
    void reloadOptionState();
    void requestDirectoryScan(const std::filesystem::path &path, bool allowQueue);
    void queueDirectoryForScan(const std::filesystem::path &path);
    void startDirectoryScan(const std::filesystem::path &path);
    void startNextQueuedDirectory();
    void updateScanProgress(DirectoryScanTask &task);
    void processActiveScanCompletion();
    void runDirectoryScan(DirectoryScanTask &task);
    void requestScanCancellation();
    void closeProgressDialog(DirectoryScanTask &task);
    void cancelActiveScan(bool waitForCompletion);
};

DirectoryOutline::DirectoryOutline(TRect bounds, TScrollBar *h, TScrollBar *v, DirTNode *rootNode, DirectoryWindow &owner)
    : TOutline(bounds, h, v, rootNode), ownerWindow(owner)
{
}

ScanProgressDialog::ScanProgressDialog()
    : TWindowInit(&TDialog::initFrame),
      TDialog(TRect(0, 0, 60, 9), "Scanning Directory")
{
    options |= ofCentered;
    insert(new TStaticText(TRect(2, 2, 58, 3), "Scanning directory..."));
    pathText = new TParamText(TRect(2, 3, 58, 4));
    insert(pathText);
    pathText->setText("%s", "Current: (scanning...)");
    insert(new TButton(TRect(24, 6, 36, 8), "~C~ancel", cmCancel, bfNormal));
}

void ScanProgressDialog::setCancelHandler(std::function<void()> handler)
{
    cancelHandler = std::move(handler);
}

void ScanProgressDialog::setPathText(const std::string &text)
{
    if (!pathText)
        return;
    pathText->setText("%s", text.c_str());
    pathText->drawView();
}

void ScanProgressDialog::updatePath(const std::string &path)
{
    std::string display = path.empty() ? std::string("(scanning...)") : path;
    constexpr std::size_t maxDisplayLength = 47;
    if (display.size() > maxDisplayLength)
        display = "..." + display.substr(display.size() - (maxDisplayLength - 3));
    if (display == lastDisplay)
        return;
    lastDisplay = display;
    setPathText("Current: " + display);
}

void ScanProgressDialog::handleEvent(TEvent &event)
{
    if (event.what == evCommand && event.message.command == cmCancel)
    {
        if (cancelHandler)
            cancelHandler();
        clearEvent(event);
        return;
    }
    TDialog::handleEvent(event);
}

DirTNode *DirectoryOutline::focusedNode()
{
    return static_cast<DirTNode *>(getNode(foc));
}

void DirectoryOutline::focusNode(DirTNode *target)
{
    if (!target)
        return;
    struct Finder
    {
        DirTNode *target;
        int index = 0;
        int found = -1;
    } finder{target};

    forEach([](TOutlineViewer *, TNode *node, int, int pos, long, ushort, void *arg) -> Boolean {
        auto &f = *static_cast<Finder *>(arg);
        if (node == f.target)
        {
            f.found = f.index;
            return True;
        }
        ++f.index;
        return False;
    }, &finder);

    if (finder.found >= 0)
    {
        foc = finder.found;
        scrollTo(0, finder.found);
        drawView();
        focused(finder.found);
    }
}

void DirectoryOutline::syncExpanded()
{
    forEach([](TOutlineViewer *, TNode *node, int, int, long, ushort, void *arg) -> Boolean {
        auto *dirNode = static_cast<DirTNode *>(node);
        dirNode->expanded = dirNode->dirNode->expanded ? True : False;
        return False;
    }, nullptr);
    update();
}

void DirectoryOutline::handleEvent(TEvent &event)
{
    if (event.what == evMouseDown && (event.mouse.buttons & mbLeftButton))
    {
        int clickX = event.mouse.where.x;
        TOutline::handleEvent(event);
        if (DirTNode *node = focusedNode())
        {
            int depth = 0;
            for (DirectoryNode *p = node->dirNode; p && p->parent; p = p->parent)
                ++depth;
            int prefixWidth = depth * 2 + 2;
            if (clickX < prefixWidth)
            {
                node->expanded = node->expanded ? False : True;
                node->dirNode->expanded = node->expanded;
                update();
                drawView();
            }
        }
        return;
    }
    if (event.what == evKeyDown)
    {
        DirTNode *node = focusedNode();
        switch (event.keyDown.keyCode)
        {
        case kbLeft:
            if (node)
            {
                if (node->expanded && node->childList)
                {
                    node->expanded = False;
                    node->dirNode->expanded = false;
                    update();
                    drawView();
                }
                else if (node->parent)
                    focusNode(static_cast<DirTNode *>(node->parent));
            }
            clearEvent(event);
            return;
        case kbRight:
            if (node)
            {
                if (!node->expanded && node->childList)
                {
                    node->expanded = True;
                    node->dirNode->expanded = true;
                    update();
                    drawView();
                }
                else if (node->childList)
                    focusNode(static_cast<DirTNode *>(node->childList));
            }
            clearEvent(event);
            return;
        default:
            break;
        }
    }
    TOutline::handleEvent(event);
}

FileListView::FileListView(const TRect &bounds, TScrollBar *h, TScrollBar *v, std::vector<FileEntry> &entries)
    : TListViewer(bounds, 1, h, v), files(entries)
{
    setRange(static_cast<short>(entries.size()));
    computeWidths();
    updateMaxLineWidth();
}

void FileListView::computeWidths()
{
    nameWidth = std::string("Name").size();
    ownerWidth = std::string("Owner").size();
    groupWidth = std::string("Group").size();
    sizeWidth = std::string("Size").size();
    createdWidth = std::string("Created").size();
    modifiedWidth = std::string("Modified").size();

    for (const auto &entry : files)
    {
        nameWidth = std::max(nameWidth, entry.displayPath.size());
        ownerWidth = std::max(ownerWidth, entry.owner.size());
        groupWidth = std::max(groupWidth, entry.group.size());
        createdWidth = std::max(createdWidth, entry.created.size());
        modifiedWidth = std::max(modifiedWidth, entry.modified.size());
        sizeWidth = std::max(sizeWidth, formatSize(entry.size, getCurrentUnit()).size());
    }
    createdWidth = std::max(createdWidth, std::string("YYYY-MM-DD HH:MM").size());
    modifiedWidth = std::max(modifiedWidth, std::string("YYYY-MM-DD HH:MM").size());
    sizeWidth = std::max(sizeWidth, std::string("0 B").size());
}

void FileListView::refreshMetrics()
{
    computeWidths();
    updateMaxLineWidth();
    if (hScrollBar)
    {
        int visibleWidth = std::max<int>(1, size.x);
        int maxIndent = 0;
        if (static_cast<int>(maxLineWidth) > visibleWidth)
            maxIndent = static_cast<int>(maxLineWidth) - visibleWidth;
        int current = hScrollBar->value;
        if (current > maxIndent)
            current = maxIndent;
        int pageStep = std::max<int>(1, visibleWidth - 1);
        hScrollBar->setParams(current, 0, maxIndent, pageStep, 1);
    }
    drawView();
    notifyHeader();
}

void FileListView::getText(char *dest, short item, short maxLen)
{
    if (item < 0 || static_cast<std::size_t>(item) >= files.size())
    {
        *dest = '\0';
        return;
    }

    const FileEntry &entry = files[item];
    std::string sizeStr = formatSize(entry.size, getCurrentUnit());
    std::string text = formatRow(entry.displayPath, entry.owner, entry.group, sizeStr, entry.created, entry.modified);
    if (text.size() >= static_cast<std::size_t>(maxLen))
        text.resize(maxLen - 1);
    std::snprintf(dest, maxLen, "%s", text.c_str());
}

void FileListView::changeBounds(const TRect &bounds)
{
    TListViewer::changeBounds(bounds);
    refreshMetrics();
}

void FileListView::handleEvent(TEvent &event)
{
    TListViewer::handleEvent(event);
    notifyHeader();
}



void FileListView::setHeader(FileListHeaderView *headerView)
{
    header = headerView;
}

std::string FileListView::headerLine() const
{
    return formatRow("Name", "Owner", "Group", "Size", "Created", "Modified");
}

int FileListView::horizontalOffset() const
{
    if (hScrollBar)
        return hScrollBar->value;
    return 0;
}

ushort FileListView::headerColorIndex() const
{
    if (getState(sfActive) && getState(sfSelected))
        return 1;
    return 2;
}

std::size_t FileListView::totalLineWidth() const
{
    return nameWidth + ownerWidth + groupWidth + sizeWidth + createdWidth + modifiedWidth +
           kSeparatorWidth * kSeparatorCount;
}

void FileListView::updateMaxLineWidth()
{
    maxLineWidth = totalLineWidth();
    if (maxLineWidth < static_cast<std::size_t>(size.x))
        maxLineWidth = static_cast<std::size_t>(size.x);
}

std::string FileListView::formatRow(const std::string &name, const std::string &owner,
                                    const std::string &group, const std::string &size,
                                    const std::string &created, const std::string &modified) const
{
    static constexpr const char *separator = "  ";
    std::ostringstream line;
    line << std::left << std::setw(nameWidth) << name;
    line << separator;
    line << std::left << std::setw(ownerWidth) << owner;
    line << separator;
    line << std::left << std::setw(groupWidth) << group;
    line << separator;
    line << std::right << std::setw(sizeWidth) << size;
    line << separator;
    line << std::left << std::setw(createdWidth) << created;
    line << separator;
    line << std::left << std::setw(modifiedWidth) << modified;
    return line.str();
}

void FileListView::notifyHeader()
{
    if (header)
        header->refresh();
}

FileListHeaderView::FileListHeaderView(const TRect &bounds, FileListView &list)
    : TView(bounds), listView(list)
{
    options &= ~(ofSelectable | ofFirstClick);
}

void FileListHeaderView::draw()
{
    TDrawBuffer buffer;
    TColorAttr color = listView.getColor(listView.headerColorIndex());
    buffer.moveChar(0, ' ', color, size.x);
    std::string headerText = listView.headerLine();
    int indent = listView.horizontalOffset();
    if (indent < 0)
        indent = 0;
    if (indent < 255)
        buffer.moveStr(0, headerText.c_str(), color, size.x, indent);
    writeLine(0, 0, size.x, 1, buffer);
}



void FileListHeaderView::refresh()
{
    drawView();
}

FileListWindow::FileListWindow(const std::string &title, std::vector<FileEntry> files, bool recursive, DiskUsageApp &appRef)
    : TWindowInit(&TWindow::initFrame),
      TWindow(TRect(0, 0, 78, 20), title.c_str(), wnNoNumber),
      app(appRef), baseEntries(std::move(files)), recursiveMode(recursive)
{
    flags |= wfGrow;
    growMode = gfGrowHiX | gfGrowHiY;
    refreshSort();
    buildView();
    app.registerFileWindow(this);
}

FileListWindow::~FileListWindow()
{
    app.unregisterFileWindow(this);
}

void FileListWindow::buildView()
{
    TRect client = getExtent();
    client.grow(-1, -1);
    if (client.b.x <= client.a.x + 2 || client.b.y <= client.a.y + 3)
        client = TRect(0, 0, 76, 18);

    TRect headerBounds(client.a.x, client.a.y, client.b.x - 1, client.a.y + 1);
    TRect listBounds(client.a.x, client.a.y + 1, client.b.x - 1, client.b.y - 1);

    vScroll = new TScrollBar(TRect(client.b.x - 1, client.a.y, client.b.x, client.b.y - 1));
    vScroll->growMode = gfGrowHiY;
    hScroll = new TScrollBar(TRect(client.a.x, client.b.y - 1, client.b.x - 1, client.b.y));
    hScroll->growMode = gfGrowHiX;

    auto *view = new FileListView(listBounds, hScroll, vScroll, entries);
    view->growMode = gfGrowHiX | gfGrowHiY;

    auto *header = new FileListHeaderView(headerBounds, *view);
    header->growMode = gfGrowHiX;

    view->setHeader(header);

    insert(vScroll);
    insert(hScroll);
    insert(header);
    insert(view);
    listView = view;
    headerView = header;
    view->refreshMetrics();
    headerView->refresh();
    hScroll->drawView();
    vScroll->drawView();
}

void FileListWindow::refreshUnits()
{
    if (listView)
    {
        listView->refreshMetrics();
    }
    if (headerView)
        headerView->refresh();
}

void FileListWindow::refreshSort()
{
    entries = baseEntries;
    applySortToFiles(entries);
    if (listView)
    {
        listView->setRange(static_cast<short>(entries.size()));
        listView->refreshMetrics();
    }
    if (headerView)
        headerView->refresh();
    if (hScroll)
        hScroll->drawView();
    if (vScroll)
        vScroll->drawView();
}

DirectoryWindow::DirectoryWindow(const std::filesystem::path &path, std::unique_ptr<DirectoryNode> rootNode,
                                 DuOptions optionsIn, DiskUsageApp &appRef)
    : TWindowInit(&TWindow::initFrame),
      TWindow(TRect(0, 0, 78, 20), path.filename().empty() ? path.string().c_str() : path.filename().string().c_str(), wnNoNumber),
      app(appRef), root(std::move(rootNode)), options(std::move(optionsIn))
{
    flags |= wfGrow;
    growMode = gfGrowHiX | gfGrowHiY;
    buildOutline();
    app.registerDirectoryWindow(this);
}

DirectoryWindow::~DirectoryWindow()
{
    app.unregisterDirectoryWindow(this);
}

DirTNode *DirectoryWindow::buildNodes(DirectoryNode *node)
{
    DirTNode *firstChild = nullptr;
    DirTNode *prev = nullptr;
    std::vector<DirTNode *> created;
    for (auto *childDir : orderedChildren(node))
    {
        DirTNode *childNode = buildNodes(childDir);
        created.push_back(childNode);
        if (!firstChild)
            firstChild = childNode;
        else
            prev->next = childNode;
        prev = childNode;
    }
    std::string label = directoryLabel(node);
    auto *current = new DirTNode(node, label.c_str(), firstChild, nullptr, node->expanded ? True : False);
    for (auto *childNode : created)
        childNode->parent = current;
    nodeMap[node] = current;
    return current;
}

void DirectoryWindow::buildOutline()
{
    nodeMap.clear();
    DirTNode *rootNode = buildNodes(root.get());
    rootNode->expanded = True;

    TRect client = getExtent();
    client.grow(-1, -1);
    if (client.b.x <= client.a.x + 2 || client.b.y <= client.a.y + 2)
        client = TRect(0, 0, 76, 18);

    TRect outlineBounds(client.a.x, client.a.y, client.b.x - 1, client.b.y - 1);
    vScroll = new TScrollBar(TRect(client.b.x - 1, client.a.y, client.b.x, client.b.y - 1));
    vScroll->growMode = gfGrowHiY;
    hScroll = new TScrollBar(TRect(client.a.x, client.b.y - 1, client.b.x - 1, client.b.y));
    hScroll->growMode = gfGrowHiX;

    auto *view = new DirectoryOutline(outlineBounds, hScroll, vScroll, rootNode, *this);
    view->growMode = gfGrowHiX | gfGrowHiY;
    insert(vScroll);
    insert(hScroll);
    insert(view);
    outline = view;
    outline->update();
    hScroll->drawView();
    vScroll->drawView();
    outline->drawView();
}

DirectoryNode *DirectoryWindow::focusedNode() const
{
    if (!outline)
        return nullptr;
    if (DirTNode *node = outline->focusedNode())
        return node->dirNode;
    return nullptr;
}

std::filesystem::path DirectoryWindow::rootPath() const
{
    return root ? root->path : std::filesystem::path();
}

void DirectoryWindow::refreshLabels()
{
    for (auto &pair : nodeMap)
    {
        DirectoryNode *node = pair.first;
        DirTNode *tnode = pair.second;
        std::string label = directoryLabel(node);
        delete[] const_cast<char *>(tnode->text);
        tnode->text = newStr(label.c_str());
    }
    if (outline)
    {
        outline->update();
        outline->drawView();
    }
}

void DirectoryWindow::refreshSort()
{
    if (!root)
        return;

    DirectoryNode *focused = focusedNode();

    auto reorder = [&](auto &&self, DirectoryNode *dir) -> void {
        auto mapIt = nodeMap.find(dir);
        if (mapIt == nodeMap.end())
            return;
        DirTNode *tnode = mapIt->second;
        std::vector<DirectoryNode *> order = orderedChildren(dir);
        DirTNode *firstChild = nullptr;
        DirTNode *prev = nullptr;
        for (DirectoryNode *childDir : order)
        {
            auto childIt = nodeMap.find(childDir);
            if (childIt == nodeMap.end())
                continue;
            DirTNode *childNode = childIt->second;
            childNode->parent = tnode;
            childNode->next = nullptr;
            if (!firstChild)
                firstChild = childNode;
            else
                prev->next = childNode;
            prev = childNode;
        }
        if (tnode)
            tnode->childList = firstChild;
        for (DirectoryNode *childDir : order)
            self(self, childDir);
    };

    reorder(reorder, root.get());

    if (outline)
    {
        outline->syncExpanded();
        outline->update();
        outline->drawView();
        if (focused)
        {
            auto it = nodeMap.find(focused);
            if (it != nodeMap.end())
                outline->focusNode(it->second);
        }
    }
}

DiskUsageApp::DiskUsageApp(const std::vector<std::filesystem::path> &paths,
                           std::shared_ptr<config::OptionRegistry> registry)
    : TProgInit(&DiskUsageApp::initStatusLine, &DiskUsageApp::initMenuBar, &TApplication::initDeskTop),
      TApplication(), optionRegistry(std::move(registry))
{
    unitBaseLabels = {{SizeUnit::Auto, "~A~uto"},
                      {SizeUnit::Bytes, "~B~ytes"},
                      {SizeUnit::Kilobytes, "~K~ilobytes"},
                      {SizeUnit::Megabytes, "~M~egabytes"},
                      {SizeUnit::Gigabytes, "~G~igabytes"},
                      {SizeUnit::Terabytes, "~T~erabytes"},
                      {SizeUnit::Blocks, "B~l~ocks"}};
    sortBaseLabels = {{SortKey::Unsorted, "~U~nsorted"},
                      {SortKey::NameAscending, "~N~ame (A→Z)"},
                      {SortKey::NameDescending, "Name (Z→~A~)"},
                      {SortKey::SizeDescending, "~S~ize (Largest)"},
                      {SortKey::SizeAscending, "Size (S~m~allest)"},
                      {SortKey::ModifiedDescending, "~M~odified (Newest)"},
                      {SortKey::ModifiedAscending, "Modified (~O~ldest)"}};
    std::array<std::pair<SizeUnit, int>, 7> unitOrder = {{
        {SizeUnit::Auto, 0},
        {SizeUnit::Bytes, 1},
        {SizeUnit::Kilobytes, 2},
        {SizeUnit::Megabytes, 3},
        {SizeUnit::Gigabytes, 4},
        {SizeUnit::Terabytes, 5},
        {SizeUnit::Blocks, 6}}};
    for (const auto &[unit, index] : unitOrder)
    {
        if (index >= 0 && index < static_cast<int>(gUnitMenuItems.size()) && gUnitMenuItems[index])
            unitMenuItems[unit] = gUnitMenuItems[index];
    }
    updateUnitMenu();

    std::array<std::pair<SortKey, int>, 7> sortOrder = {{
        {SortKey::Unsorted, 0},
        {SortKey::NameAscending, 1},
        {SortKey::NameDescending, 2},
        {SortKey::SizeDescending, 3},
        {SortKey::SizeAscending, 4},
        {SortKey::ModifiedDescending, 5},
        {SortKey::ModifiedAscending, 6}}};
    for (const auto &[key, index] : sortOrder)
    {
        if (index >= 0 && index < static_cast<int>(gSortMenuItems.size()) && gSortMenuItems[index])
            sortMenuItems[key] = gSortMenuItems[index];
    }
    updateSortMenu();
    symlinkBaseLabels = {"Do ~N~ot Follow Links", "Follow ~C~LI Links", "Follow ~A~ll Links"};
    for (std::size_t i = 0; i < symlinkMenuItems.size(); ++i)
    {
        if (i < gSymlinkMenuItems.size())
            symlinkMenuItems[i] = gSymlinkMenuItems[i];
    }
    hardLinkMenuItem = gHardLinkMenuItem;
    nodumpMenuItem = gNodumpMenuItem;
    errorsMenuItem = gErrorsMenuItem;
    oneFsMenuItem = gOneFsMenuItem;
    ignoreMenuItem = gIgnoreMenuItem;
    thresholdMenuItem = gThresholdMenuItem;

    reloadOptionState();

    for (const auto &path : paths)
        queueDirectoryForScan(path);
}

DiskUsageApp::~DiskUsageApp()
{
    cancelActiveScan(true);
    pendingScanQueue.clear();
}

void DiskUsageApp::handleEvent(TEvent &event)
{
    TApplication::handleEvent(event);
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmOpen:
            promptOpenDirectory();
            break;
        case cmViewFiles:
            viewFiles(false);
            break;
        case cmViewFilesRecursive:
            viewFiles(true);
            break;
        case cmUnitAuto:
            applyUnit(SizeUnit::Auto);
            break;
        case cmUnitBytes:
            applyUnit(SizeUnit::Bytes);
            break;
        case cmUnitKB:
            applyUnit(SizeUnit::Kilobytes);
            break;
        case cmUnitMB:
            applyUnit(SizeUnit::Megabytes);
            break;
        case cmUnitGB:
            applyUnit(SizeUnit::Gigabytes);
            break;
        case cmUnitTB:
            applyUnit(SizeUnit::Terabytes);
            break;
        case cmUnitBlocks:
            applyUnit(SizeUnit::Blocks);
            break;
        case cmSortUnsorted:
            applySortMode(SortKey::Unsorted);
            break;
        case cmSortNameAsc:
            applySortMode(SortKey::NameAscending);
            break;
        case cmSortNameDesc:
            applySortMode(SortKey::NameDescending);
            break;
        case cmSortSizeDesc:
            applySortMode(SortKey::SizeDescending);
            break;
        case cmSortSizeAsc:
            applySortMode(SortKey::SizeAscending);
            break;
        case cmSortModifiedDesc:
            applySortMode(SortKey::ModifiedDescending);
            break;
        case cmSortModifiedAsc:
            applySortMode(SortKey::ModifiedAscending);
            break;
        case cmOptionFollowNever:
            applySymlinkPolicy(BuildDirectoryTreeOptions::SymlinkPolicy::Never);
            break;
        case cmOptionFollowCommandLine:
            applySymlinkPolicy(BuildDirectoryTreeOptions::SymlinkPolicy::CommandLineOnly);
            break;
        case cmOptionFollowAll:
            applySymlinkPolicy(BuildDirectoryTreeOptions::SymlinkPolicy::Always);
            break;
        case cmOptionToggleHardLinks:
            toggleHardLinks();
            break;
        case cmOptionToggleNodump:
            toggleNodump();
            break;
        case cmOptionToggleErrors:
            toggleErrors();
            break;
        case cmOptionToggleOneFs:
            toggleOneFilesystem();
            break;
        case cmOptionEditIgnores:
            editIgnorePatterns();
            break;
        case cmOptionEditThreshold:
            editThreshold();
            break;
        case cmOptionLoad:
            loadOptionsFromFile();
            break;
        case cmOptionSave:
            saveOptionsToFile();
            break;
        case cmOptionSaveDefaults:
            saveDefaultOptions();
            break;
        case cmAbout:
        {
            ck::ui::showAboutDialog("ck-du", CK_DU_VERSION, kAboutDescription);
            break;
        }
        default:
            return;
        }
        clearEvent(event);
    }
}

void DiskUsageApp::idle()
{
    TApplication::idle();
    processRescanRequests();
    if (activeScan)
    {
        updateScanProgress(*activeScan);
        if (activeScan->finished.load())
            processActiveScanCompletion();
    }
    else if (!pendingScanQueue.empty())
        startNextQueuedDirectory();
}

TMenuBar *DiskUsageApp::initMenuBar(TRect r)
{
    r.b.y = r.a.y + 1;
    auto *unitAuto = new TMenuItem("~A~uto", cmUnitAuto, kbNoKey, hcNoContext);
    auto *unitBytes = new TMenuItem("~B~ytes", cmUnitBytes, kbNoKey, hcNoContext);
    auto *unitKB = new TMenuItem("~K~ilobytes", cmUnitKB, kbNoKey, hcNoContext);
    auto *unitMB = new TMenuItem("~M~egabytes", cmUnitMB, kbNoKey, hcNoContext);
    auto *unitGB = new TMenuItem("~G~igabytes", cmUnitGB, kbNoKey, hcNoContext);
    auto *unitTB = new TMenuItem("~T~erabytes", cmUnitTB, kbNoKey, hcNoContext);
    auto *unitBlocks = new TMenuItem("B~l~ocks", cmUnitBlocks, kbNoKey, hcNoContext);
    gUnitMenuItems = {unitAuto, unitBytes, unitKB, unitMB, unitGB, unitTB, unitBlocks};
    auto *sortUnsorted = new TMenuItem("~U~nsorted", cmSortUnsorted, kbNoKey, hcNoContext);
    auto *sortNameAsc = new TMenuItem("~N~ame (A→Z)", cmSortNameAsc, kbNoKey, hcNoContext);
    auto *sortNameDesc = new TMenuItem("Name (Z→~A~)", cmSortNameDesc, kbNoKey, hcNoContext);
    auto *sortSizeDesc = new TMenuItem("~S~ize (Largest)", cmSortSizeDesc, kbNoKey, hcNoContext);
    auto *sortSizeAsc = new TMenuItem("Size (S~m~allest)", cmSortSizeAsc, kbNoKey, hcNoContext);
    auto *sortModifiedDesc = new TMenuItem("~M~odified (Newest)", cmSortModifiedDesc, kbNoKey, hcNoContext);
    auto *sortModifiedAsc = new TMenuItem("Modified (~O~ldest)", cmSortModifiedAsc, kbNoKey, hcNoContext);
    gSortMenuItems = {sortUnsorted, sortNameAsc, sortNameDesc, sortSizeDesc, sortSizeAsc, sortModifiedDesc, sortModifiedAsc};
    auto *followNever = new TMenuItem("Do ~N~ot Follow Links", cmOptionFollowNever, kbNoKey, hcNoContext);
    auto *followCommand = new TMenuItem("Follow ~C~LI Links", cmOptionFollowCommandLine, kbNoKey, hcNoContext);
    auto *followAll = new TMenuItem("Follow ~A~ll Links", cmOptionFollowAll, kbNoKey, hcNoContext);
    gSymlinkMenuItems = {followNever, followCommand, followAll};
    auto *hardLinks = new TMenuItem("Count ~H~ard Links Multiple Times", cmOptionToggleHardLinks, kbNoKey, hcNoContext);
    auto *nodump = new TMenuItem("Ignore ~N~odump Flag", cmOptionToggleNodump, kbNoKey, hcNoContext);
    auto *errors = new TMenuItem("Report ~E~rrors", cmOptionToggleErrors, kbNoKey, hcNoContext);
    auto *oneFs = new TMenuItem("Stay on One ~F~ile System", cmOptionToggleOneFs, kbNoKey, hcNoContext);
    auto *ignore = new TMenuItem("Ignore ~P~atterns...", cmOptionEditIgnores, kbNoKey, hcNoContext);
    auto *threshold = new TMenuItem("Size ~T~hreshold...", cmOptionEditThreshold, kbNoKey, hcNoContext);
    gHardLinkMenuItem = hardLinks;
    gNodumpMenuItem = nodump;
    gErrorsMenuItem = errors;
    gOneFsMenuItem = oneFs;
    gIgnoreMenuItem = ignore;
    gThresholdMenuItem = threshold;
    auto *loadOptions = new TMenuItem("~L~oad Options...", cmOptionLoad, kbNoKey, hcNoContext);
    auto *saveOptions = new TMenuItem("~S~ave Options...", cmOptionSave, kbNoKey, hcNoContext);
    auto *saveDefaults = new TMenuItem("Save ~D~efaults", cmOptionSaveDefaults, kbNoKey, hcNoContext);
    return new TMenuBar(r,
                        *new TSubMenu("~F~ile", hcNoContext) +
                            *new TMenuItem("~O~pen Directory", cmOpen, kbF2, hcOpen, "F2") +
                            *new TMenuItem("~C~lose", cmClose, kbF4, hcClose, "F4") +
                            newLine() +
                            *new TMenuItem("E~x~it", cmQuit, kbAltX, hcExit, "Alt-X") +
                        *new TSubMenu("~S~ort", hcNoContext) +
                            *sortUnsorted +
                            *sortNameAsc +
                            *sortNameDesc +
                            *sortSizeDesc +
                            *sortSizeAsc +
                            *sortModifiedDesc +
                            *sortModifiedAsc +
                        *new TSubMenu("~U~nits", hcNoContext) +
                            *unitAuto +
                            *unitBytes +
                            *unitKB +
                            *unitMB +
                            *unitGB +
                            *unitTB +
                            *unitBlocks +
                        *new TSubMenu("Op~t~ions", hcNoContext) +
                            *followNever +
                            *followCommand +
                            *followAll +
                            newLine() +
                            *hardLinks +
                            *nodump +
                            *errors +
                            *oneFs +
                            *ignore +
                            *threshold +
                            newLine() +
                            *loadOptions +
                            *saveOptions +
                            *saveDefaults +
                        *new TSubMenu("~V~iew", hcNoContext) +
                            *new TMenuItem("~F~iles", cmViewFiles, kbF3, hcNoContext, "F3") +
                            *new TMenuItem("Files (~R~ecursive)", cmViewFilesRecursive, kbShiftF3, hcNoContext, "Shift-F3") +
                        *new TSubMenu("~H~elp", hcNoContext) +
                            *new TMenuItem("~A~bout", cmAbout, kbF1, hcNoContext, "F1"));
}

TStatusLine *DiskUsageApp::initStatusLine(TRect r)
{
    r.a.y = r.b.y - 1;
    return new DiskUsageStatusLine(r);
}

void DiskUsageApp::promptOpenDirectory()
{
    struct DialogData
    {
        char path[PATH_MAX];
    } data{};

    std::string current = std::filesystem::current_path().string();
    std::snprintf(data.path, sizeof(data.path), "%s", current.c_str());

    TDialog *d = new TDialog(TRect(0, 0, 60, 10), "Open Directory");
    d->options |= ofCentered;
    auto *input = new TInputLine(TRect(3, 3, 55, 4), sizeof(data.path) - 1);
    d->insert(input);
    d->insert(new TLabel(TRect(2, 2, 20, 3), "~P~ath:", input));
    d->insert(new TButton(TRect(15, 6, 25, 8), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(27, 6, 37, 8), "Cancel", cmCancel, bfNormal));

    if (TProgram::application->executeDialog(d, &data) != cmCancel)
        openDirectory(data.path);
}

void DiskUsageApp::openDirectory(const std::filesystem::path &path)
{
    requestDirectoryScan(path, false);
}

void DiskUsageApp::viewFiles(bool recursive)
{
    DirectoryWindow *window = activeDirectoryWindow();
    if (!window)
    {
        messageBox("No directory window active", mfError | mfOKButton);
        return;
    }
    DirectoryNode *node = window->focusedNode();
    if (!node)
    {
        messageBox("No directory selected", mfError | mfOKButton);
        return;
    }

    BuildDirectoryTreeOptions listOptions = makeScanOptions(window->scanOptions());
    std::vector<FileEntry> files = listFiles(node->path, recursive, listOptions);
    std::string title = node->path.filename().empty() ? node->path.string() : node->path.filename().string();
    if (title.empty())
        title = node->path.string();
    title += recursive ? " (files + subdirs)" : " (files)";

    auto *win = new FileListWindow(title, std::move(files), recursive, *this);
    deskTop->insert(win);
    win->drawView();
}

void DiskUsageApp::requestDirectoryScan(const std::filesystem::path &path, bool allowQueue)
{
    processActiveScanCompletion();

    std::filesystem::path absolute = std::filesystem::absolute(path);
    std::error_code ec;
    if (!std::filesystem::exists(absolute, ec) || !std::filesystem::is_directory(absolute, ec))
    {
        std::string message = "Path is not a directory:\n" + absolute.string();
        messageBox(message.c_str(), mfError | mfOKButton);
        return;
    }

    if (activeScan && !activeScan->finished.load())
    {
        if (allowQueue)
            pendingScanQueue.push_back(absolute);
        else
            messageBox("A directory scan is already in progress", mfInformation | mfOKButton);
        return;
    }

    startDirectoryScan(absolute);
}

void DiskUsageApp::queueDirectoryForScan(const std::filesystem::path &path)
{
    requestDirectoryScan(path, true);
}

void DiskUsageApp::startDirectoryScan(const std::filesystem::path &path)
{
    auto task = std::make_unique<DirectoryScanTask>();
    task->rootPath = path;
    task->currentPath = path.string();
    task->optionState = currentOptions;
    task->scanOptions = makeScanOptions(task->optionState);
    task->errors.clear();

    auto *dialog = new ScanProgressDialog();
    dialog->setCancelHandler([this]() { requestScanCancellation(); });
    task->dialog = dialog;
    deskTop->insert(dialog);
    dialog->drawView();
    dialog->updatePath(task->currentPath);

    DirectoryScanTask *rawTask = task.get();
    rawTask->worker = std::thread([this, rawTask]() { runDirectoryScan(*rawTask); });

    activeScan = std::move(task);
}

void DiskUsageApp::startNextQueuedDirectory()
{
    if (activeScan || pendingScanQueue.empty())
        return;

    std::filesystem::path next = pendingScanQueue.front();
    pendingScanQueue.pop_front();
    startDirectoryScan(next);
}

void DiskUsageApp::updateScanProgress(DirectoryScanTask &task)
{
    if (!task.dialog)
        return;

    std::string currentPath;
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        currentPath = task.currentPath;
    }

    task.dialog->updatePath(currentPath);
}

void DiskUsageApp::processActiveScanCompletion()
{
    if (!activeScan || !activeScan->finished.load())
        return;

    if (activeScan->worker.joinable())
        activeScan->worker.join();

    std::unique_ptr<DirectoryNode> result;
    bool cancelled = false;
    bool failed = false;
    std::string errorMessage;
    DuOptions optionState = currentOptions;
    std::vector<std::string> errors;
    std::filesystem::path rootPath = activeScan->rootPath;
    {
        std::lock_guard<std::mutex> lock(activeScan->mutex);
        result = std::move(activeScan->result);
        cancelled = activeScan->cancelled;
        failed = activeScan->failed;
        errorMessage = activeScan->errorMessage;
        optionState = activeScan->optionState;
        errors = activeScan->errors;
    }

    closeProgressDialog(*activeScan);

    activeScan.reset();

    if (failed)
    {
        std::string message = errorMessage.empty() ? std::string("Failed to read directory") : errorMessage;
        messageBox(message.c_str(), mfError | mfOKButton);
    }
    else if (!cancelled && result)
    {
        auto *win = new DirectoryWindow(rootPath, std::move(result), optionState, *this);
        deskTop->insert(win);
        win->drawView();
        if (optionState.reportErrors && !errors.empty())
        {
            std::string message = "Some entries could not be read:\n";
            std::size_t count = std::min<std::size_t>(errors.size(), 10);
            for (std::size_t i = 0; i < count; ++i)
                message += " - " + errors[i] + "\n";
            if (errors.size() > count)
                message += "... (" + std::to_string(errors.size() - count) + " more)";
            messageBox(message.c_str(), mfWarning | mfOKButton);
        }
    }

    startNextQueuedDirectory();
}

void DiskUsageApp::runDirectoryScan(DirectoryScanTask &task)
{
    BuildDirectoryTreeOptions options = task.scanOptions;
    options.progressCallback = [&](const std::filesystem::path &current) {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.currentPath = current.string();
    };
    options.cancelRequested = [&]() -> bool { return task.cancelRequested.load(); };
    if (options.reportErrors)
    {
        options.errorCallback = [&](const std::filesystem::path &p, const std::error_code &ec) {
            std::lock_guard<std::mutex> lock(task.mutex);
            if (task.errors.size() < 200)
            {
                std::string message = p.empty() ? std::string("(unknown)") : p.string();
                if (!ec.message().empty())
                    message += ": " + ec.message();
                task.errors.push_back(std::move(message));
            }
        };
    }

    BuildDirectoryTreeResult result;
    try
    {
        result = buildDirectoryTree(task.rootPath, options);
    }
    catch (const std::exception &ex)
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.failed = true;
        task.errorMessage = ex.what();
        task.finished.store(true);
        return;
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.failed = true;
        task.errorMessage = "Unknown error";
        task.finished.store(true);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.cancelled = result.cancelled;
        task.result = std::move(result.root);
    }

    task.finished.store(true);
}

void DiskUsageApp::requestScanCancellation()
{
    if (!activeScan)
        return;
    activeScan->cancelRequested.store(true);
    closeProgressDialog(*activeScan);
}

void DiskUsageApp::closeProgressDialog(DirectoryScanTask &task)
{
    if (!task.dialog)
        return;

    TDialog *dialog = task.dialog;
    task.dialog = nullptr;
    if (dialog->owner)
        dialog->close();
    else
    {
        dialog->shutDown();
        delete dialog;
    }
}

void DiskUsageApp::cancelActiveScan(bool waitForCompletion)
{
    if (!activeScan)
        return;

    activeScan->cancelRequested.store(true);
    if (waitForCompletion && activeScan->worker.joinable())
        activeScan->worker.join();

    closeProgressDialog(*activeScan);
    activeScan.reset();
}

DirectoryWindow *DiskUsageApp::activeDirectoryWindow() const
{
    if (!deskTop)
        return nullptr;
    TView *current = deskTop->current;
    while (current && current->owner != deskTop)
        current = current->owner;
    return dynamic_cast<DirectoryWindow *>(current);
}

void DiskUsageApp::registerDirectoryWindow(DirectoryWindow *window)
{
    directoryWindows.push_back(window);
}

void DiskUsageApp::unregisterDirectoryWindow(DirectoryWindow *window)
{
    directoryWindows.erase(std::remove(directoryWindows.begin(), directoryWindows.end(), window), directoryWindows.end());
}

void DiskUsageApp::registerFileWindow(FileListWindow *window)
{
    fileWindows.push_back(window);
}

void DiskUsageApp::unregisterFileWindow(FileListWindow *window)
{
    fileWindows.erase(std::remove(fileWindows.begin(), fileWindows.end(), window), fileWindows.end());
}

void DiskUsageApp::notifyUnitsChanged()
{
    for (auto *win : directoryWindows)
        if (win)
            win->refreshLabels();
    for (auto *win : fileWindows)
        if (win)
            win->refreshUnits();
}

void DiskUsageApp::notifySortChanged()
{
    for (auto *win : directoryWindows)
        if (win)
            win->refreshSort();
    for (auto *win : fileWindows)
        if (win)
            win->refreshSort();
}

void DiskUsageApp::updateUnitMenu()
{
    SizeUnit current = getCurrentUnit();
    for (auto &pair : unitMenuItems)
    {
        SizeUnit unit = pair.first;
        TMenuItem *item = pair.second;
        if (!item)
            continue;
        auto baseIt = unitBaseLabels.find(unit);
        std::string base = (baseIt != unitBaseLabels.end()) ? baseIt->second : unitName(unit);
        std::string label = std::string(unit == current ? "● " : "  ") + base;
        delete[] const_cast<char *>(item->name);
        item->name = newStr(label.c_str());
    }
    if (menuBar)
        menuBar->drawView();
}

void DiskUsageApp::applyUnit(SizeUnit unit)
{
    if (getCurrentUnit() == unit)
        return;
    setCurrentUnit(unit);
    updateUnitMenu();
    notifyUnitsChanged();
}

void DiskUsageApp::updateSortMenu()
{
    SortKey current = getCurrentSortKey();
    for (auto &pair : sortMenuItems)
    {
        SortKey key = pair.first;
        TMenuItem *item = pair.second;
        if (!item)
            continue;
        auto baseIt = sortBaseLabels.find(key);
        std::string base = (baseIt != sortBaseLabels.end()) ? baseIt->second : sortKeyName(key);
        std::string label = std::string(key == current ? "● " : "  ") + base;
        delete[] const_cast<char *>(item->name);
        item->name = newStr(label.c_str());
    }
    if (menuBar)
        menuBar->drawView();
}

void DiskUsageApp::applySortMode(SortKey key)
{
    if (getCurrentSortKey() == key)
        return;
    setCurrentSortKey(key);
    updateSortMenu();
    notifySortChanged();
}

void DiskUsageApp::updateToggleMenuItem(TMenuItem *item, bool enabled, const std::string &baseLabel)
{
    if (!item)
        return;
    std::string label = std::string(enabled ? "[x] " : "[ ] ") + baseLabel;
    delete[] const_cast<char *>(item->name);
    item->name = newStr(label.c_str());
}

void DiskUsageApp::updateSymlinkMenu()
{
    int activeIndex = 0;
    switch (currentOptions.symlinkPolicy)
    {
    case BuildDirectoryTreeOptions::SymlinkPolicy::CommandLineOnly:
        activeIndex = 1;
        break;
    case BuildDirectoryTreeOptions::SymlinkPolicy::Always:
        activeIndex = 2;
        break;
    case BuildDirectoryTreeOptions::SymlinkPolicy::Never:
    default:
        activeIndex = 0;
        break;
    }
    for (std::size_t i = 0; i < symlinkMenuItems.size(); ++i)
    {
        TMenuItem *item = symlinkMenuItems[i];
        if (!item)
            continue;
        std::string label = std::string(static_cast<int>(i) == activeIndex ? "● " : "  ") + symlinkBaseLabels[i];
        delete[] const_cast<char *>(item->name);
        item->name = newStr(label.c_str());
    }
}

void DiskUsageApp::updateOptionsMenu()

{
    updateSymlinkMenu();
    updateToggleMenuItem(hardLinkMenuItem, currentOptions.countHardLinksMultipleTimes, hardLinkBaseLabel);
    updateToggleMenuItem(nodumpMenuItem, currentOptions.ignoreNodump, nodumpBaseLabel);
    updateToggleMenuItem(errorsMenuItem, currentOptions.reportErrors, errorsBaseLabel);
    updateToggleMenuItem(oneFsMenuItem, currentOptions.stayOnFilesystem, oneFsBaseLabel);
    if (ignoreMenuItem)
    {
        std::string label = ignoreMenuLabel(currentOptions);
        delete[] const_cast<char *>(ignoreMenuItem->name);
        ignoreMenuItem->name = newStr(label.c_str());
    }
    if (thresholdMenuItem)
    {
        std::string label = formatThresholdLabel(currentOptions.threshold);
        delete[] const_cast<char *>(thresholdMenuItem->name);
        thresholdMenuItem->name = newStr(label.c_str());
    }
    if (menuBar)
        menuBar->drawView();
}

void DiskUsageApp::optionsChanged(bool triggerRescan)
{
    updateOptionsMenu();
    if (triggerRescan)
    {
        requestRescanAllDirectories();
        processRescanRequests();
    }
}

void DiskUsageApp::requestRescanAllDirectories()
{
    if (directoryWindows.empty())
        return;
    rescanRequested = true;
}

void DiskUsageApp::processRescanRequests()
{
    if (!rescanRequested || rescanInProgress)
        return;
    rescanInProgress = true;
    rescanRequested = false;
    performRescanAllDirectories();
    rescanInProgress = false;
}

void DiskUsageApp::performRescanAllDirectories()
{
    std::vector<std::filesystem::path> paths;
    paths.reserve(directoryWindows.size());
    for (auto *window : directoryWindows)
    {
        if (window)
            paths.push_back(window->rootPath());
    }
    if (paths.empty())
        return;

    cancelActiveScan(true);
    pendingScanQueue.clear();

    std::vector<FileListWindow *> fileCopies = fileWindows;
    for (auto *fileWin : fileCopies)
        if (fileWin && fileWin->owner)
            fileWin->close();

    std::vector<DirectoryWindow *> dirCopies = directoryWindows;
    for (auto *dirWin : dirCopies)
        if (dirWin && dirWin->owner)
            dirWin->close();

    for (const auto &path : paths)
        queueDirectoryForScan(path);
}

void DiskUsageApp::applySymlinkPolicy(BuildDirectoryTreeOptions::SymlinkPolicy policy)
{
    if (currentOptions.symlinkPolicy == policy)
        return;
    currentOptions.symlinkPolicy = policy;
    currentOptions.followCommandLineSymlinks = policy != BuildDirectoryTreeOptions::SymlinkPolicy::Never;
    if (optionRegistry)
        optionRegistry->set(kOptionSymlinkPolicy, config::OptionValue(policyToString(policy)));
    optionsChanged(true);
}

void DiskUsageApp::toggleHardLinks()
{
    currentOptions.countHardLinksMultipleTimes = !currentOptions.countHardLinksMultipleTimes;
    if (optionRegistry)
        optionRegistry->set(kOptionHardLinks, config::OptionValue(currentOptions.countHardLinksMultipleTimes));
    optionsChanged(true);
}

void DiskUsageApp::toggleNodump()
{
    currentOptions.ignoreNodump = !currentOptions.ignoreNodump;
    if (optionRegistry)
        optionRegistry->set(kOptionIgnoreNodump, config::OptionValue(currentOptions.ignoreNodump));
    optionsChanged(true);
}

void DiskUsageApp::toggleErrors()
{
    currentOptions.reportErrors = !currentOptions.reportErrors;
    if (optionRegistry)
        optionRegistry->set(kOptionReportErrors, config::OptionValue(currentOptions.reportErrors));
    optionsChanged(true);
}

void DiskUsageApp::toggleOneFilesystem()
{
    currentOptions.stayOnFilesystem = !currentOptions.stayOnFilesystem;
    if (optionRegistry)
        optionRegistry->set(kOptionStayOnFilesystem, config::OptionValue(currentOptions.stayOnFilesystem));
    optionsChanged(true);
}

void DiskUsageApp::editIgnorePatterns()
{
    auto *dialog = new PatternEditorDialog(currentOptions.ignorePatterns);
    if (TProgram::application->executeDialog(dialog, nullptr) != cmOK)
        return;

    std::vector<std::string> patterns = dialog->result();
    currentOptions.ignorePatterns = patterns;
    if (optionRegistry)
        optionRegistry->set(kOptionIgnorePatterns, config::OptionValue(patterns));
    optionsChanged(true);
}

void DiskUsageApp::editThreshold()
{
    struct DialogData
    {
        char value[64];
    } data{};

    if (currentOptions.threshold != 0)
        std::snprintf(data.value, sizeof(data.value), "%lld",
                      static_cast<long long>(currentOptions.threshold));
    else
        data.value[0] = '\0';

    TDialog *d = new TDialog(TRect(0, 0, 60, 12), "Size Threshold");
    d->options |= ofCentered;
    auto *input = new TInputLine(TRect(3, 5, 55, 6), sizeof(data.value) - 1);
    d->insert(new TStaticText(TRect(2, 2, 58, 4),
                               "Enter a byte value (supports K, M, G, T suffix)."
                               " Use a leading '-' to match entries below the value."));
    d->insert(new TLabel(TRect(2, 4, 20, 5), "~T~hreshold:", input));
    d->insert(input);
    d->insert(new TButton(TRect(15, 8, 25, 10), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(27, 8, 37, 10), "Cancel", cmCancel, bfNormal));

    if (TProgram::application->executeDialog(d, &data) != cmOK)
        return;

    std::optional<std::int64_t> parsed = parseThresholdValue(data.value);
    if (!parsed)
    {
        messageBox("Invalid threshold value", mfError | mfOKButton);
        return;
    }

    currentOptions.threshold = *parsed;
    if (optionRegistry)
        optionRegistry->set(kOptionThreshold, config::OptionValue(currentOptions.threshold));
    optionsChanged(true);
}

void DiskUsageApp::loadOptionsFromFile()
{
    if (!optionRegistry)
        return;
    struct DialogData
    {
        char path[PATH_MAX];
    } data{};

    std::filesystem::path configPath = config::OptionRegistry::configRoot();
    std::snprintf(data.path, sizeof(data.path), "%s", configPath.string().c_str());

    TDialog *d = new TDialog(TRect(0, 0, 68, 10), "Load Options");
    d->options |= ofCentered;
    auto *input = new TInputLine(TRect(3, 4, 64, 5), sizeof(data.path) - 1);
    d->insert(new TLabel(TRect(2, 3, 20, 4), "~F~ile:", input));
    d->insert(input);
    d->insert(new TButton(TRect(18, 6, 28, 8), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(30, 6, 40, 8), "Cancel", cmCancel, bfNormal));

    if (TProgram::application->executeDialog(d, &data) != cmOK)
        return;

    std::filesystem::path path = data.path;
    if (!optionRegistry->loadFromFile(path))
    {
        std::string message = "Failed to load options:\n" + path.string();
        messageBox(message.c_str(), mfError | mfOKButton);
        return;
    }
    reloadOptionState();
    std::string success = "Options loaded from:\n" + path.string();
    messageBox(success.c_str(), mfInformation | mfOKButton);
}

void DiskUsageApp::saveOptionsToFile()
{
    if (!optionRegistry)
        return;
    struct DialogData
    {
        char path[PATH_MAX];
    } data{};

    std::filesystem::path configPath = config::OptionRegistry::configRoot() / "options.json";
    std::snprintf(data.path, sizeof(data.path), "%s", configPath.string().c_str());

    TDialog *d = new TDialog(TRect(0, 0, 68, 10), "Save Options");
    d->options |= ofCentered;
    auto *input = new TInputLine(TRect(3, 4, 64, 5), sizeof(data.path) - 1);
    d->insert(new TLabel(TRect(2, 3, 20, 4), "~F~ile:", input));
    d->insert(input);
    d->insert(new TButton(TRect(18, 6, 28, 8), "O~K~", cmOK, bfDefault));
    d->insert(new TButton(TRect(30, 6, 40, 8), "Cancel", cmCancel, bfNormal));

    if (TProgram::application->executeDialog(d, &data) != cmOK)
        return;

    std::filesystem::path path = data.path;
    if (!optionRegistry->saveToFile(path))
    {
        std::string message = "Failed to save options:\n" + path.string();
        messageBox(message.c_str(), mfError | mfOKButton);
        return;
    }
    std::string success = "Options saved to:\n" + path.string();
    messageBox(success.c_str(), mfInformation | mfOKButton);
}

void DiskUsageApp::saveDefaultOptions()
{
    if (!optionRegistry)
        return;
    std::filesystem::path dest = optionRegistry->defaultOptionsPath();
    if (optionRegistry->saveDefaults())
    {
        std::string message = "Defaults saved to:\n" + dest.string();
        messageBox(message.c_str(), mfInformation | mfOKButton);
    }
    else
    {
        std::string message = "Failed to save defaults:\n" + dest.string();
        messageBox(message.c_str(), mfError | mfOKButton);
    }
}

void DiskUsageApp::reloadOptionState()
{
    if (!optionRegistry)
        return;
    currentOptions = optionsFromRegistry(*optionRegistry);
    optionsChanged(false);
}

int main(int argc, char **argv)
{
    auto registry = std::make_shared<config::OptionRegistry>("ck-du");
    registerDiskUsageOptions(*registry);

    bool loadDefaults = true;
    bool forceReloadDefaults = false;
    std::vector<std::filesystem::path> optionFiles;
    std::vector<std::string> cliIgnorePatterns;
    std::optional<BuildDirectoryTreeOptions::SymlinkPolicy> symlinkOverride;
    std::optional<bool> hardLinksOverride;
    std::optional<bool> nodumpOverride;
    std::optional<bool> errorsOverride;
    std::optional<bool> oneFsOverride;
    std::optional<std::int64_t> thresholdOverride;
    std::vector<std::filesystem::path> directories;

    auto printUsage = []() {
        std::cout << "Usage: ck-du [options] [paths...]\n"
                  << "  -H             Follow symlinks listed on the command line only\n"
                  << "  -L             Follow all symbolic links\n"
                  << "  -P             Do not follow symbolic links\n"
                  << "  -l             Count hard links multiple times\n"
                  << "  -n             Ignore entries with the nodump flag\n"
                  << "  -r             Report read errors (default)\n"
                  << "  -q             Suppress read error warnings\n"
                  << "  -t N           Apply size threshold N (supports K/M/G/T suffix)\n"
                  << "  -I PATTERN     Ignore entries matching PATTERN\n"
                  << "  -x             Stay on a single file system\n"
                  << "  --load-options FILE    Load options from FILE\n"
                  << "  --no-default-options   Do not load saved defaults\n"
                  << "  --default-options      Load saved defaults after parsing flags\n"
                  << std::endl;
    };

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return 0;
        }
        else if (arg == "--no-default-options")
        {
            loadDefaults = false;
        }
        else if (arg == "--default-options")
        {
            forceReloadDefaults = true;
        }
        else if (arg.rfind("--load-options", 0) == 0)
        {
            std::string value;
            const std::string prefix = "--load-options=";
            if (arg == "--load-options")
            {
                if (i + 1 >= argc)
                {
                    std::cerr << "ck-du: --load-options requires a file path" << std::endl;
                    return 1;
                }
                value = argv[++i];
            }
            else if (arg.size() > prefix.size())
            {
                value = arg.substr(prefix.size());
            }
            else
            {
                std::cerr << "ck-du: invalid --load-options usage" << std::endl;
                return 1;
            }
            optionFiles.emplace_back(value);
        }
        else if (!arg.empty() && arg[0] == '-' && arg.size() > 1)
        {
            for (std::size_t j = 1; j < arg.size(); ++j)
            {
                char opt = arg[j];
                switch (opt)
                {
                case 'H':
                    symlinkOverride = BuildDirectoryTreeOptions::SymlinkPolicy::CommandLineOnly;
                    break;
                case 'L':
                    symlinkOverride = BuildDirectoryTreeOptions::SymlinkPolicy::Always;
                    break;
                case 'P':
                    symlinkOverride = BuildDirectoryTreeOptions::SymlinkPolicy::Never;
                    break;
                case 'l':
                    hardLinksOverride = true;
                    break;
                case 'n':
                    nodumpOverride = true;
                    break;
                case 'r':
                    errorsOverride = true;
                    break;
                case 'q':
                    errorsOverride = false;
                    break;
                case 'x':
                    oneFsOverride = true;
                    break;
                case 'I':
                {
                    std::string pattern;
                    if (j + 1 < arg.size())
                    {
                        pattern = arg.substr(j + 1);
                        j = arg.size();
                    }
                    else
                    {
                        if (i + 1 >= argc)
                        {
                            std::cerr << "ck-du: -I requires a pattern" << std::endl;
                            return 1;
                        }
                        pattern = argv[++i];
                    }
                    cliIgnorePatterns.push_back(pattern);
                    break;
                }
                case 't':
                {
                    std::string value;
                    if (j + 1 < arg.size())
                    {
                        value = arg.substr(j + 1);
                        j = arg.size();
                    }
                    else
                    {
                        if (i + 1 >= argc)
                        {
                            std::cerr << "ck-du: -t requires a value" << std::endl;
                            return 1;
                        }
                        value = argv[++i];
                    }
                    auto parsed = parseThresholdValue(value);
                    if (!parsed)
                    {
                        std::cerr << "ck-du: invalid threshold value '" << value << "'" << std::endl;
                        return 1;
                    }
                    thresholdOverride = *parsed;
                    break;
                }
                case '-':
                    std::cerr << "ck-du: unknown option '" << arg << "'" << std::endl;
                    return 1;
                default:
                    std::cerr << "ck-du: unknown option '-" << opt << "'" << std::endl;
                    return 1;
                }
                if (opt == 'I' || opt == 't')
                    break;
            }
        }
        else
        {
            directories.emplace_back(arg);
        }
    }

    if (loadDefaults)
        registry->loadDefaults();
    if (forceReloadDefaults)
        registry->loadDefaults();
    for (const auto &file : optionFiles)
    {
        if (!registry->loadFromFile(file))
        {
            std::cerr << "ck-du: failed to load options from '" << file.string() << "'" << std::endl;
            return 1;
        }
    }

    DuOptions options = optionsFromRegistry(*registry);
    if (symlinkOverride)
    {
        options.symlinkPolicy = *symlinkOverride;
        options.followCommandLineSymlinks = options.symlinkPolicy != BuildDirectoryTreeOptions::SymlinkPolicy::Never;
    }
    if (hardLinksOverride)
        options.countHardLinksMultipleTimes = *hardLinksOverride;
    if (nodumpOverride)
        options.ignoreNodump = *nodumpOverride;
    if (errorsOverride)
        options.reportErrors = *errorsOverride;
    if (oneFsOverride)
        options.stayOnFilesystem = *oneFsOverride;
    if (thresholdOverride)
        options.threshold = *thresholdOverride;
    for (const auto &pattern : cliIgnorePatterns)
        options.ignorePatterns.push_back(pattern);

    registry->set(kOptionSymlinkPolicy, config::OptionValue(policyToString(options.symlinkPolicy)));
    registry->set(kOptionHardLinks, config::OptionValue(options.countHardLinksMultipleTimes));
    registry->set(kOptionIgnoreNodump, config::OptionValue(options.ignoreNodump));
    registry->set(kOptionReportErrors, config::OptionValue(options.reportErrors));
    registry->set(kOptionThreshold, config::OptionValue(options.threshold));
    registry->set(kOptionStayOnFilesystem, config::OptionValue(options.stayOnFilesystem));
    registry->set(kOptionIgnorePatterns, config::OptionValue(options.ignorePatterns));

    DiskUsageApp app(directories, registry);
    app.run();
    return 0;
}

