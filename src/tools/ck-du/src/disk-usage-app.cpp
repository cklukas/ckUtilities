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
#define Uses_TClipboard
#define Uses_TOutline
#define Uses_TScrollBar
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#define Uses_TWindow
#define Uses_TCommandSet
#define Uses_MsgBox
#include <tvision/tv.h>

#include "ck/about_dialog.hpp"
#include "ck/app_info.hpp"
#include "ck/commands/ck_du.hpp"
#include "ck/hotkeys.hpp"
#include "ck/launcher.hpp"
#include "ck/options.hpp"
#include "ck/ui/clock_aware_application.hpp"
#include "ck/ui/clock_view.hpp"
#include "ck/ui/status_line.hpp"
#include "ck/ui/window_menu.hpp"
#include "cloud_actions.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ck::du;
namespace config = ck::config;
namespace commands = ck::commands::disk_usage;

static constexpr std::string_view kToolId = "ck-du";

static const ck::appinfo::ToolInfo &toolInfo()
{
    return ck::appinfo::requireTool(kToolId);
}

static constexpr unsigned short cmViewFiles = commands::ViewFiles;
static constexpr unsigned short cmViewFilesRecursive = commands::ViewFilesRecursive;
static constexpr unsigned short cmViewFileTypes = commands::ViewFileTypes;
static constexpr unsigned short cmViewFileTypesRecursive = commands::ViewFileTypesRecursive;
static constexpr unsigned short cmViewFilesForType = commands::ViewFilesForType;
static constexpr unsigned short cmCopyPath = commands::CopyPath;
static constexpr unsigned short cmAbout = commands::About;
static constexpr unsigned short cmUnitAuto = commands::UnitAuto;
static constexpr unsigned short cmUnitBytes = commands::UnitBytes;
static constexpr unsigned short cmUnitKB = commands::UnitKB;
static constexpr unsigned short cmUnitMB = commands::UnitMB;
static constexpr unsigned short cmUnitGB = commands::UnitGB;
static constexpr unsigned short cmUnitTB = commands::UnitTB;
static constexpr unsigned short cmUnitBlocks = commands::UnitBlocks;
static constexpr unsigned short cmSortUnsorted = commands::SortUnsorted;
static constexpr unsigned short cmSortNameAsc = commands::SortNameAsc;
static constexpr unsigned short cmSortNameDesc = commands::SortNameDesc;
static constexpr unsigned short cmSortSizeDesc = commands::SortSizeDesc;
static constexpr unsigned short cmSortSizeAsc = commands::SortSizeAsc;
static constexpr unsigned short cmSortModifiedDesc = commands::SortModifiedDesc;
static constexpr unsigned short cmSortModifiedAsc = commands::SortModifiedAsc;
static constexpr unsigned short cmOptionFollowNever = commands::OptionFollowNever;
static constexpr unsigned short cmOptionFollowCommandLine = commands::OptionFollowCommandLine;
static constexpr unsigned short cmOptionFollowAll = commands::OptionFollowAll;
static constexpr unsigned short cmOptionToggleHardLinks = commands::OptionToggleHardLinks;
static constexpr unsigned short cmOptionToggleNodump = commands::OptionToggleNodump;
static constexpr unsigned short cmOptionToggleErrors = commands::OptionToggleErrors;
static constexpr unsigned short cmOptionToggleOneFs = commands::OptionToggleOneFs;
static constexpr unsigned short cmOptionEditIgnores = commands::OptionEditIgnores;
static constexpr unsigned short cmOptionEditThreshold = commands::OptionEditThreshold;
static constexpr unsigned short cmOptionLoad = commands::OptionLoad;
static constexpr unsigned short cmOptionSave = commands::OptionSave;
static constexpr unsigned short cmOptionSaveDefaults = commands::OptionSaveDefaults;
static constexpr unsigned short cmPatternAdd = commands::PatternAdd;
static constexpr unsigned short cmPatternEdit = commands::PatternEdit;
static constexpr unsigned short cmPatternDelete = commands::PatternDelete;
static constexpr unsigned short cmReturnToLauncher = commands::ReturnToLauncher;
static constexpr unsigned short cmManageCloud = commands::ManageCloudStorage;
#if defined(__APPLE__)
static constexpr unsigned short cmPauseOperation = 0x7100;
static constexpr unsigned short cmResumeOperation = 0x7101;
#endif

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

#if !defined(_WIN32)
bool clipboardOsc52Likely()
{
    const char *noOsc52 = std::getenv("NO_OSC52");
    if (noOsc52 && *noOsc52)
        return false;

    const char *term = std::getenv("TERM");
    if (!term)
        return false;

    std::string termStr(term);

    if (termStr == "dumb" || termStr == "linux")
        return false;

    return termStr.find("xterm") != std::string::npos || termStr.find("tmux") != std::string::npos ||
           termStr.find("screen") != std::string::npos || termStr.find("rxvt") != std::string::npos ||
           termStr.find("alacritty") != std::string::npos || termStr.find("foot") != std::string::npos ||
           termStr.find("kitty") != std::string::npos || termStr.find("wezterm") != std::string::npos;
}
#endif

std::string clipboardStatusMessage()
{
#if defined(_WIN32)
    return "Path copied to clipboard!";
#else
    bool likely = clipboardOsc52Likely();
    if (likely)
        return "Path copied to clipboard!";
    if (std::getenv("TMUX") && !likely)
        return "Clipboard not supported - tmux needs OSC 52 configuration";
    return "Clipboard not supported by this terminal";
#endif
}

void copyTextToClipboard(const std::string &text)
{
    TClipboard::setText(text.c_str());
}

#if defined(__APPLE__)
std::string ellipsizeMiddle(const std::string &text, std::size_t maxLen)
{
    if (text.size() <= maxLen)
        return text;
    if (maxLen <= 3)
        return text.substr(0, maxLen);
    std::size_t front = maxLen / 2;
    if (front < 1)
        front = 1;
    std::size_t back = maxLen - front - 3;
    if (back < 1)
        back = 1;
    if (front + back + 3 > maxLen)
        front = maxLen > 3 ? (maxLen - 3) / 2 : 0;
    if (front + back + 3 > maxLen)
        back = maxLen > 3 ? maxLen - 3 - front : 0;
    return text.substr(0, front) + "..." + text.substr(text.size() - back);
}

std::string formatCountLabel(std::size_t count, std::string_view singular, std::string_view plural)
{
    std::ostringstream out;
    out << count << ' ' << (count == 1 ? singular : plural);
    return out.str();
}
#endif

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
std::string listEntryName(const FileEntry &entry);

std::uintmax_t combinedLogicalBytes(std::uintmax_t local, std::uintmax_t cloud, std::uintmax_t logical)
{
    std::uintmax_t combined = local + cloud;
    if (logical > 0)
        combined = std::max(combined, logical);
    return combined;
}

std::string formatUsageBreakdown(std::uintmax_t local, std::uintmax_t cloud, std::uintmax_t logical)
{
    std::ostringstream out;
    out << formatSize(local, getCurrentUnit()) << " local";
    if (cloud > 0)
        out << ", " << formatSize(cloud, getCurrentUnit()) << " cloud";
    std::uintmax_t total = combinedLogicalBytes(local, cloud, logical);
    if (cloud > 0 || total != local)
        out << ", " << formatSize(total, getCurrentUnit()) << " total";
    return out.str();
}

std::string formatDirectoryUsage(const DirectoryStats &stats)
{
    return formatUsageBreakdown(stats.totalSize, stats.cloudOnlySize, stats.logicalSize);
}

std::string describeICloudState(const FileEntry &entry)
{
    if (entry.isICloudDownloading)
        return "downloading";
    if (entry.cloudOnlySize > 0)
    {
        if (entry.size == 0)
            return "not downloaded";
        return "partially downloaded";
    }
    if (entry.isICloudItem)
        return "downloaded";
    return {};
}

std::string fileCloudLabel(const FileEntry &entry)
{
    if (entry.isICloudDownloading)
        return " [downloading]";
    if (entry.cloudOnlySize > 0)
    {
        if (entry.size == 0)
            return " [cloud]";
        return " [partial]";
    }
    return {};
}

std::string displayEntryName(const FileEntry &entry)
{
    std::string name = listEntryName(entry);
    std::string label = fileCloudLabel(entry);
    if (!label.empty())
        name += label;
    return name;
}

std::string fileSizeColumnText(const FileEntry &entry)
{
    std::string text = formatSize(entry.size, getCurrentUnit());
    if (entry.cloudOnlySize > 0)
    {
        text += " + ";
        text += formatSize(entry.cloudOnlySize, getCurrentUnit());
        text += " cloud";
    }
    return text;
}

std::string formatFileUsage(const FileEntry &entry)
{
    return formatUsageBreakdown(entry.size, entry.cloudOnlySize, entry.logicalSize);
}

std::string fileTypeDisplayName(const FileTypeSummary &summary)
{
    if (summary.cloudOnlyCount == 0)
        return summary.type;
    std::ostringstream out;
    out << summary.type << " (" << summary.cloudOnlyCount << " cloud-only)";
    return out.str();
}

std::string fileTypeSizeColumnText(const FileTypeSummary &summary)
{
    std::string text = formatSize(summary.totalSize, getCurrentUnit());
    if (summary.cloudOnlySize > 0)
    {
        text += " + ";
        text += formatSize(summary.cloudOnlySize, getCurrentUnit());
        text += " cloud";
    }
    return text;
}

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
    out << name << "  [" << formatDirectoryUsage(node->stats) << "]";
    out << "  " << node->stats.fileCount << (node->stats.fileCount == 1 ? " file" : " files");
    if (node->stats.cloudOnlyFileCount > 0)
        out << " (" << node->stats.cloudOnlyFileCount << " cloud-only)";
    if (node->stats.directoryCount > 0)
        out << ", " << node->stats.directoryCount << (node->stats.directoryCount == 1 ? " dir" : " dirs");
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

