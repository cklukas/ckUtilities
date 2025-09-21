#include "ck/app_info.hpp"

#define Uses_TApplication
#define Uses_TButton
#define Uses_TDeskTop
#define Uses_TDialog
#define Uses_TDrawBuffer
#define Uses_TKeys
#define Uses_TListViewer
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TMessageBox
#define Uses_MsgBox
#define Uses_TScrollBar
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#include <tvision/tv.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{

constexpr std::string_view kLauncherId = "ck-utilities";

const ck::appinfo::ToolInfo &launcherInfo()
{
    return ck::appinfo::requireTool(kLauncherId);
}

constexpr ushort cmLaunchTool = 6000;

std::vector<std::string> wrapText(std::string_view text, int width)
{
    std::vector<std::string> lines;
    if (width <= 0)
    {
        if (!text.empty())
            lines.emplace_back(text);
        return lines;
    }

    std::string currentLine;
    std::size_t pos = 0;
    while (pos < text.size())
    {
        unsigned char ch = static_cast<unsigned char>(text[pos]);
        if (ch == '\r')
        {
            ++pos;
            continue;
        }
        if (ch == '\n')
        {
            if (!currentLine.empty())
            {
                lines.push_back(currentLine);
                currentLine.clear();
            }
            else if (lines.empty() || !lines.back().empty())
            {
                lines.emplace_back();
            }
            ++pos;
            continue;
        }
        if (std::isspace(ch))
        {
            ++pos;
            continue;
        }

        std::size_t wordEnd = pos;
        while (wordEnd < text.size())
        {
            unsigned char wc = static_cast<unsigned char>(text[wordEnd]);
            if (wc == '\n' || std::isspace(wc))
                break;
            ++wordEnd;
        }

        std::string_view word = text.substr(pos, wordEnd - pos);
        pos = wordEnd;

        if (static_cast<int>(word.size()) >= width)
        {
            if (!currentLine.empty())
            {
                lines.push_back(currentLine);
                currentLine.clear();
            }
            std::size_t offset = 0;
            while (offset < word.size())
            {
                std::size_t chunkLen = std::min<std::size_t>(width, word.size() - offset);
                lines.emplace_back(word.substr(offset, chunkLen));
                offset += chunkLen;
            }
            continue;
        }

        if (currentLine.empty())
        {
            currentLine.assign(word);
        }
        else if (static_cast<int>(currentLine.size() + 1 + word.size()) <= width)
        {
            currentLine.push_back(' ');
            currentLine.append(word);
        }
        else
        {
            lines.push_back(currentLine);
            currentLine.assign(word);
        }
    }

    if (!currentLine.empty())
        lines.push_back(currentLine);

    return lines;
}

std::vector<std::string> splitBannerLines()
{
    std::vector<std::string> lines;
    std::string current;
    for (char ch : ck::appinfo::kProjectBanner)
    {
        if (ch == '\n')
        {
            lines.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(ch);
        }
    }
    lines.push_back(std::move(current));
    return lines;
}

class BannerView : public TView
{
public:
    BannerView(const TRect &bounds, std::vector<std::string> lines)
        : TView(bounds), bannerLines(std::move(lines))
    {
        growMode = gfGrowHiX;
    }

    virtual void changeBounds(const TRect &bounds) override
    {
        TView::changeBounds(bounds);
        drawView();
    }

    virtual void draw() override
    {
        TDrawBuffer buffer;
        TColorAttr background = TColorAttr(0x70);     // Light gray background.
        TColorAttr blueText = TColorAttr(0x71);       // Blue text on light gray.
        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', background, size.x);
            int lineIndex = y - 1; // First row stays empty so banner sits one line down.
            if (lineIndex >= 0 && lineIndex < static_cast<int>(bannerLines.size()))
            {
                const std::string &line = bannerLines[static_cast<std::size_t>(lineIndex)];
                int glyphWidth = static_cast<int>(line.size());
                int start = glyphWidth >= size.x ? 0 : std::max(0, (size.x - glyphWidth) / 2);
                int maxWidth = std::max(0, size.x - start);
                if (maxWidth > 0)
                {
                    TStringView view{line};
                    if (static_cast<int>(view.size()) > maxWidth)
                        view = view.substr(0, static_cast<std::size_t>(maxWidth));
                    buffer.moveStr(start, view, blueText);
                }
            }
            writeLine(0, y, size.x, 1, buffer);
        }
    }

