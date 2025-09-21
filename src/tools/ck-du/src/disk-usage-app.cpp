#include "disk_usage_core.hpp"

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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#include <functional>
#include <filesystem>
#include <iomanip>
#include <limits.h>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace ck::du;

static constexpr const char *kDeveloperName = "Dr. C. Klukas";

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

namespace
{
std::array<TMenuItem *, 7> gUnitMenuItems{};
}

namespace
{
std::array<TMenuItem *, 7> gSortMenuItems{};
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
    virtual TPalette &getPalette() const override;

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
    virtual TPalette &getPalette() const override;
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
    DirectoryWindow(const std::filesystem::path &path, std::unique_ptr<DirectoryNode> rootNode, class DiskUsageApp &app);
    ~DirectoryWindow();

    DirectoryNode *focusedNode() const;
    void refreshLabels();
    void refreshSort();

private:
    class DiskUsageApp &app;
    std::unique_ptr<DirectoryNode> root;
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
        auto *quit = new TStatusItem("~Alt-X~ Quit", kbAltX, cmQuit);
        open->next = files;
        files->next = recursive;
        recursive->next = quit;
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
    DiskUsageApp(int argc, char **argv);
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

TPalette &FileListView::getPalette() const
{
    static const char paletteData[] = "\x1F\x17\x3F\x70\x1F";
    static TPalette palette(paletteData, sizeof(paletteData) - 1);
    return palette;
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

TPalette &FileListHeaderView::getPalette() const
{
    return listView.getPalette();
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
    TRect r = getExtent();
    r.grow(-1, -1);
    if (r.b.x <= r.a.x + 2 || r.b.y <= r.a.y + 3)
        r = TRect(0, 0, 76, 18);

    TRect headerBounds(r.a.x, r.a.y, r.b.x - 1, r.a.y + 1);
    TRect listBounds(r.a.x, r.a.y + 1, r.b.x - 1, r.b.y - 1);

    vScroll = new TScrollBar(TRect(r.b.x - 1, r.a.y, r.b.x, r.b.y - 1));
    vScroll->growMode = gfGrowHiY;
    hScroll = new TScrollBar(TRect(r.a.x, r.b.y - 1, r.b.x - 1, r.b.y));
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

DirectoryWindow::DirectoryWindow(const std::filesystem::path &path, std::unique_ptr<DirectoryNode> rootNode, DiskUsageApp &appRef)
    : TWindowInit(&TWindow::initFrame),
      TWindow(TRect(0, 0, 78, 20), path.filename().empty() ? path.string().c_str() : path.filename().string().c_str(), wnNoNumber),
      app(appRef), root(std::move(rootNode))
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

    TRect r = getExtent();
    r.grow(-1, -1);
    if (r.b.x <= r.a.x + 2 || r.b.y <= r.a.y + 2)
        r = TRect(0, 0, 76, 18);

    vScroll = new TScrollBar(TRect(r.b.x - 1, r.a.y, r.b.x, r.b.y));
    vScroll->growMode = gfGrowHiY;
    hScroll = new TScrollBar(TRect(r.a.x, r.b.y - 1, r.b.x - 1, r.b.y));
    hScroll->growMode = gfGrowHiX;

    auto *view = new DirectoryOutline(TRect(r.a.x, r.a.y, r.b.x - 1, r.b.y - 1), hScroll, vScroll, rootNode, *this);
    view->growMode = gfGrowHiX | gfGrowHiY;
    insert(vScroll);
    insert(hScroll);
    insert(view);
    outline = view;
    outline->update();
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

DiskUsageApp::DiskUsageApp(int argc, char **argv)
    : TProgInit(&DiskUsageApp::initStatusLine, &DiskUsageApp::initMenuBar, &TApplication::initDeskTop),
      TApplication()
{
    unitBaseLabels = {{SizeUnit::Auto, "Auto"},
                      {SizeUnit::Bytes, "Bytes"},
                      {SizeUnit::Kilobytes, "Kilobytes"},
                      {SizeUnit::Megabytes, "Megabytes"},
                      {SizeUnit::Gigabytes, "Gigabytes"},
                      {SizeUnit::Terabytes, "Terabytes"},
                      {SizeUnit::Blocks, "Blocks"}};
    sortBaseLabels = {{SortKey::Unsorted, "Unsorted"},
                      {SortKey::NameAscending, "Name (A→Z)"},
                      {SortKey::NameDescending, "Name (Z→A)"},
                      {SortKey::SizeDescending, "Size (Largest)"},
                      {SortKey::SizeAscending, "Size (Smallest)"},
                      {SortKey::ModifiedDescending, "Modified (Newest)"},
                      {SortKey::ModifiedAscending, "Modified (Oldest)"}};
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

    for (int i = 1; i < argc; ++i)
        queueDirectoryForScan(argv[i]);
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
        case cmAbout:
        {
            std::string msg = std::string("ck-du ") + CK_DU_VERSION +
                              "\nDeveloper: " + kDeveloperName;
            messageBox(msg.c_str(), mfInformation | mfOKButton);
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
    auto *unitAuto = new TMenuItem("Auto", cmUnitAuto, kbNoKey, hcNoContext);
    auto *unitBytes = new TMenuItem("Bytes", cmUnitBytes, kbNoKey, hcNoContext);
    auto *unitKB = new TMenuItem("Kilobytes", cmUnitKB, kbNoKey, hcNoContext);
    auto *unitMB = new TMenuItem("Megabytes", cmUnitMB, kbNoKey, hcNoContext);
    auto *unitGB = new TMenuItem("Gigabytes", cmUnitGB, kbNoKey, hcNoContext);
    auto *unitTB = new TMenuItem("Terabytes", cmUnitTB, kbNoKey, hcNoContext);
    auto *unitBlocks = new TMenuItem("Blocks", cmUnitBlocks, kbNoKey, hcNoContext);
    gUnitMenuItems = {unitAuto, unitBytes, unitKB, unitMB, unitGB, unitTB, unitBlocks};
    auto *sortUnsorted = new TMenuItem("Unsorted", cmSortUnsorted, kbNoKey, hcNoContext);
    auto *sortNameAsc = new TMenuItem("Name (A→Z)", cmSortNameAsc, kbNoKey, hcNoContext);
    auto *sortNameDesc = new TMenuItem("Name (Z→A)", cmSortNameDesc, kbNoKey, hcNoContext);
    auto *sortSizeDesc = new TMenuItem("Size (Largest)", cmSortSizeDesc, kbNoKey, hcNoContext);
    auto *sortSizeAsc = new TMenuItem("Size (Smallest)", cmSortSizeAsc, kbNoKey, hcNoContext);
    auto *sortModifiedDesc = new TMenuItem("Modified (Newest)", cmSortModifiedDesc, kbNoKey, hcNoContext);
    auto *sortModifiedAsc = new TMenuItem("Modified (Oldest)", cmSortModifiedAsc, kbNoKey, hcNoContext);
    gSortMenuItems = {sortUnsorted, sortNameAsc, sortNameDesc, sortSizeDesc, sortSizeAsc, sortModifiedDesc, sortModifiedAsc};
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

    if (executeDialog(d, &data) != cmCancel)
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

    std::vector<FileEntry> files = listFiles(node->path, recursive);
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
    std::filesystem::path rootPath = activeScan->rootPath;
    {
        std::lock_guard<std::mutex> lock(activeScan->mutex);
        result = std::move(activeScan->result);
        cancelled = activeScan->cancelled;
        failed = activeScan->failed;
        errorMessage = activeScan->errorMessage;
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
        auto *win = new DirectoryWindow(rootPath, std::move(result), *this);
        deskTop->insert(win);
        win->drawView();
    }

    startNextQueuedDirectory();
}

void DiskUsageApp::runDirectoryScan(DirectoryScanTask &task)
{
    BuildDirectoryTreeOptions options;
    options.progressCallback = [&](const std::filesystem::path &current) {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.currentPath = current.string();
    };
    options.cancelRequested = [&]() -> bool { return task.cancelRequested.load(); };

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

int main(int argc, char **argv)
{
    DiskUsageApp app(argc, argv);
    app.run();
    return 0;
}