std::string listEntryName(const FileEntry &entry)
{
    std::string name = entry.path.filename().string();
    if (!name.empty())
        return name;
    if (!entry.displayPath.empty())
        return entry.displayPath;
    return entry.path.string();
}

void applySortToFiles(std::vector<FileEntry> &entries)
{
    auto key = getCurrentSortKey();
    auto nameLess = [](const FileEntry &a, const FileEntry &b) {
        return listEntryName(a) < listEntryName(b);
    };
    auto nameGreater = [](const FileEntry &a, const FileEntry &b) {
        return listEntryName(a) > listEntryName(b);
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

#if defined(__APPLE__)

using CloudActionKind = cloud::ActionKind;
using CloudUsageSnapshot = cloud::UsageSnapshot;
using CloudOperationDefinition = cloud::OperationDefinition;
using CloudDialogSelection = cloud::DialogSelection;
using CloudOperationProgress = cloud::OperationProgress;

class CloudOperationProgressDialog : public TDialog
{
public:
    CloudOperationProgressDialog(const char *titleText, const char *messageText);

    void setCancelHandler(std::function<void()> handler);
    void setPauseHandler(std::function<void()> handler);
    void setResumeHandler(std::function<void()> handler);

    void update(const CloudOperationProgress &progress, bool paused, const std::string &statusText);

    virtual void handleEvent(TEvent &event) override;

private:
    void applyPausedState(bool paused);
    void setParamText(TParamText *view, const std::string &text);

    TParamText *statusLine = nullptr;
    TParamText *detailLine = nullptr;
    TParamText *stateLine = nullptr;
    TButton *pauseButton = nullptr;
    TButton *resumeButton = nullptr;
    std::function<void()> cancelHandler;
    std::function<void()> pauseHandler;
    std::function<void()> resumeHandler;
    bool pausedState = false;
};

class ManageCloudDialog;

class CloudActionListView : public TListViewer
{
public:
    CloudActionListView(const TRect &bounds, TScrollBar *scrollBar,
                        std::vector<CloudOperationDefinition> &definitions)
        : TListViewer(bounds, 1, nullptr, scrollBar), actions(&definitions)
    {
        setRange(static_cast<short>(actions->size()));
    }

    void setOwner(ManageCloudDialog *dialog) { ownerDialog = dialog; }

    short currentIndex() const { return focused; }

    virtual void getText(char *dest, short item, short maxLen) override
    {
        if (!actions || item < 0 || static_cast<std::size_t>(item) >= actions->size())
        {
            if (maxLen > 0)
                dest[0] = '\0';
            return;
        }
        const auto &action = (*actions)[static_cast<std::size_t>(item)];
        std::string text = action.label;
        if (!action.enabled)
            text += " (not applicable)";
        if (static_cast<std::size_t>(maxLen) <= text.size())
        {
            if (maxLen > 1)
                text.resize(static_cast<std::size_t>(maxLen) - 1);
            else
                text.clear();
        }
        std::snprintf(dest, static_cast<std::size_t>(maxLen), "%s", text.c_str());
    }

    virtual void focusItem(short item) override;
    virtual void handleEvent(TEvent &event) override;

private:
    std::vector<CloudOperationDefinition> *actions = nullptr;
    ManageCloudDialog *ownerDialog = nullptr;
};

CloudOperationProgressDialog::CloudOperationProgressDialog(const char *titleText, const char *messageText)
    : TWindowInit(&TDialog::initFrame),
      TDialog(TRect(0, 0, 68, 12), titleText ? titleText : "Cloud Operation")
{
    options |= ofCentered;
    insert(new TStaticText(TRect(2, 2, 66, 3), messageText ? messageText : "Processing iCloud items..."));

    stateLine = new TParamText(TRect(2, 3, 66, 4));
    insert(stateLine);

    statusLine = new TParamText(TRect(2, 4, 66, 5));
    insert(statusLine);

    detailLine = new TParamText(TRect(2, 5, 66, 6));
    insert(detailLine);

    pauseButton = new TButton(TRect(12, 8, 24, 10), "~P~ause", cmPauseOperation, bfNormal);
    resumeButton = new TButton(TRect(26, 8, 38, 10), "~R~esume", cmResumeOperation, bfNormal);
    resumeButton->setState(sfDisabled, True);
    insert(pauseButton);
    insert(resumeButton);
    insert(new TButton(TRect(40, 8, 52, 10), "Cancel", cmCancel, bfNormal));

    setParamText(stateLine, "Preparing operation...");
    setParamText(statusLine, "");
    setParamText(detailLine, "");
}

void CloudOperationProgressDialog::setCancelHandler(std::function<void()> handler)
{
    cancelHandler = std::move(handler);
}

void CloudOperationProgressDialog::setPauseHandler(std::function<void()> handler)
{
    pauseHandler = std::move(handler);
}

void CloudOperationProgressDialog::setResumeHandler(std::function<void()> handler)
{
    resumeHandler = std::move(handler);
}

void CloudOperationProgressDialog::setParamText(TParamText *view, const std::string &text)
{
    if (!view)
        return;
    view->setText("%s", text.c_str());
    view->drawView();
}

void CloudOperationProgressDialog::applyPausedState(bool paused)
{
    pausedState = paused;
    if (pauseButton)
        pauseButton->setState(sfDisabled, paused ? True : False);
    if (resumeButton)
        resumeButton->setState(sfDisabled, paused ? False : True);
}

void CloudOperationProgressDialog::update(const CloudOperationProgress &progress, bool paused, const std::string &statusText)
{
    std::ostringstream status;
    if (progress.totalItems > 0)
    {
        status << "Items: " << progress.processedItems << "/" << progress.totalItems;
    }
    if (progress.totalBytes > 0)
    {
        if (!status.str().empty())
            status << " — ";
        status << "Data: " << formatSize(progress.processedBytes, SizeUnit::Auto)
               << "/" << formatSize(progress.totalBytes, SizeUnit::Auto);
    }
    setParamText(statusLine, status.str());

    std::string detail;
    if (!progress.currentItem.empty())
        detail = "Current: " + ellipsizeMiddle(progress.currentItem, 58);
    else
        detail = "Current: (waiting)";
    setParamText(detailLine, detail);

    setParamText(stateLine, statusText);
    applyPausedState(paused);
}

void CloudOperationProgressDialog::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmCancel:
            if (cancelHandler)
                cancelHandler();
            clearEvent(event);
            return;
        case cmPauseOperation:
            if (!pausedState)
            {
                applyPausedState(true);
                if (pauseHandler)
                    pauseHandler();
            }
            clearEvent(event);
            return;
        case cmResumeOperation:
            if (pausedState)
            {
                applyPausedState(false);
                if (resumeHandler)
                    resumeHandler();
            }
            clearEvent(event);
            return;
        default:
            break;
        }
    }
    TDialog::handleEvent(event);
}

class ManageCloudDialog : public TDialog
{
public:
    ManageCloudDialog(const std::filesystem::path &targetPath,
                      CloudUsageSnapshot snapshot,
                      std::vector<CloudOperationDefinition> definitions,
                      CloudDialogSelection *selection);

    std::optional<CloudActionKind> chosenAction() const { return selected; }
    std::optional<CloudOperationDefinition> selectedDefinition() const;

    void updateDetails();
    CloudOperationDefinition *currentDefinition();
    const CloudOperationDefinition *currentDefinition() const;

    virtual void handleEvent(TEvent &event) override;

private:
    void updateRunButtonState();
    std::string pathLine() const;
    std::string usageLine(const CloudUsageSnapshot &snapshot) const;

    std::filesystem::path path;
    CloudUsageSnapshot usage{};
    std::vector<CloudOperationDefinition> actions;
    CloudActionListView *listView = nullptr;
    TScrollBar *listScroll = nullptr;
    TParamText *explanation = nullptr;
    TParamText *impact = nullptr;
    TButton *runButton = nullptr;
    std::optional<CloudActionKind> selected;
    short chosenIndex = -1;
    CloudDialogSelection *selectionOut = nullptr;
};

void CloudActionListView::focusItem(short item)
{
    TListViewer::focusItem(item);
    if (ownerDialog)
        ownerDialog->updateDetails();
}

void CloudActionListView::handleEvent(TEvent &event)
{
    TListViewer::handleEvent(event);
    if (event.what == evKeyDown && event.keyDown.keyCode == kbEnter)
    {
        if (ownerDialog)
            message(ownerDialog, evCommand, cmOK, this);
        clearEvent(event);
    }
}

ManageCloudDialog::ManageCloudDialog(const std::filesystem::path &targetPath,
                                     CloudUsageSnapshot snapshot,
                                     std::vector<CloudOperationDefinition> definitions,
                                     CloudDialogSelection *selection)
    : TWindowInit(&TDialog::initFrame),
      TDialog(TRect(0, 0, 78, 22), "Manage Cloud Storage"),
      path(targetPath),
      usage(snapshot),
      actions(std::move(definitions)),
      selectionOut(selection)
{
    options |= ofCentered;
    if (selectionOut)
        selectionOut->confirmed = false;

    insert(new TStaticText(TRect(2, 2, 76, 3), pathLine().c_str()));
    insert(new TStaticText(TRect(2, 3, 76, 4), usageLine(snapshot).c_str()));

    listScroll = new TScrollBar(TRect(74, 5, 75, 14));
    insert(listScroll);

    listView = new CloudActionListView(TRect(2, 5, 74, 14), listScroll, actions);
    listView->growMode = gfGrowHiX | gfGrowHiY;
    listScroll->growMode = gfGrowHiY;
    listView->setOwner(this);
    insert(listView);

    explanation = new TParamText(TRect(2, 14, 76, 17));
    insert(explanation);

    impact = new TParamText(TRect(2, 17, 76, 19));
    insert(impact);

    runButton = new TButton(TRect(20, 19, 32, 21), "~R~un", cmOK, bfDefault);
    insert(runButton);
    insert(new TButton(TRect(34, 19, 46, 21), "Cancel", cmCancel, bfNormal));

    if (!actions.empty())
        listView->focusItem(0);
    updateDetails();
}