private:
    std::vector<std::string> bannerLines;
};

class ToolDetailView : public TView
{
public:
    explicit ToolDetailView(const TRect &bounds)
        : TView(bounds)
    {
        growMode = gfGrowHiX | gfGrowHiY;
    }

    void setTool(const ck::appinfo::ToolInfo *info)
    {
        selected = info;
        rebuildLines();
        drawView();
    }

    virtual void changeBounds(const TRect &bounds) override
    {
        TView::changeBounds(bounds);
        rebuildLines();
    }

    virtual void draw() override
    {
        TDrawBuffer buffer;
        ushort color = getColor(0x0301);
        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', color, size.x);
            if (y < static_cast<int>(wrappedLines.size()))
            {
                buffer.moveStr(1, wrappedLines[static_cast<std::size_t>(y)].c_str(), color);
            }
            writeLine(0, y, size.x, 1, buffer);
        }
    }

private:
    const ck::appinfo::ToolInfo *selected = nullptr;
    std::vector<std::string> wrappedLines;

    void rebuildLines()
    {
        wrappedLines.clear();
        int width = size.x - 2;
        if (width < 1)
            width = size.x > 0 ? size.x : 1;

        if (!selected)
        {
            wrappedLines.emplace_back("Select a tool on the left to view details and launch it.");
            return;
        }

        std::string title = std::string(selected->displayName);
        title.append(" (" + std::string(selected->executable) + ")");
        wrappedLines.push_back(std::move(title));

        auto summary = wrapText(selected->shortDescription, width);
        wrappedLines.insert(wrappedLines.end(), summary.begin(), summary.end());
        wrappedLines.emplace_back();

        auto doc = wrapText(selected->longDescription, width);
        wrappedLines.insert(wrappedLines.end(), doc.begin(), doc.end());
    }
};

class ToolListView : public TListViewer
{
public:
    ToolListView(const TRect &bounds,
                 std::vector<const ck::appinfo::ToolInfo *> &entriesRef,
                 TScrollBar *vScroll)
        : TListViewer(bounds, 1, nullptr, vScroll), entries(&entriesRef)
    {
        growMode = gfGrowHiY;
        updateRange();
    }

    void updateRange()
    {
        if (!entries)
            return;
        setRange(static_cast<short>(entries->size()));
    }

    short currentIndex() const { return focused; }

    const ck::appinfo::ToolInfo *toolAt(short index) const
    {
        if (!entries || index < 0 || index >= static_cast<short>(entries->size()))
            return nullptr;
        return (*entries)[static_cast<std::size_t>(index)];
    }

    virtual void getText(char *dest, short item, short maxChars) override
    {
        if (!entries || item < 0 || item >= static_cast<short>(entries->size()))
        {
            if (maxChars > 0)
                dest[0] = '\0';
            return;
        }
        const auto *info = (*entries)[static_cast<std::size_t>(item)];
        std::snprintf(dest, static_cast<std::size_t>(maxChars), "%s", info->displayName.data());
    }

    virtual void handleEvent(TEvent &event) override
    {
        TListViewer::handleEvent(event);
        if (event.what == evKeyDown && event.keyDown.keyCode == kbEnter)
        {
            message(owner, evCommand, cmLaunchTool, this);
            clearEvent(event);
        }
    }

private:
    std::vector<const ck::appinfo::ToolInfo *> *entries;
};