std::string ManageCloudDialog::pathLine() const
{
    std::string display = path.empty() ? std::string("(no path selected)") : path.string();
    display = ellipsizeMiddle(display, 70);
    return "Directory: " + display;
}

std::string ManageCloudDialog::usageLine(const CloudUsageSnapshot &snapshot) const
{
    std::ostringstream out;
    out << formatCountLabel(snapshot.totalFiles, "file", "files");
    if (snapshot.cloudOnlyFiles > 0)
        out << " — " << snapshot.cloudOnlyFiles << " cloud-only";
    if (snapshot.localFiles > 0)
    {
        out << " — local " << formatSize(snapshot.localBytes, SizeUnit::Auto);
        if (snapshot.cloudBytes > 0)
            out << ", cloud " << formatSize(snapshot.cloudBytes, SizeUnit::Auto);
    }
    else if (snapshot.cloudBytes > 0)
        out << " — downloads pending " << formatSize(snapshot.cloudBytes, SizeUnit::Auto);
    return out.str();
}

CloudOperationDefinition *ManageCloudDialog::currentDefinition()
{
    if (!listView)
        return nullptr;
    short index = listView->currentIndex();
    if (index < 0 || static_cast<std::size_t>(index) >= actions.size())
        return nullptr;
    return &actions[static_cast<std::size_t>(index)];
}

const CloudOperationDefinition *ManageCloudDialog::currentDefinition() const
{
    return const_cast<ManageCloudDialog *>(this)->currentDefinition();
}

void ManageCloudDialog::updateRunButtonState()
{
    if (!runButton)
        return;
    bool enable = false;
    if (const auto *def = currentDefinition())
        enable = def->enabled;
    runButton->setState(sfDisabled, enable ? False : True);
}

void ManageCloudDialog::updateDetails()
{
    const auto *def = currentDefinition();
    if (explanation)
    {
        std::string text = def ? def->explanation : "Select an action to review its details.";
        explanation->setText("%s", text.c_str());
        explanation->drawView();
    }
    if (impact)
    {
        std::string text = def ? def->impact : "";
        impact->setText("%s", text.c_str());
        impact->drawView();
    }
    updateRunButtonState();
}

void ManageCloudDialog::handleEvent(TEvent &event)
{
    if (event.what == evCommand)
    {
        if (event.message.command == cmOK)
        {
            if (auto *def = currentDefinition(); def && def->enabled)
            {
                selected = def->kind;
                chosenIndex = listView ? listView->currentIndex() : -1;
                if (selectionOut)
                {
                    selectionOut->confirmed = true;
                    selectionOut->action = def->kind;
                    selectionOut->definition = *def;
                }
            }
            else
            {
                messageBox("The selected action is not available.", mfInformation | mfOKButton);
                clearEvent(event);
                return;
            }
        }
        else if (event.message.command == cmCancel)
        {
            selected.reset();
            if (selectionOut)
                selectionOut->confirmed = false;
        }
    }
    TDialog::handleEvent(event);
}

std::size_t cloudOperationItemTarget(CloudActionKind action, const CloudUsageSnapshot &usage)
{
    switch (action)
    {
    case CloudActionKind::DownloadAll:
        return usage.cloudOnlyFiles;
    case CloudActionKind::EvictLocalCopies:
    case CloudActionKind::KeepAlways:
        return usage.localFiles;
    case CloudActionKind::OptimizeStorage:
        return usage.totalFiles;
    case CloudActionKind::PauseSync:
    case CloudActionKind::ResumeSync:
    case CloudActionKind::RevealInFinder:
        return 1;
    default:
        return usage.totalFiles;
    }
}

std::uintmax_t cloudOperationByteTarget(CloudActionKind action, const CloudUsageSnapshot &usage)
{
    switch (action)
    {
    case CloudActionKind::DownloadAll:
        return usage.cloudBytes;
    case CloudActionKind::EvictLocalCopies:
        return usage.localBytes;
    case CloudActionKind::KeepAlways:
        return usage.logicalBytes;
    case CloudActionKind::OptimizeStorage:
        return usage.logicalBytes;
    case CloudActionKind::PauseSync:
    case CloudActionKind::ResumeSync:
    case CloudActionKind::RevealInFinder:
        return 0;
    default:
        return usage.localBytes + usage.cloudBytes;
    }
}

std::vector<CloudOperationDefinition> buildCloudOperationDefinitions(const CloudUsageSnapshot &usage, bool canPause = false)
{
    auto makeSize = [](std::uintmax_t bytes) { return formatSize(bytes, SizeUnit::Auto); };
    std::vector<CloudOperationDefinition> actions;
    actions.reserve(7);

    CloudOperationDefinition download{CloudActionKind::DownloadAll,
                                      "Download missing content",
                                      "Downloads every file that currently lives only in iCloud so the entire selection is usable offline.",
                                      ""};
    if (usage.cloudOnlyFiles > 0)
    {
        std::ostringstream impact;
        impact << "Would fetch " << formatCountLabel(usage.cloudOnlyFiles, "file", "files");
        impact << " totaling " << makeSize(usage.cloudBytes) << '.';
        download.impact = impact.str();
    }
    else
    {
        download.enabled = false;
        download.impact = "All files are already stored locally.";
    }
    actions.push_back(download);

    CloudOperationDefinition evict{CloudActionKind::EvictLocalCopies,
                                   "Remove local copies",
                                   "Evicts downloaded data while leaving placeholders intact so macOS can reclaim disk space.",
                                   ""};
    if (usage.localFiles > 0 && usage.localBytes > 0)
    {
        std::ostringstream impact;
        impact << "Could free up to " << makeSize(usage.localBytes)
               << " across " << formatCountLabel(usage.localFiles, "file", "files") << '.';
        evict.impact = impact.str();
    }
    else
    {
        evict.enabled = false;
        evict.impact = "No local content is available to evict.";
    }
    actions.push_back(evict);

    CloudOperationDefinition keep{CloudActionKind::KeepAlways,
                                  "Always keep on this device",
                                  "Marks the selected files as \"Always Keep\" so macOS maintains local copies even under storage pressure.",
                                  ""};
    if (usage.localFiles > 0)
    {
        std::ostringstream impact;
        impact << "Applies to " << formatCountLabel(usage.localFiles, "file", "files")
               << " totaling " << makeSize(usage.logicalBytes) << '.';
        keep.impact = impact.str();
    }
    else
    {
        keep.enabled = false;
        keep.impact = "No downloaded files are available to pin locally.";
    }
    actions.push_back(keep);

    CloudOperationDefinition optimize{CloudActionKind::OptimizeStorage,
                                      "Let macOS optimize storage",
                                      "Clears the \"Always Keep\" preference so macOS can evict downloads for this folder automatically.",
                                      "Leaves data in iCloud; macOS may reclaim up to " +
                                          makeSize(usage.localBytes) + " if space is needed."};
    actions.push_back(optimize);

    CloudOperationDefinition pause{CloudActionKind::PauseSync,
                                   "Pause sync transfers",
                                   "Requests iCloud Drive to pause uploads and downloads for this directory.",
                                   "No files are modified; outstanding transfers remain pending until resumed."};
    if (!canPause)
    {
        pause.enabled = false;
        pause.impact = "Not supported on this macOS version.";
    }
    actions.push_back(pause);

    CloudOperationDefinition resume{CloudActionKind::ResumeSync,
                                    "Resume sync transfers",
                                    "Resumes iCloud Drive activity for this directory after a pause.",
                                    "Outstanding uploads and downloads will continue."};
    if (!canPause)
    {
        resume.enabled = false;
        resume.impact = "Not supported on this macOS version.";
    }
    actions.push_back(resume);

    CloudOperationDefinition reveal{CloudActionKind::RevealInFinder,
                                    "Show in Finder",
                                    "Opens Finder and highlights the selected directory for further management.",
                                    "Finder will open a new window for the directory."};
    actions.push_back(reveal);

    return actions;
}

#endif // defined(__APPLE__)