class LauncherDialog : public TDialog
{
public:
    LauncherDialog(const TRect &bounds, std::vector<const ck::appinfo::ToolInfo *> tools)
        : TWindowInit(&TDialog::initFrame),
          TDialog(bounds, launcherInfo().displayName.data()),
          bannerLines(splitBannerLines()),
          toolRefs(std::move(tools))
    {
        flags |= wfGrow;
        growMode = gfGrowHiX | gfGrowHiY;

        bannerView = new BannerView(TRect(0, 0, 1, 1), bannerLines);
        bannerView->growMode = gfGrowHiX;
        insert(bannerView);

        vScroll = new TScrollBar(TRect(0, 0, 0, 0));
        vScroll->growMode = gfGrowHiY;
        insert(vScroll);

        listView = new ToolListView(TRect(0, 0, 0, 0), toolRefs, vScroll);
        listView->growMode = gfGrowHiY;
        insert(listView);

        detailView = new ToolDetailView(TRect(0, 0, 0, 0));
        insert(detailView);

        launchButton = new TButton(TRect(0, 0, 0, 0), "~L~aunch", cmLaunchTool, bfDefault);
        insert(launchButton);

        layoutChildren();

        if (!toolRefs.empty())
            listView->focusItem(0);

        ensureDetailUpdated();
    }

    const ck::appinfo::ToolInfo *currentTool() const
    {
        if (!listView)
            return nullptr;
        return listView->toolAt(listView->currentIndex());
    }

    virtual void handleEvent(TEvent &event) override
    {
        TDialog::handleEvent(event);
        if (event.what == evCommand && event.message.command == cmLaunchTool)
        {
            message(TProgram::application, evCommand, cmLaunchTool, this);
            clearEvent(event);
            return;
        }
        ensureDetailUpdated();
    }

    virtual void changeBounds(const TRect &bounds) override
    {
        TDialog::changeBounds(bounds);
        layoutChildren();
        ensureDetailUpdated();
    }

private:
    std::vector<std::string> bannerLines;
    BannerView *bannerView = nullptr;
    std::vector<const ck::appinfo::ToolInfo *> toolRefs;
    ToolListView *listView = nullptr;
    ToolDetailView *detailView = nullptr;
    TScrollBar *vScroll = nullptr;
    TButton *launchButton = nullptr;
    short lastIndex = -1;

    void ensureDetailUpdated()
    {
        if (!listView || !detailView)
            return;
        short index = listView->currentIndex();
        if (index == lastIndex)
            return;
        lastIndex = index;
        const ck::appinfo::ToolInfo *info = listView->toolAt(index);
        detailView->setTool(info);
        if (launchButton)
            launchButton->setState(sfDisabled, info == nullptr);
    }

    void layoutChildren()
    {
        if (!bannerView || !detailView)
            return;

        TRect extent = getExtent();
        extent.grow(-2, -1);
        if (extent.b.x <= extent.a.x || extent.b.y <= extent.a.y)
            return;

        int areaWidth = extent.b.x - extent.a.x;
        int areaHeight = extent.b.y - extent.a.y;

        int desiredBannerHeight = static_cast<int>(bannerLines.size()) + 1;
        int bannerHeight = std::clamp(desiredBannerHeight, 1, std::max(1, areaHeight - 4));
        TRect bannerRect(extent.a.x, extent.a.y, extent.b.x, std::min(extent.a.y + bannerHeight, extent.b.y));

        int contentTop = std::min(bannerRect.b.y + 1, extent.b.y);
        if (contentTop >= extent.b.y)
            contentTop = std::max(extent.b.y - 3, extent.a.y);

        TRect contentRect(extent.a.x, contentTop, extent.b.x, extent.b.y);
        if (contentRect.b.y <= contentRect.a.y)
            contentRect.a.y = std::max(extent.a.y, extent.b.y - 3);

        int buttonHeight = std::min(2, contentRect.b.y - contentRect.a.y);
        if (buttonHeight < 1)
            buttonHeight = 1;
        int buttonTop = contentRect.b.y - buttonHeight;

        TRect mainRect(contentRect.a.x, contentRect.a.y, contentRect.b.x, buttonTop);
        if (mainRect.b.y <= mainRect.a.y)
            mainRect.b.y = contentRect.a.y;

        int mainWidth = std::max(0, mainRect.b.x - mainRect.a.x);
        int listWidth = std::clamp(mainWidth / 3, 18, std::max(18, mainWidth - 24));
        if (listWidth + 24 > mainWidth)
            listWidth = std::max(12, mainWidth - 24);
        if (listWidth < 12)
            listWidth = std::max(12, mainWidth / 2);

        int listRight = std::min(mainRect.a.x + listWidth, mainRect.b.x - 12);
        if (listRight <= mainRect.a.x)
            listRight = std::min(mainRect.a.x + std::max(10, mainWidth / 2), mainRect.b.x - 1);

        TRect listRect(mainRect.a.x, mainRect.a.y, listRight, mainRect.b.y);
        if (listRect.b.x < listRect.a.x)
            listRect.b.x = listRect.a.x;

        int scrollWidth = (mainRect.b.x - listRect.b.x > 1) ? 1 : 0;
        TRect scrollRect(listRect.b.x, mainRect.a.y, listRect.b.x + scrollWidth, mainRect.b.y);

        int detailLeft = scrollRect.b.x + (scrollWidth > 0 ? 1 : 0);
        if (detailLeft > mainRect.b.x)
            detailLeft = mainRect.b.x;
        TRect detailRect(detailLeft, mainRect.a.y, mainRect.b.x, mainRect.b.y);
        if (detailRect.b.x <= detailRect.a.x)
            detailRect.a.x = std::max(detailRect.b.x - 20, mainRect.a.x);

        int buttonWidth = std::min(18, detailRect.b.x - detailRect.a.x);
        if (buttonWidth < 8)
            buttonWidth = std::max(8, detailRect.b.x - detailRect.a.x);
        int buttonLeft = detailRect.b.x - buttonWidth;
        if (buttonLeft < detailRect.a.x)
            buttonLeft = detailRect.a.x;
        TRect launchRect(buttonLeft, buttonTop, detailRect.b.x, std::min(contentRect.b.y, buttonTop + buttonHeight));
        if (launchRect.b.y <= launchRect.a.y)
            launchRect.b.y = launchRect.a.y + 1;

        bannerView->locate(bannerRect);
        if (listView)
            listView->locate(listRect);
        if (vScroll)
        {
            if (scrollWidth <= 0)
                vScroll->hide();
            else
            {
                vScroll->show();
                vScroll->locate(scrollRect);
            }
        }
        if (detailView)
            detailView->locate(detailRect);
        if (launchButton)
            launchButton->locate(launchRect);
    }
};

class LauncherApp : public TApplication
{
public:
    LauncherApp(int argc, char **argv)
        : TProgInit(&LauncherApp::initStatusLine, &LauncherApp::initMenuBar, &TApplication::initDeskTop),
          TApplication()
    {
        toolDirectory = resolveToolDirectory(argc > 0 ? argv[0] : nullptr);
        openLauncher();
    }

    virtual void handleEvent(TEvent &event) override
    {
        TApplication::handleEvent(event);
        if (event.what == evCommand && event.message.command == cmLaunchTool)
        {
            auto *dialog = static_cast<LauncherDialog *>(event.message.infoPtr);
            if (dialog)
                launchTool(dialog->currentTool());
            clearEvent(event);
        }
    }

    static TMenuBar *initMenuBar(TRect r)
    {
        r.b.y = r.a.y + 1;
        return new TMenuBar(r,
                            *new TSubMenu("~F~ile", kbAltF) +
                                *new TMenuItem("~E~xit", cmQuit, kbAltX));
    }

    static TStatusLine *initStatusLine(TRect r)
    {
        r.a.y = r.b.y - 1;
        auto *launch = new TStatusItem("~Enter~ Launch", kbEnter, cmLaunchTool);
        auto *exitItem = new TStatusItem("~Alt-X~ Exit", kbAltX, cmQuit);
        launch->next = exitItem;
        return new TStatusLine(r, *new TStatusDef(0, 0xFFFF, launch));
    }

private:
    std::filesystem::path toolDirectory;