void applySortToFileTypes(std::vector<FileTypeSummary> &entries)
{
    auto key = getCurrentSortKey();
    auto nameLess = [](const FileTypeSummary &a, const FileTypeSummary &b) {
        return a.type < b.type;
    };
    auto nameGreater = [](const FileTypeSummary &a, const FileTypeSummary &b) {
        return a.type > b.type;
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
        std::stable_sort(entries.begin(), entries.end(), [&](const FileTypeSummary &a, const FileTypeSummary &b) {
            if (a.totalSize == b.totalSize)
                return nameLess(a, b);
            return a.totalSize > b.totalSize;
        });
        break;
    case SortKey::SizeAscending:
        std::stable_sort(entries.begin(), entries.end(), [&](const FileTypeSummary &a, const FileTypeSummary &b) {
            if (a.totalSize == b.totalSize)
                return nameLess(a, b);
            return a.totalSize < b.totalSize;
        });
        break;
    case SortKey::ModifiedDescending:
        std::stable_sort(entries.begin(), entries.end(), [&](const FileTypeSummary &a, const FileTypeSummary &b) {
            if (a.count == b.count)
                return nameLess(a, b);
            return a.count > b.count;
        });
        break;
    case SortKey::ModifiedAscending:
        std::stable_sort(entries.begin(), entries.end(), [&](const FileTypeSummary &a, const FileTypeSummary &b) {
            if (a.count == b.count)
                return nameLess(a, b);
            return a.count < b.count;
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
    virtual void focusItem(short item) override;

    void refreshMetrics();
    void setHeader(FileListHeaderView *headerView);
    void setOwner(FileListWindow *window);
    const FileEntry *currentEntry() const;
    std::string headerLine() const;
    int horizontalOffset() const;
    ushort headerColorIndex() const;

private:
    static constexpr std::size_t kSeparatorWidth = 2;
    static constexpr std::size_t kSeparatorCount = 5;

    std::vector<FileEntry> &files;
    FileListHeaderView *header = nullptr;
    FileListWindow *owner = nullptr;
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
    void updateStatus();
    const FileEntry *selectedEntry() const;

    virtual void setState(ushort aState, Boolean enable) override;

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

class FileTypeWindow;
class FileTypeHeaderView;

class FileTypeListView : public TListViewer
{
public:
    FileTypeListView(const TRect &bounds, TScrollBar *h, TScrollBar *v,
                     std::vector<FileTypeSummary> &entries);

    virtual void getText(char *dest, short item, short maxLen) override;
    virtual void changeBounds(const TRect &bounds) override;
    virtual void handleEvent(TEvent &event) override;
    virtual void focusItem(short item) override;

    void refreshMetrics();
    void setHeader(FileTypeHeaderView *headerView);
    void setOwner(FileTypeWindow *window);
    const FileTypeSummary *currentEntry() const;
    std::string headerLine() const;
    int horizontalOffset() const;
    ushort headerColorIndex() const;

private:
    static constexpr std::size_t kSeparatorWidth = 2;
    static constexpr std::size_t kSeparatorCount = 2;

    std::vector<FileTypeSummary> &entries;
    FileTypeHeaderView *header = nullptr;
    FileTypeWindow *owner = nullptr;
    std::size_t maxLineWidth = 0;
    std::size_t typeWidth = 0;
    std::size_t countWidth = 0;
    std::size_t sizeWidth = 0;

    void computeWidths();
    std::size_t totalLineWidth() const;
    void updateMaxLineWidth();
    std::string formatRow(const std::string &type, const std::string &count,
                          const std::string &size) const;
    void notifyHeader();
};

class FileTypeHeaderView : public TView
{
public:
    FileTypeHeaderView(const TRect &bounds, FileTypeListView &listView);

    virtual void draw() override;
    void refresh();

private:
    FileTypeListView &listView;
};

class FileTypeWindow : public TWindow
{
public:
    FileTypeWindow(const std::string &title, std::filesystem::path directory,
                   std::vector<FileTypeSummary> entries, bool recursive,
                   BuildDirectoryTreeOptions scanOptions, class DiskUsageApp &app);
    ~FileTypeWindow();

    void refreshUnits();
    void refreshSort();
    void updateStatus();
    const FileTypeSummary *selectedEntry() const;

    virtual void handleEvent(TEvent &event) override;
    virtual void setState(ushort aState, Boolean enable) override;

private:
    void buildView();
    void openFilesForSelectedType();

    class DiskUsageApp &app;
    std::filesystem::path basePath;
    BuildDirectoryTreeOptions scanOptions;
    std::vector<FileTypeSummary> baseEntries;
    std::vector<FileTypeSummary> entries;
    FileTypeListView *listView = nullptr;
    FileTypeHeaderView *headerView = nullptr;
    TScrollBar *hScroll = nullptr;
    TScrollBar *vScroll = nullptr;
    bool recursiveMode = false;
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
    ScanProgressDialog(const char *titleText = "Scanning Directory",
                       const char *messageText = "Scanning directory...");

    void setCancelHandler(std::function<void()> handler);
    void updatePath(const std::string &path);

    virtual void handleEvent(TEvent &event) override;

private:
    void setPathText(const std::string &text);

    TParamText *pathText = nullptr;
    std::function<void()> cancelHandler;
    std::string lastDisplay;
};

class DiskUsageStatusLine : public ck::ui::CommandAwareStatusLine
{
public:
    DiskUsageStatusLine(TRect r)
        : ck::ui::CommandAwareStatusLine(r, *new TStatusDef(0, 0xFFFF, nullptr))
    {
        showDefaultHints();
    }

    void showDefaultHints()
    {
        currentMessage.clear();
        setItems(buildHintChain());
    }

    void showMessage(std::string message)
    {
        currentMessage = std::move(message);
        auto *item = new TStatusItem(currentMessage.c_str(), kbNoKey, 0);
        setItems(item);
    }

private:
    std::string currentMessage;

    void setItems(TStatusItem *chain)
    {
        disposeItems(items);
        items = chain;
        defs->items = items;
        drawView();
    }

    TStatusItem *buildHintChain()
    {
        auto *open = new TStatusItem("Open", kbNoKey, cmOpen);
        ck::hotkeys::configureStatusItem(*open, "Open");
        auto *files = new TStatusItem("Files", kbNoKey, commands::ViewFiles);
        ck::hotkeys::configureStatusItem(*files, "Files");
        auto *recursive = new TStatusItem("Files+Sub", kbNoKey, commands::ViewFilesRecursive);
        ck::hotkeys::configureStatusItem(*recursive, "Files+Sub");
        auto *types = new TStatusItem("Types", kbNoKey, commands::ViewFileTypes);
        ck::hotkeys::configureStatusItem(*types, "Types");
        auto *typesRecursive = new TStatusItem("Types+Sub", kbNoKey, commands::ViewFileTypesRecursive);
        ck::hotkeys::configureStatusItem(*typesRecursive, "Types+Sub");
        auto *sortName = new TStatusItem("Sort Name", kbNoKey, cmSortNameAsc);
        ck::hotkeys::configureStatusItem(*sortName, "Sort Name");
        auto *sortSize = new TStatusItem("Sort Size", kbNoKey, cmSortSizeDesc);
        ck::hotkeys::configureStatusItem(*sortSize, "Sort Size");
        auto *sortModified = new TStatusItem("Sort Modified", kbNoKey, cmSortModifiedDesc);
        ck::hotkeys::configureStatusItem(*sortModified, "Sort Modified");
        auto *quit = new TStatusItem("Quit", kbNoKey, cmQuit);
        ck::hotkeys::configureStatusItem(*quit, "Quit");
        TStatusItem *returnItem = nullptr;
        if (ck::launcher::launchedFromCkLauncher())
        {
            returnItem = new TStatusItem("Return", kbNoKey, cmReturnToLauncher);
            ck::hotkeys::configureStatusItem(*returnItem, "Return");
        }
        open->next = files;
        files->next = recursive;
        recursive->next = types;
        types->next = typesRecursive;
        typesRecursive->next = sortName;
        sortName->next = sortSize;
        sortSize->next = sortModified;
        if (returnItem)
        {
            sortModified->next = returnItem;
            returnItem->next = quit;
        }
        else
        {
            sortModified->next = quit;
        }
        return open;
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

class DiskUsageApp : public ck::ui::ClockAwareApplication
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
    void registerTypeWindow(FileTypeWindow *window);
    void unregisterTypeWindow(FileTypeWindow *window);

    void showDefaultStatusHints();
    void showFilePath(const std::filesystem::path &path);
    void showFileDetails(const FileEntry &entry);
    void showTypeSummary(const FileTypeSummary &summary, bool recursive);

    void notifyUnitsChanged();
    void notifySortChanged();

private:
    friend class FileTypeWindow;

    std::vector<DirectoryWindow *> directoryWindows;
    std::vector<FileListWindow *> fileWindows;
    std::vector<FileTypeWindow *> typeWindows;
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

    struct FileListTask
    {
        std::filesystem::path directory;
        bool recursive = false;
        std::string title;
        std::optional<std::string> typeFilter;
        std::thread worker;
        std::mutex mutex;
        std::vector<FileEntry> files;
        std::vector<std::string> errors;
        std::string currentPath;
        std::string errorMessage;
        bool cancelled = false;
        bool failed = false;
        bool reportErrors = false;
        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> finished{false};
        ScanProgressDialog *dialog = nullptr;
    };

    struct FileTypeTask
    {
        std::filesystem::path directory;
        bool recursive = false;
        std::string title;
        BuildDirectoryTreeOptions options;
        std::thread worker;
        std::mutex mutex;
        std::vector<FileTypeSummary> types;
        std::vector<std::string> errors;
        std::string currentPath;
        std::string errorMessage;
        bool cancelled = false;
        bool failed = false;
        bool reportErrors = false;
        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> finished{false};
        ScanProgressDialog *dialog = nullptr;
    };

    std::unique_ptr<DirectoryScanTask> activeScan;
    std::deque<std::filesystem::path> pendingScanQueue;
    std::unique_ptr<FileListTask> activeFileList;
    std::unique_ptr<FileTypeTask> activeFileType;
#if defined(__APPLE__)
    struct CloudOperationTask
    {
        CloudActionKind action;
        CloudOperationDefinition definition;
        CloudUsageSnapshot usage;
        std::filesystem::path rootPath;
        std::thread worker;
        std::mutex mutex;
        CloudOperationProgress progress;
        std::string statusMessage;
        bool failed = false;
        std::string errorMessage;
        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> pauseRequested{false};
        std::atomic<bool> paused{false};
        std::atomic<bool> finished{false};
        CloudOperationProgressDialog *dialog = nullptr;
        bool recursive = true;
    };
    std::unique_ptr<CloudOperationTask> activeCloudOperation;
#endif

    void promptOpenDirectory();
    void openDirectory(const std::filesystem::path &path);
    void copySelectedPath();
    void viewFiles(bool recursive);
    void viewFileTypes(bool recursive);
    void viewFilesForType(const std::filesystem::path &directory, bool recursive, const std::string &type,
                          const BuildDirectoryTreeOptions &options);
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
#if defined(__APPLE__)
    void manageCloudStorage();
    void startCloudOperation(CloudActionKind action, const CloudOperationDefinition &definition,
                             const CloudUsageSnapshot &usage, const std::filesystem::path &path);
    void updateCloudOperationProgress(CloudOperationTask &task);
    void closeCloudOperationDialog(CloudOperationTask &task);
    void processCloudOperationCompletion();
    void runCloudOperation(CloudOperationTask &task);
    void requestCloudOperationPause();
    void requestCloudOperationResume();
    void requestCloudOperationCancel();
    void cancelActiveCloudOperation(bool waitForCompletion);
#endif
    void loadOptionsFromFile();
    void saveOptionsToFile();
    void saveDefaultOptions();
    void reloadOptionState();
    void requestDirectoryScan(const std::filesystem::path &path, bool allowQueue);
    void queueDirectoryForScan(const std::filesystem::path &path);
    void startDirectoryScan(const std::filesystem::path &path);
    void startNextQueuedDirectory();
    void startFileListTask(const std::filesystem::path &directory, bool recursive,
                           BuildDirectoryTreeOptions options, std::string title,
                           std::optional<std::string> typeFilter = std::nullopt);
    void startFileTypeTask(const std::filesystem::path &directory, bool recursive,
                           BuildDirectoryTreeOptions options, std::string title);
    void updateScanProgress(DirectoryScanTask &task);
    void updateFileListProgress(FileListTask &task);
    void updateFileTypeProgress(FileTypeTask &task);
    void processActiveScanCompletion();
    void processActiveFileListCompletion();
    void processActiveFileTypeCompletion();
    void runDirectoryScan(DirectoryScanTask &task);
    void requestScanCancellation();
    void requestFileListCancellation();
    void requestFileTypeCancellation();
    void closeProgressDialog(DirectoryScanTask &task);
    void closeProgressDialog(FileListTask &task);
    void closeProgressDialog(FileTypeTask &task);
    void cancelActiveScan(bool waitForCompletion);
    void cancelActiveFileList(bool waitForCompletion);
    void cancelActiveFileType(bool waitForCompletion);
};

DirectoryOutline::DirectoryOutline(TRect bounds, TScrollBar *h, TScrollBar *v, DirTNode *rootNode, DirectoryWindow &owner)
    : TOutline(bounds, h, v, rootNode), ownerWindow(owner)
{
}

ScanProgressDialog::ScanProgressDialog(const char *titleText, const char *messageText)
    : TWindowInit(&TDialog::initFrame),
      TDialog(TRect(0, 0, 60, 9), titleText ? titleText : "Scanning Directory")
{
    options |= ofCentered;
    insert(new TStaticText(TRect(2, 2, 58, 3), messageText ? messageText : "Scanning directory..."));
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
    sizeWidth = std::string("Local").size();
    createdWidth = std::string("Created").size();
    modifiedWidth = std::string("Modified").size();

    for (const auto &entry : files)
    {
        nameWidth = std::max(nameWidth, displayEntryName(entry).size());
        ownerWidth = std::max(ownerWidth, entry.owner.size());
        groupWidth = std::max(groupWidth, entry.group.size());
        createdWidth = std::max(createdWidth, entry.created.size());
        modifiedWidth = std::max(modifiedWidth, entry.modified.size());
        sizeWidth = std::max(sizeWidth, fileSizeColumnText(entry).size());
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
    std::string sizeStr = fileSizeColumnText(entry);
    std::string text = formatRow(displayEntryName(entry), entry.owner, entry.group, sizeStr, entry.created, entry.modified);
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
    if (owner)
        owner->updateStatus();
}

void FileListView::focusItem(short item)
{
    TListViewer::focusItem(item);
    if (owner)
        owner->updateStatus();
}

void FileListView::setOwner(FileListWindow *window)
{
    owner = window;
}

const FileEntry *FileListView::currentEntry() const
{
    if (focused < 0 || static_cast<std::size_t>(focused) >= files.size())
        return nullptr;
    return &files[focused];
}



void FileListView::setHeader(FileListHeaderView *headerView)
{
    header = headerView;
}

std::string FileListView::headerLine() const
{
    return formatRow("Name", "Owner", "Group", "Local", "Created", "Modified");
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
    if (getState(sfActive))
        app.showDefaultStatusHints();
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

    view->setOwner(this);
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
    updateStatus();
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
    updateStatus();
}

const FileEntry *FileListWindow::selectedEntry() const
{
    if (!listView)
        return nullptr;
    return listView->currentEntry();
}

void FileListWindow::updateStatus()
{
    if (!getState(sfActive))
        return;
    if (const FileEntry *entry = selectedEntry())
        app.showFileDetails(*entry);
    else
        app.showDefaultStatusHints();
}

void FileListWindow::setState(ushort aState, Boolean enable)
{
    TWindow::setState(aState, enable);
    if ((aState & sfActive) != 0)
    {
        if (enable)
            updateStatus();
        else
            app.showDefaultStatusHints();
    }
}

FileTypeListView::FileTypeListView(const TRect &bounds, TScrollBar *h, TScrollBar *v,
                                   std::vector<FileTypeSummary> &entriesIn)
    : TListViewer(bounds, 1, h, v), entries(entriesIn)
{
    setRange(static_cast<short>(entries.size()));
    computeWidths();
    updateMaxLineWidth();
}

void FileTypeListView::computeWidths()
{
    typeWidth = std::string("Type").size();
    countWidth = std::string("Files").size();
    sizeWidth = std::string("Local").size();

    for (const auto &entry : entries)
    {
        typeWidth = std::max(typeWidth, fileTypeDisplayName(entry).size());
        countWidth = std::max<std::size_t>(countWidth, std::to_string(entry.count).size());
        sizeWidth = std::max(sizeWidth, fileTypeSizeColumnText(entry).size());
    }

    countWidth = std::max(countWidth, std::string("0").size());
    sizeWidth = std::max(sizeWidth, std::string("0 B").size());
}

void FileTypeListView::refreshMetrics()
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

void FileTypeListView::getText(char *dest, short item, short maxLen)
{
    if (item < 0 || static_cast<std::size_t>(item) >= entries.size())
    {
        *dest = '\0';
        return;
    }

    const FileTypeSummary &entry = entries[item];
    std::string countStr = std::to_string(entry.count);
    std::string sizeStr = fileTypeSizeColumnText(entry);
    std::string text = formatRow(fileTypeDisplayName(entry), countStr, sizeStr);
    if (text.size() >= static_cast<std::size_t>(maxLen))
        text.resize(maxLen - 1);
    std::snprintf(dest, maxLen, "%s", text.c_str());
}

void FileTypeListView::changeBounds(const TRect &bounds)
{
    TListViewer::changeBounds(bounds);
    refreshMetrics();
}

void FileTypeListView::handleEvent(TEvent &event)
{
    TListViewer::handleEvent(event);
    if (event.what == evKeyDown && event.keyDown.keyCode == kbEnter)
    {
        if (owner)
            message(owner, evCommand, commands::ViewFilesForType, this);
        clearEvent(event);
    }
    notifyHeader();
    if (owner)
        owner->updateStatus();
}

void FileTypeListView::focusItem(short item)
{
    TListViewer::focusItem(item);
    if (owner)
        owner->updateStatus();
}

void FileTypeListView::setHeader(FileTypeHeaderView *headerView)
{
    header = headerView;
}

void FileTypeListView::setOwner(FileTypeWindow *window)
{
    owner = window;
}

const FileTypeSummary *FileTypeListView::currentEntry() const
{
    if (focused < 0 || static_cast<std::size_t>(focused) >= entries.size())
        return nullptr;
    return &entries[focused];
}

std::string FileTypeListView::headerLine() const
{
    return formatRow("Type", "Files", "Local");
}

int FileTypeListView::horizontalOffset() const
{
    if (hScrollBar)
        return hScrollBar->value;
    return 0;
}

ushort FileTypeListView::headerColorIndex() const
{
    if (getState(sfActive) && getState(sfSelected))
        return 1;
    return 2;
}

std::size_t FileTypeListView::totalLineWidth() const
{
    return typeWidth + countWidth + sizeWidth + kSeparatorWidth * kSeparatorCount;
}

void FileTypeListView::updateMaxLineWidth()
{
    maxLineWidth = totalLineWidth();
    if (maxLineWidth < static_cast<std::size_t>(size.x))
        maxLineWidth = static_cast<std::size_t>(size.x);
}

std::string FileTypeListView::formatRow(const std::string &type, const std::string &count,
                                        const std::string &size) const
{
    static constexpr const char *separator = "  ";
    std::ostringstream line;
    line << std::left << std::setw(typeWidth) << type;
    line << separator;
    line << std::right << std::setw(countWidth) << count;
    line << separator;
    line << std::right << std::setw(sizeWidth) << size;
    return line.str();
}

void FileTypeListView::notifyHeader()
{
    if (header)
        header->refresh();
}

FileTypeHeaderView::FileTypeHeaderView(const TRect &bounds, FileTypeListView &list)
    : TView(bounds), listView(list)
{
    options &= ~(ofSelectable | ofFirstClick);
}

void FileTypeHeaderView::draw()
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

void FileTypeHeaderView::refresh()
{
    drawView();
}

FileTypeWindow::FileTypeWindow(const std::string &title, std::filesystem::path directory,
                               std::vector<FileTypeSummary> entriesIn, bool recursive,
                               BuildDirectoryTreeOptions scanOptionsIn, DiskUsageApp &appRef)
    : TWindowInit(&TWindow::initFrame),
      TWindow(TRect(0, 0, 74, 18), title.c_str(), wnNoNumber),
      app(appRef), basePath(std::move(directory)), scanOptions(std::move(scanOptionsIn)),
      baseEntries(std::move(entriesIn)), recursiveMode(recursive)
{
    flags |= wfGrow;
    growMode = gfGrowHiX | gfGrowHiY;
    refreshSort();
    buildView();
    app.registerTypeWindow(this);
}

FileTypeWindow::~FileTypeWindow()
{
    if (getState(sfActive))
        app.showDefaultStatusHints();
    app.unregisterTypeWindow(this);
}

void FileTypeWindow::buildView()
{
    TRect client = getExtent();
    client.grow(-1, -1);
    if (client.b.x <= client.a.x + 2 || client.b.y <= client.a.y + 3)
        client = TRect(0, 0, 60, 16);

    TRect headerBounds(client.a.x, client.a.y, client.b.x - 1, client.a.y + 1);
    TRect listBounds(client.a.x, client.a.y + 1, client.b.x - 1, client.b.y - 1);

    vScroll = new TScrollBar(TRect(client.b.x - 1, client.a.y, client.b.x, client.b.y - 1));
    vScroll->growMode = gfGrowHiY;
    hScroll = new TScrollBar(TRect(client.a.x, client.b.y - 1, client.b.x - 1, client.b.y));
    hScroll->growMode = gfGrowHiX;

    auto *view = new FileTypeListView(listBounds, hScroll, vScroll, entries);
    view->growMode = gfGrowHiX | gfGrowHiY;

    auto *header = new FileTypeHeaderView(headerBounds, *view);
    header->growMode = gfGrowHiX;

    view->setOwner(this);
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
    updateStatus();
}

void FileTypeWindow::refreshUnits()
{
    if (listView)
        listView->refreshMetrics();
    if (headerView)
        headerView->refresh();
    updateStatus();
}

void FileTypeWindow::refreshSort()
{
    entries = baseEntries;
    applySortToFileTypes(entries);
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
    updateStatus();
}

const FileTypeSummary *FileTypeWindow::selectedEntry() const
{
    if (!listView)
        return nullptr;
    return listView->currentEntry();
}

void FileTypeWindow::updateStatus()
{
    if (!getState(sfActive))
        return;
    if (const FileTypeSummary *entry = selectedEntry())
        app.showTypeSummary(*entry, recursiveMode);
    else
        app.showDefaultStatusHints();
}

void FileTypeWindow::handleEvent(TEvent &event)
{
    TWindow::handleEvent(event);
    if (event.what == evCommand && event.message.command == commands::ViewFilesForType)
    {
        openFilesForSelectedType();
        clearEvent(event);
    }
}

void FileTypeWindow::setState(ushort aState, Boolean enable)
{
    TWindow::setState(aState, enable);
    if ((aState & sfActive) != 0)
    {
        if (enable)
            updateStatus();
        else
            app.showDefaultStatusHints();
    }
}

void FileTypeWindow::openFilesForSelectedType()
{
    const FileTypeSummary *entry = selectedEntry();
    if (!entry)
        return;
    app.viewFilesForType(basePath, recursiveMode, entry->type, scanOptions);
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
      ck::ui::ClockAwareApplication(),
      optionRegistry(std::move(registry))
{
    insertMenuClock();

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
    cancelActiveFileList(true);
    cancelActiveFileType(true);
#if defined(__APPLE__)
    cancelActiveCloudOperation(true);
#endif
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
        case commands::ViewFiles:
            viewFiles(false);
            break;
        case commands::ViewFilesRecursive:
            viewFiles(true);
            break;
        case commands::ViewFileTypes:
            viewFileTypes(false);
            break;
        case commands::ViewFileTypesRecursive:
            viewFileTypes(true);
            break;
        case cmCopyPath:
            copySelectedPath();
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
        case cmReturnToLauncher:
            std::exit(ck::launcher::kReturnToLauncherExitCode);
            break;
#if defined(__APPLE__)
        case cmManageCloud:
            manageCloudStorage();
            break;
#endif
        case cmAbout:
        {
            const auto &info = toolInfo();
            ck::ui::showAboutDialog(info.executable, CK_DU_VERSION, info.aboutDescription);
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
    ck::ui::ClockAwareApplication::idle();
    processRescanRequests();
    if (activeScan)
    {
        updateScanProgress(*activeScan);
        if (activeScan->finished.load())
            processActiveScanCompletion();
    }
    else if (!pendingScanQueue.empty())
        startNextQueuedDirectory();

    if (activeFileList)
    {
        updateFileListProgress(*activeFileList);
        if (activeFileList->finished.load())
            processActiveFileListCompletion();
    }

    if (activeFileType)
    {
        updateFileTypeProgress(*activeFileType);
        if (activeFileType->finished.load())
            processActiveFileTypeCompletion();
    }

#if defined(__APPLE__)
    if (activeCloudOperation)
    {
        updateCloudOperationProgress(*activeCloudOperation);
        if (activeCloudOperation->finished.load())
            processCloudOperationCompletion();
    }
#endif

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
    TSubMenu &fileMenu = *new TSubMenu("~F~ile", hcNoContext) +
                         *new TMenuItem("~O~pen Directory", cmOpen, kbNoKey, hcOpen) +
                         *new TMenuItem("~C~lose", cmClose, kbNoKey, hcClose);
#if defined(__APPLE__)
    fileMenu + *new TMenuItem("Manage ~C~loud Storage...", cmManageCloud, kbNoKey, hcNoContext);
#endif
    fileMenu + newLine();
    if (ck::launcher::launchedFromCkLauncher())
        fileMenu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher, kbNoKey, hcNoContext);
    fileMenu + *new TMenuItem("E~x~it", cmQuit, kbNoKey, hcExit);

    TSubMenu &editMenu = *new TSubMenu("~E~dit", hcNoContext) +
                         *new TMenuItem("~C~opy Path", cmCopyPath, kbNoKey, hcNoContext);

    TMenuItem &menuChain = fileMenu + editMenu +
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
                               *new TMenuItem("~F~iles", commands::ViewFiles, kbNoKey, hcNoContext) +
                               *new TMenuItem("Files (~R~ecursive)", commands::ViewFilesRecursive, kbNoKey, hcNoContext) +
                               *new TMenuItem("~T~ypes", commands::ViewFileTypes, kbNoKey, hcNoContext) +
                               *new TMenuItem("Types (~S~ubdirs)", commands::ViewFileTypesRecursive, kbNoKey, hcNoContext) +
                          ck::ui::createWindowMenu() +
                          *new TSubMenu("~H~elp", hcNoContext) +
                               *new TMenuItem("~A~bout", cmAbout, kbNoKey, hcNoContext);

    ck::hotkeys::configureMenuTree(menuChain);
    return new TMenuBar(r, static_cast<TSubMenu &>(menuChain));
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

void DiskUsageApp::copySelectedPath()
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

    std::filesystem::path path = node->path;
    std::string text = path.string();
    copyTextToClipboard(text);
    showFilePath(path);
    std::string status = clipboardStatusMessage();
    messageBox(status.c_str(), mfInformation | mfOKButton);
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
    if (activeFileList)
    {
        if (!activeFileList->finished.load())
        {
            messageBox("A file listing is already in progress", mfInformation | mfOKButton);
            return;
        }
        processActiveFileListCompletion();
    }

    std::filesystem::path directory = node->path;
    std::string title = directory.filename().empty() ? directory.string() : directory.filename().string();
    if (title.empty())
        title = directory.string();
    title += recursive ? " (files + subdirs)" : " (files)";
    startFileListTask(directory, recursive, std::move(listOptions), std::move(title));
}

void DiskUsageApp::viewFileTypes(bool recursive)
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
    if (activeFileType)
    {
        if (!activeFileType->finished.load())
        {
            messageBox("A file type analysis is already in progress", mfInformation | mfOKButton);
            return;
        }
        processActiveFileTypeCompletion();
    }

    std::filesystem::path directory = node->path;
    std::string title = directory.filename().empty() ? directory.string() : directory.filename().string();
    if (title.empty())
        title = directory.string();
    title += recursive ? " (types + subdirs)" : " (types)";
    startFileTypeTask(directory, recursive, std::move(listOptions), std::move(title));
}

void DiskUsageApp::viewFilesForType(const std::filesystem::path &directory, bool recursive, const std::string &type,
                                    const BuildDirectoryTreeOptions &options)
{
    if (activeFileList)
    {
        if (!activeFileList->finished.load())
        {
            messageBox("A file listing is already in progress", mfInformation | mfOKButton);
            return;
        }
        processActiveFileListCompletion();
    }

    BuildDirectoryTreeOptions listOptions = options;
    std::string title = directory.filename().empty() ? directory.string() : directory.filename().string();
    if (title.empty())
        title = directory.string();
    title += recursive ? " (files + subdirs)" : " (files)";
    if (!type.empty())
        title += " — " + type;
    startFileListTask(directory, recursive, std::move(listOptions), std::move(title), type);
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

void DiskUsageApp::startFileListTask(const std::filesystem::path &directory, bool recursive,
                                     BuildDirectoryTreeOptions options, std::string title,
                                     std::optional<std::string> typeFilter)
{
    auto task = std::make_unique<FileListTask>();
    task->directory = directory;
    task->recursive = recursive;
    task->title = std::move(title);
    task->typeFilter = std::move(typeFilter);
    task->currentPath = directory.string();
    task->reportErrors = options.reportErrors;

    auto *dialog = new ScanProgressDialog("Listing Files", "Listing files...");
    dialog->setCancelHandler([this]() { requestFileListCancellation(); });
    task->dialog = dialog;
    deskTop->insert(dialog);
    dialog->drawView();
    dialog->updatePath(task->currentPath);

    FileListTask *rawTask = task.get();

    BuildDirectoryTreeOptions workerOptions = options;
    workerOptions.progressCallback = [rawTask](const std::filesystem::path &current) {
        std::lock_guard<std::mutex> lock(rawTask->mutex);
        rawTask->currentPath = current.string();
    };
    workerOptions.cancelRequested = [rawTask]() -> bool { return rawTask->cancelRequested.load(); };
    if (workerOptions.reportErrors)
    {
        workerOptions.errorCallback = [rawTask](const std::filesystem::path &path, const std::error_code &ec) {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            if (rawTask->errors.size() < 200)
            {
                std::string message = path.empty() ? std::string("(unknown)") : path.string();
                if (!ec.message().empty())
                    message += ": " + ec.message();
                rawTask->errors.push_back(std::move(message));
            }
        };
    }

    rawTask->worker = std::thread([this, rawTask, workerOptions]() mutable {
        std::vector<FileEntry> result;
        try
        {
            if (rawTask->typeFilter)
                result = listFilesByType(rawTask->directory, rawTask->recursive, *rawTask->typeFilter, workerOptions);
            else
                result = listFiles(rawTask->directory, rawTask->recursive, workerOptions);
        }
        catch (const std::exception &ex)
        {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            rawTask->failed = true;
            rawTask->errorMessage = ex.what();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            rawTask->failed = true;
            rawTask->errorMessage = "Unknown error";
        }

        if (rawTask->cancelRequested.load())
        {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            if (!rawTask->failed)
                rawTask->cancelled = true;
        }

        {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            if (!rawTask->cancelled && !rawTask->failed)
                rawTask->files = std::move(result);
        }

        rawTask->finished.store(true);
    });

    activeFileList = std::move(task);
}

void DiskUsageApp::startFileTypeTask(const std::filesystem::path &directory, bool recursive,
                                     BuildDirectoryTreeOptions options, std::string title)
{
    auto task = std::make_unique<FileTypeTask>();
    task->directory = directory;
    task->recursive = recursive;
    task->title = std::move(title);
    task->options = options;
    task->currentPath = directory.string();
    task->reportErrors = options.reportErrors;

    auto *dialog = new ScanProgressDialog("Analyzing File Types", "Analyzing file types...");
    dialog->setCancelHandler([this]() { requestFileTypeCancellation(); });
    task->dialog = dialog;
    deskTop->insert(dialog);
    dialog->drawView();
    dialog->updatePath(task->currentPath);

    FileTypeTask *rawTask = task.get();

    BuildDirectoryTreeOptions workerOptions = task->options;
    workerOptions.progressCallback = [rawTask](const std::filesystem::path &current) {
        std::lock_guard<std::mutex> lock(rawTask->mutex);
        rawTask->currentPath = current.string();
    };
    workerOptions.cancelRequested = [rawTask]() -> bool { return rawTask->cancelRequested.load(); };
    if (workerOptions.reportErrors)
    {
        workerOptions.errorCallback = [rawTask](const std::filesystem::path &path, const std::error_code &ec) {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            if (rawTask->errors.size() < 200)
            {
                std::string message = path.empty() ? std::string("(unknown)") : path.string();
                if (!ec.message().empty())
                    message += ": " + ec.message();
                rawTask->errors.push_back(std::move(message));
            }
        };
    }

    rawTask->worker = std::thread([this, rawTask, workerOptions]() mutable {
        std::vector<FileTypeSummary> result;
        try
        {
            result = summarizeFileTypes(rawTask->directory, rawTask->recursive, workerOptions);
        }
        catch (const std::exception &ex)
        {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            rawTask->failed = true;
            rawTask->errorMessage = ex.what();
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            rawTask->failed = true;
            rawTask->errorMessage = "Unknown error";
        }

        if (rawTask->cancelRequested.load())
        {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            if (!rawTask->failed)
                rawTask->cancelled = true;
        }

        {
            std::lock_guard<std::mutex> lock(rawTask->mutex);
            if (!rawTask->cancelled && !rawTask->failed)
                rawTask->types = std::move(result);
        }

        rawTask->finished.store(true);
    });

    activeFileType = std::move(task);
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

void DiskUsageApp::updateFileListProgress(FileListTask &task)
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

void DiskUsageApp::updateFileTypeProgress(FileTypeTask &task)
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

void DiskUsageApp::processActiveFileListCompletion()
{
    if (!activeFileList || !activeFileList->finished.load())
        return;

    if (activeFileList->worker.joinable())
        activeFileList->worker.join();

    std::vector<FileEntry> files;
    std::vector<std::string> errors;
    bool cancelled = false;
    bool failed = false;
    std::string errorMessage;
    bool recursive = activeFileList->recursive;
    std::string title = std::move(activeFileList->title);
    bool reportErrors = activeFileList->reportErrors;

    {
        std::lock_guard<std::mutex> lock(activeFileList->mutex);
        files = std::move(activeFileList->files);
        errors = std::move(activeFileList->errors);
        cancelled = activeFileList->cancelled;
        failed = activeFileList->failed;
        errorMessage = activeFileList->errorMessage;
    }

    closeProgressDialog(*activeFileList);

    activeFileList.reset();

    if (failed)
    {
        std::string message = errorMessage.empty() ? std::string("Failed to list files") : errorMessage;
        messageBox(message.c_str(), mfError | mfOKButton);
        return;
    }

    if (cancelled)
        return;

    auto *win = new FileListWindow(title, std::move(files), recursive, *this);
    deskTop->insert(win);
    win->drawView();

    if (reportErrors && !errors.empty())
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

void DiskUsageApp::processActiveFileTypeCompletion()
{
    if (!activeFileType || !activeFileType->finished.load())
        return;

    if (activeFileType->worker.joinable())
        activeFileType->worker.join();

    std::vector<FileTypeSummary> types;
    std::vector<std::string> errors;
    bool cancelled = false;
    bool failed = false;
    std::string errorMessage;
    std::filesystem::path directory = activeFileType->directory;
    bool recursive = activeFileType->recursive;
    std::string title = std::move(activeFileType->title);
    BuildDirectoryTreeOptions options = std::move(activeFileType->options);
    bool reportErrors = activeFileType->reportErrors;

    {
        std::lock_guard<std::mutex> lock(activeFileType->mutex);
        types = std::move(activeFileType->types);
        errors = std::move(activeFileType->errors);
        cancelled = activeFileType->cancelled;
        failed = activeFileType->failed;
        errorMessage = activeFileType->errorMessage;
    }

    closeProgressDialog(*activeFileType);

    activeFileType.reset();

    if (failed)
    {
        std::string message = errorMessage.empty() ? std::string("Failed to analyze file types") : errorMessage;
        messageBox(message.c_str(), mfError | mfOKButton);
        return;
    }

    if (cancelled)
        return;

    auto *win = new FileTypeWindow(title, std::move(directory), std::move(types), recursive, std::move(options), *this);
    deskTop->insert(win);
    win->drawView();

    if (reportErrors && !errors.empty())
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

void DiskUsageApp::requestFileListCancellation()
{
    if (!activeFileList)
        return;
    activeFileList->cancelRequested.store(true);
    closeProgressDialog(*activeFileList);
}

void DiskUsageApp::requestFileTypeCancellation()
{
    if (!activeFileType)
        return;
    activeFileType->cancelRequested.store(true);
    closeProgressDialog(*activeFileType);
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

void DiskUsageApp::closeProgressDialog(FileListTask &task)
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

void DiskUsageApp::closeProgressDialog(FileTypeTask &task)
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

void DiskUsageApp::cancelActiveFileList(bool waitForCompletion)
{
    if (!activeFileList)
        return;

    activeFileList->cancelRequested.store(true);
    if (waitForCompletion && activeFileList->worker.joinable())
        activeFileList->worker.join();

    closeProgressDialog(*activeFileList);
    activeFileList.reset();
}

void DiskUsageApp::cancelActiveFileType(bool waitForCompletion)
{
    if (!activeFileType)
        return;

    activeFileType->cancelRequested.store(true);
    if (waitForCompletion && activeFileType->worker.joinable())
        activeFileType->worker.join();

    closeProgressDialog(*activeFileType);
    activeFileType.reset();
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

void DiskUsageApp::registerTypeWindow(FileTypeWindow *window)
{
    typeWindows.push_back(window);
}

void DiskUsageApp::unregisterTypeWindow(FileTypeWindow *window)
{
    typeWindows.erase(std::remove(typeWindows.begin(), typeWindows.end(), window), typeWindows.end());
}

void DiskUsageApp::showDefaultStatusHints()
{
    if (auto *line = dynamic_cast<DiskUsageStatusLine *>(statusLine))
        line->showDefaultHints();
}

void DiskUsageApp::showFilePath(const std::filesystem::path &path)
{
    if (auto *line = dynamic_cast<DiskUsageStatusLine *>(statusLine))
        line->showMessage(path.string());
}

void DiskUsageApp::showFileDetails(const FileEntry &entry)
{
    if (auto *line = dynamic_cast<DiskUsageStatusLine *>(statusLine))
    {
        std::ostringstream out;
        out << entry.path.string() << " — " << formatFileUsage(entry);
        if (std::string state = describeICloudState(entry); !state.empty())
            out << " (" << state << ")";
        line->showMessage(out.str());
    }
}

void DiskUsageApp::showTypeSummary(const FileTypeSummary &summary, bool recursive)
{
    if (auto *line = dynamic_cast<DiskUsageStatusLine *>(statusLine))
    {
        std::ostringstream out;
        out << summary.type << " — " << summary.count << (summary.count == 1 ? " file" : " files")
            << ", " << formatUsageBreakdown(summary.totalSize, summary.cloudOnlySize, summary.logicalSize);
        if (summary.cloudOnlyCount > 0)
            out << " (" << summary.cloudOnlyCount << " cloud-only)";
        if (recursive)
            out << " (including subdirectories)";
        out << " — Press Enter to view files";
        line->showMessage(out.str());
    }
}

void DiskUsageApp::notifyUnitsChanged()
{
    for (auto *win : directoryWindows)
        if (win)
            win->refreshLabels();
    for (auto *win : fileWindows)
        if (win)
            win->refreshUnits();
    for (auto *win : typeWindows)
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
    for (auto *win : typeWindows)
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

#if defined(__APPLE__)
void DiskUsageApp::manageCloudStorage()
{
    DirectoryWindow *window = activeDirectoryWindow();
    if (!window)
    {
        messageBox("Open a directory view before managing cloud storage.", mfInformation | mfOKButton);
        return;
    }
    DirectoryNode *node = window->focusedNode();
    if (!node)
    {
        messageBox("Select a directory in the tree before managing cloud storage.", mfInformation | mfOKButton);
        return;
    }

    CloudUsageSnapshot usage;
    usage.totalFiles = node->stats.fileCount;
    usage.cloudOnlyFiles = node->stats.cloudOnlyFileCount;
    if (usage.totalFiles >= usage.cloudOnlyFiles)
        usage.localFiles = usage.totalFiles - usage.cloudOnlyFiles;
    usage.localBytes = node->stats.totalSize;
    usage.cloudBytes = node->stats.cloudOnlySize;
    usage.logicalBytes = node->stats.logicalSize;

    bool canPause = cloud::supportsPauseResume(node->path);
    auto definitions = buildCloudOperationDefinitions(usage, canPause);
    CloudDialogSelection selection;
    auto *dialog = new ManageCloudDialog(node->path, usage, std::move(definitions), &selection);
    if (TProgram::application->executeDialog(dialog, nullptr) != cmOK || !selection.confirmed)
        return;

    std::ostringstream confirm;
    confirm << selection.definition.label << "\n\n"
            << selection.definition.explanation << "\n"
            << selection.definition.impact << "\n\nProceed?";

    if (messageBox(confirm.str().c_str(), mfConfirmation | mfYesButton | mfNoButton) != cmYes)
        return;

    startCloudOperation(selection.action, selection.definition, usage, node->path);
}

void DiskUsageApp::startCloudOperation(CloudActionKind action, const CloudOperationDefinition &definition,
                                       const CloudUsageSnapshot &usage, const std::filesystem::path &path)
{
    if (activeCloudOperation && !activeCloudOperation->finished.load())
    {
        messageBox("Another cloud operation is already running.", mfInformation | mfOKButton);
        return;
    }
    if (activeCloudOperation && activeCloudOperation->finished.load())
        processCloudOperationCompletion();
    cancelActiveCloudOperation(true);

    if (action == CloudActionKind::RevealInFinder)
    {
        auto result = cloud::revealInFinder(path);
        if (!result.success)
        {
            messageBox(result.errorMessage.c_str(), mfError | mfOKButton);
        }
        return;
    }

    auto task = std::make_unique<CloudOperationTask>();
    task->action = action;
    task->definition = definition;
    task->usage = usage;
    task->rootPath = path;
    task->statusMessage = "Starting operation...";
    task->progress.totalItems = cloudOperationItemTarget(action, usage);
    task->progress.totalBytes = cloudOperationByteTarget(action, usage);
    if (task->progress.totalItems == 0)
        task->progress.totalItems = 1;
    task->recursive = true;

    std::string title = definition.label;
    auto *dialog = new CloudOperationProgressDialog(title.c_str(), definition.explanation.c_str());
    dialog->setCancelHandler([this]() { requestCloudOperationCancel(); });
    dialog->setPauseHandler([this]() { requestCloudOperationPause(); });
    dialog->setResumeHandler([this]() { requestCloudOperationResume(); });
    task->dialog = dialog;
    deskTop->insert(dialog);
    dialog->drawView();
    dialog->update(task->progress, false, task->statusMessage);

    CloudOperationTask *rawTask = task.get();
    rawTask->worker = std::thread([this, rawTask]() { runCloudOperation(*rawTask); });

    activeCloudOperation = std::move(task);
}

void DiskUsageApp::updateCloudOperationProgress(CloudOperationTask &task)
{
    if (!task.dialog)
        return;
    CloudOperationProgress snapshot;
    std::string status;
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        snapshot = task.progress;
        status = task.statusMessage;
    }
    bool paused = task.paused.load() || task.pauseRequested.load();
    task.dialog->update(snapshot, paused, status);
}

void DiskUsageApp::closeCloudOperationDialog(CloudOperationTask &task)
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

void DiskUsageApp::processCloudOperationCompletion()
{
    if (!activeCloudOperation)
        return;
    CloudOperationTask &task = *activeCloudOperation;
    if (task.worker.joinable())
        task.worker.join();
    closeCloudOperationDialog(task);
    std::string status;
    bool failed = task.failed;
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        status = failed && !task.errorMessage.empty() ? task.errorMessage : task.statusMessage;
    }
    if (status.empty())
        status = failed ? "Cloud operation failed." : "Cloud operation finished.";
    // Reset before showing the modal message box to avoid re-entrancy
    // from idle() while the dialog is open.
    activeCloudOperation.reset();
    messageBox(status.c_str(), (failed ? mfError : mfInformation) | mfOKButton);
}

void DiskUsageApp::runCloudOperation(CloudOperationTask &task)
{
    try
    {
        cloud::OperationCallbacks callbacks;
        callbacks.isCancelled = [&]() {
            while (task.pauseRequested.load() && !task.cancelRequested.load())
            {
                task.paused.store(true);
                {
                    std::lock_guard<std::mutex> lock(task.mutex);
                    task.statusMessage = "Paused.";
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!task.cancelRequested.load())
            {
                task.paused.store(false);
                std::lock_guard<std::mutex> lock(task.mutex);
                if (task.statusMessage == "Paused.")
                    task.statusMessage = "Working...";
            }
            return task.cancelRequested.load();
        };
        callbacks.onStatus = [&](const std::string &status) {
            std::lock_guard<std::mutex> lock(task.mutex);
            task.statusMessage = status;
        };
        callbacks.onItem = [&](const std::filesystem::path &item, std::uintmax_t bytes) -> bool {
            if (task.cancelRequested.load())
                return false;
            std::lock_guard<std::mutex> lock(task.mutex);
            if (task.progress.totalItems == 0)
                task.progress.totalItems = 1;
            task.progress.processedItems += 1;
            if (task.progress.processedItems > task.progress.totalItems)
                task.progress.totalItems = task.progress.processedItems;
            if (task.progress.totalBytes > 0)
            {
                task.progress.processedBytes = std::min<std::uintmax_t>(task.progress.totalBytes,
                                                                         task.progress.processedBytes + bytes);
            }
            else
            {
                task.progress.processedBytes += bytes;
            }
            task.progress.currentItem = item.string();
            return !task.cancelRequested.load();
        };

        auto result = cloud::performCloudOperation(task.action, task.rootPath, callbacks, task.recursive);

        {
            std::lock_guard<std::mutex> lock(task.mutex);
            if (result.processedItems > task.progress.processedItems)
                task.progress.processedItems = result.processedItems;
            if (result.processedBytes > task.progress.processedBytes)
                task.progress.processedBytes = result.processedBytes;
            if (task.progress.totalItems < task.progress.processedItems)
                task.progress.totalItems = task.progress.processedItems;
            if (task.progress.totalBytes < task.progress.processedBytes)
                task.progress.totalBytes = task.progress.processedBytes;

            if (result.cancelled || task.cancelRequested.load())
            {
                task.statusMessage = "Cloud operation cancelled.";
                task.cancelRequested.store(true);
            }
            else if (!result.success)
            {
                task.failed = true;
                task.errorMessage = result.errorMessage.empty() ? "Cloud operation failed." : result.errorMessage;
                task.statusMessage = task.errorMessage;
            }
            else
            {
                task.statusMessage = "Cloud operation complete.";
            }
        }
    }
    catch (const std::exception &ex)
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.failed = true;
        task.errorMessage = std::string("Cloud operation failed: ") + ex.what();
        task.statusMessage = task.errorMessage;
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.failed = true;
        task.errorMessage = "Cloud operation failed with an unknown error.";
        task.statusMessage = task.errorMessage;
    }

    task.paused.store(false);
    task.finished.store(true);
}

void DiskUsageApp::requestCloudOperationPause()
{
    if (!activeCloudOperation)
        return;
    activeCloudOperation->pauseRequested.store(true);
    {
        std::lock_guard<std::mutex> lock(activeCloudOperation->mutex);
        activeCloudOperation->statusMessage = "Pausing...";
    }
}

void DiskUsageApp::requestCloudOperationResume()
{
    if (!activeCloudOperation)
        return;
    activeCloudOperation->pauseRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(activeCloudOperation->mutex);
        activeCloudOperation->statusMessage = "Resuming...";
    }
}

void DiskUsageApp::requestCloudOperationCancel()
{
    if (!activeCloudOperation)
        return;
    activeCloudOperation->cancelRequested.store(true);
    activeCloudOperation->pauseRequested.store(false);
    {
        std::lock_guard<std::mutex> lock(activeCloudOperation->mutex);
        activeCloudOperation->statusMessage = "Cancelling...";
    }
}

void DiskUsageApp::cancelActiveCloudOperation(bool waitForCompletion)
{
    if (!activeCloudOperation)
        return;
    activeCloudOperation->cancelRequested.store(true);
    activeCloudOperation->pauseRequested.store(false);
    if (waitForCompletion && activeCloudOperation->worker.joinable())
        activeCloudOperation->worker.join();
    closeCloudOperationDialog(*activeCloudOperation);
    activeCloudOperation.reset();
}
#endif

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

    ck::hotkeys::registerDefaultSchemes();
    ck::hotkeys::initializeFromEnvironment();
    ck::hotkeys::applyCommandLineScheme(argc, argv);

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
        const auto &info = toolInfo();
        std::cout << info.executable << " - " << info.shortDescription << "\n\n"
                  << "Usage: " << info.executable << " [options] [paths...]\n"
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
                  << "  --hotkeys SCHEME       Use the specified hotkey scheme for this run\n\n"
                  << "Available schemes: linux, mac, windows, custom.\n"
                  << "Set CK_HOTKEY_SCHEME to choose a default hotkey scheme." << std::endl;
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
#if defined(__APPLE__)
std::optional<CloudOperationDefinition> ManageCloudDialog::selectedDefinition() const
{
    if (chosenIndex < 0 || static_cast<std::size_t>(chosenIndex) >= actions.size())
        return std::nullopt;
    return actions[static_cast<std::size_t>(chosenIndex)];
}
#endif // defined(__APPLE__)