    void openLauncher()
    {
        std::vector<const ck::appinfo::ToolInfo *> tools;
        for (const auto &info : ck::appinfo::tools())
        {
            if (info.id == kLauncherId)
                continue;
            tools.push_back(&info);
        }
        std::sort(tools.begin(), tools.end(), [](const auto *a, const auto *b) {
            return a->displayName < b->displayName;
        });

        TRect desktopExtent = deskTop->getExtent();
        TRect dialogBounds = desktopExtent;

        if (dialogBounds.b.x - dialogBounds.a.x > 2)
        {
            ++dialogBounds.a.x;
            --dialogBounds.b.x;
        }
        if (dialogBounds.b.y - dialogBounds.a.y > 2)
        {
            ++dialogBounds.a.y;
            --dialogBounds.b.y;
        }

        auto *dialog = new LauncherDialog(dialogBounds, std::move(tools));
        deskTop->insert(dialog);
        dialog->select();
    }

    void launchTool(const ck::appinfo::ToolInfo *info)
    {
        if (!info)
            return;

        std::filesystem::path programPath = toolDirectory / std::filesystem::path(info->executable);
        std::error_code ec;
        std::filesystem::path resolved = std::filesystem::weakly_canonical(programPath, ec);
        if (!ec)
            programPath = resolved;
        std::error_code existsEc;
        if (!std::filesystem::exists(programPath, existsEc))
        {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "Unable to locate %s", programPath.c_str());
            messageBox(buffer, mfError | mfOKButton);
            return;
        }

        suspend();
        int result = executeProgram(programPath);
        resume();

        bool report = false;
        char buffer[256];

#ifndef _WIN32
        if (result == -1)
        {
            std::snprintf(buffer, sizeof(buffer), "Failed to launch %s", programPath.c_str());
            report = true;
        }
        else if (WIFSIGNALED(result))
        {
            int signum = WTERMSIG(result);
            const char *signalName = strsignal(signum);
            if (!signalName)
                signalName = "unknown signal";
            std::snprintf(buffer, sizeof(buffer), "%s terminated by signal %d (%s)", programPath.c_str(), signum, signalName);
            report = true;
        }
        else if (WIFEXITED(result) && WEXITSTATUS(result) != 0)
        {
            std::snprintf(buffer, sizeof(buffer), "%s exited with status %d", programPath.c_str(), WEXITSTATUS(result));
            report = true;
        }
#else
        if (result != 0)
        {
            std::snprintf(buffer, sizeof(buffer), "%s exited with status %d", programPath.c_str(), result);
            report = true;
        }
#endif

        if (report)
            messageBox(buffer, mfInformation | mfOKButton);
    }

    static int executeProgram(const std::filesystem::path &programPath)
    {
#ifndef _WIN32
        pid_t pid = fork();
        if (pid == 0)
        {
            execl(programPath.c_str(), programPath.c_str(), static_cast<char *>(nullptr));
            _exit(127);
        }
        if (pid < 0)
            return -1;

        int status = 0;
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
            ;
        return status;
#else
        std::string command = "\"" + programPath.string() + "\"";
        return std::system(command.c_str());
#endif
    }

    static std::filesystem::path resolveToolDirectory(const char *argv0)
    {
        std::filesystem::path base = std::filesystem::current_path();
        if (!argv0 || !argv0[0])
            return base;

        std::filesystem::path candidate = argv0;
        std::error_code ec;
        if (!candidate.is_absolute())
            candidate = std::filesystem::current_path() / candidate;

        std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, ec);
        if (!ec)
            candidate = canonical;

        if (!candidate.has_parent_path())
            return base;
        return candidate.parent_path();
    }
};

} // namespace

int main(int argc, char **argv)
{
    LauncherApp app(argc, argv);
    app.run();
    app.shutDown();
    return 0;
}
