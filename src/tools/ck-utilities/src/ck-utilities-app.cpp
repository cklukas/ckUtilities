#include "ck/app_info.hpp"
#include "ck/launcher.hpp"

#define Uses_TApplication
#define Uses_TButton
#define Uses_TDeskTop
#define Uses_TDialog
#define Uses_TDrawBuffer
#define Uses_TEvent
#define Uses_TFrame
#define Uses_TKeys
#define Uses_TListViewer
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TMessageBox
#define Uses_MsgBox
#define Uses_TPoint
#define Uses_TScrollBar
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#define Uses_TTerminal
#define Uses_TWindow
#define Uses_TPalette
#include <tvision/tv.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

namespace
{

constexpr std::string_view kLauncherId = "ck-utilities";
constexpr short kUtilityReserveLines = 20;
constexpr short kUtilityWindowSpacing = 1;

const ck::appinfo::ToolInfo &launcherInfo()
{
    return ck::appinfo::requireTool(kLauncherId);
}

constexpr ushort cmLaunchTool = 6000;
constexpr ushort cmNewLauncher = 6001;
constexpr ushort cmShowCalendar = 6002;
constexpr ushort cmShowAsciiTable = 6003;
constexpr ushort cmShowCalculator = 6004;
constexpr ushort cmToggleEventViewer = 6005;
constexpr ushort cmCalcButtonCommand = 6100;
constexpr ushort cmAsciiSelectionChanged = 6101;
constexpr ushort cmFindEventViewer = 6102;

std::string quoteArgument(std::string_view value)
{
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (char ch : value)
    {
        if (ch == '\'')
            quoted.append("'\\''");
        else
            quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

void showLaunchBanner(const std::filesystem::path &programPath,
                      const std::vector<std::string> &arguments)
{
    if (!::isatty(STDOUT_FILENO))
        return;

    std::string commandText = quoteArgument(programPath.string());
    for (const auto &arg : arguments)
    {
        commandText.push_back(' ');
        commandText.append(quoteArgument(arg));
    }

    std::fprintf(stdout,
                 "\n[ck-utilities] Launching %s\n"
                 "[ck-utilities] Return to the launcher once the tool exits.\n\n",
                 commandText.c_str());
    std::fflush(stdout);
}

std::filesystem::path resolveToolDirectory(const char *argv0)
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

    std::optional<std::filesystem::path> locateProgramPath(const std::filesystem::path &toolDirectory,
                                                           const ck::appinfo::ToolInfo &info)
    {
        std::filesystem::path programPath = toolDirectory / std::filesystem::path(info.executable);
        std::error_code ec;
        std::filesystem::path resolved = std::filesystem::weakly_canonical(programPath, ec);
        if (!ec)
            programPath = std::move(resolved);

        std::error_code existsEc;
        if (!std::filesystem::exists(programPath, existsEc))
            return std::nullopt;
        return programPath;
    }

int executeProgram(const std::filesystem::path &programPath,
                   const std::vector<std::string> &arguments,
                   const std::vector<std::pair<std::string, std::string>> &extraEnv = {})
{
    pid_t pid = fork();
    if (pid == -1)
    {
        return -1;
    }
    else if (pid == 0)
    {
        for (const auto &entry : extraEnv)
            setenv(entry.first.c_str(), entry.second.c_str(), 1);

        std::vector<std::string> argvStorage;
        argvStorage.reserve(1 + arguments.size());
        argvStorage.push_back(programPath.string());
        for (const auto &arg : arguments)
            argvStorage.push_back(arg);

            std::vector<char *> argvPointers;
            argvPointers.reserve(argvStorage.size() + 1);
            for (auto &entry : argvStorage)
                argvPointers.push_back(entry.data());
            argvPointers.push_back(nullptr);

        execve(programPath.c_str(), argvPointers.data(), environ);
        _exit(127);
    }

    int status = 0;
    pid_t waitResult = 0;
    do
    {
        waitResult = waitpid(pid, &status, 0);
    } while (waitResult == -1 && errno == EINTR);

    if (waitResult == -1)
        return -1;
    return status;
}

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

    bool decodeUtf8Char(std::string_view text, std::size_t &pos, uint32_t &codePoint)
    {
        if (pos >= text.size())
            return false;

        unsigned char lead = static_cast<unsigned char>(text[pos]);
        if ((lead & 0x80) == 0)
        {
            codePoint = lead;
            ++pos;
            return true;
        }

        auto needsContinuation = [&](unsigned char byte)
        { return (byte & 0xC0) == 0x80; };

        if ((lead & 0xE0) == 0xC0)
        {
            if (pos + 1 >= text.size())
                return false;
            unsigned char b1 = static_cast<unsigned char>(text[pos + 1]);
            if (!needsContinuation(b1))
                return false;
            codePoint = static_cast<uint32_t>((lead & 0x1F) << 6) | static_cast<uint32_t>(b1 & 0x3F);
            pos += 2;
            return true;
        }

        if ((lead & 0xF0) == 0xE0)
        {
            if (pos + 2 >= text.size())
                return false;
            unsigned char b1 = static_cast<unsigned char>(text[pos + 1]);
            unsigned char b2 = static_cast<unsigned char>(text[pos + 2]);
            if (!needsContinuation(b1) || !needsContinuation(b2))
                return false;
            codePoint = static_cast<uint32_t>((lead & 0x0F) << 12) |
                        static_cast<uint32_t>((b1 & 0x3F) << 6) |
                        static_cast<uint32_t>(b2 & 0x3F);
            pos += 3;
            return true;
        }

        if ((lead & 0xF8) == 0xF0)
        {
            if (pos + 3 >= text.size())
                return false;
            unsigned char b1 = static_cast<unsigned char>(text[pos + 1]);
            unsigned char b2 = static_cast<unsigned char>(text[pos + 2]);
            unsigned char b3 = static_cast<unsigned char>(text[pos + 3]);
            if (!needsContinuation(b1) || !needsContinuation(b2) || !needsContinuation(b3))
                return false;
            codePoint = static_cast<uint32_t>((lead & 0x07) << 18) |
                        static_cast<uint32_t>((b1 & 0x3F) << 12) |
                        static_cast<uint32_t>((b2 & 0x3F) << 6) |
                        static_cast<uint32_t>(b3 & 0x3F);
            pos += 4;
            return true;
        }

        // Invalid leading byte; treat it as a literal single-byte char.
        ++pos;
        codePoint = lead;
        return true;
    }

    std::vector<std::string> splitBannerLines()
    {
        std::vector<std::string> lines;
        std::string current;
        for (char ch : ck::appinfo::kProjectBanner)
        {
            if (ch == '\r')
                continue;
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

    int utf8ColumnCount(std::string_view text)
    {
        int count = 0;
        std::size_t pos = 0;
        while (pos < text.size())
        {
            uint32_t codePoint = 0;
            if (!decodeUtf8Char(text, pos, codePoint))
                break;
            ++count;
        }
        return count;
    }

    struct Utf8Slice
    {
        std::size_t offset;
        std::size_t length;
    };

    Utf8Slice utf8ColumnSlice(std::string_view text, int startColumn, int columns)
    {
        std::size_t pos = 0;
        for (int skipped = 0; skipped < startColumn && pos < text.size(); ++skipped)
        {
            uint32_t codePoint = 0;
            if (!decodeUtf8Char(text, pos, codePoint))
            {
                pos = text.size();
                break;
            }
        }

        std::size_t sliceStart = pos;
        int taken = 0;
        while (taken < columns && pos < text.size())
        {
            uint32_t codePoint = 0;
            if (!decodeUtf8Char(text, pos, codePoint))
            {
                pos = text.size();
                break;
            }
            ++taken;
        }

        return {sliceStart, pos - sliceStart};
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
            TColorAttr background{TColorBIOS(0x0), TColorBIOS(0x7)}; // Spaces: black on light gray.
            TColorAttr blueText{TColorBIOS(0x1), TColorBIOS(0x7)};   // Banner glyphs: blue on light gray.
            for (int y = 0; y < size.y; ++y)
            {
                buffer.moveChar(0, ' ', background, size.x);
                int lineIndex = y - 1; // First row stays empty so banner sits one line down.
                if (lineIndex >= 0 && lineIndex < static_cast<int>(bannerLines.size()))
                {
                    const std::string &line = bannerLines[static_cast<std::size_t>(lineIndex)];
                    int width = utf8ColumnCount(line);
                    if (width > 0)
                    {
                        int start = 0;
                        int copyOffset = 0;
                        int copyWidth = width;
                        if (width > size.x)
                        {
                            copyOffset = (width - size.x) / 2;
                            copyWidth = std::min(width - copyOffset, size.x);
                        }
                        else
                        {
                            start = (size.x - width) / 2;
                        }
                        if (copyWidth > 0 && start < size.x)
                        {
                            Utf8Slice slice = utf8ColumnSlice(line, copyOffset, copyWidth);
                            TStringView fragment{line.data() + slice.offset, slice.length};
                            buffer.moveStr(start, fragment, blueText);
                        }
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

    class LauncherApp;

    class LauncherDialog : public TDialog
    {
    public:
        LauncherDialog(LauncherApp &owner, const TRect &bounds, std::vector<const ck::appinfo::ToolInfo *> tools)
            : TWindowInit(&TDialog::initFrame),
              TDialog(bounds, launcherInfo().displayName.data()),
              launcher(&owner),
              bannerLines(splitBannerLines()),
              toolRefs(std::move(tools))
        {
            flags |= wfGrow;
            growMode = gfGrowHiX | gfGrowHiY;

            bannerView = new BannerView(TRect(0, 0, 1, 1), bannerLines);
            bannerView->growMode = gfGrowHiX;
            insert(bannerView);

        vScroll = new TScrollBar(TRect(0, 0, 1, 2));
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
            {
                listView->focusItem(0);
                listView->select();
            }

            ensureDetailUpdated();
        }

        virtual void shutDown() override;

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
                // Only forward the command to the application if it did not
                // originate from this dialog (to avoid loops).
                if (event.message.infoPtr != this)
                {
                    if (TProgram::application)
                    {
                        TEvent launchEvent{};
                        launchEvent.what = evCommand;
                        launchEvent.message.command = cmLaunchTool;
                        launchEvent.message.infoPtr = this;
                        TProgram::application->putEvent(launchEvent);
                    }
                }
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
        LauncherApp *launcher = nullptr;
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

    LauncherDialog *findLauncherDialogFromView(TView *view)
    {
        while (view)
        {
            if (auto *dialog = dynamic_cast<LauncherDialog *>(view))
                return dialog;
            view = view->owner;
        }
        return nullptr;
    }

    constexpr std::array<const char *, 13> kMonthNames = {
        "",
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December",
    };

    constexpr std::array<unsigned, 13> kMonthLengths = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };

    bool isLeapYear(int year)
    {
        if (year % 400 == 0)
            return true;
        if (year % 100 == 0)
            return false;
        return year % 4 == 0;
    }

    unsigned daysInMonth(int year, unsigned month)
    {
        if (month == 0 || month >= kMonthLengths.size())
            return 30;
        unsigned days = kMonthLengths[month];
        if (month == 2 && isLeapYear(year))
            ++days;
        return days;
    }

    int calendarDayOfWeek(int day, unsigned month, int year)
    {
        int m = static_cast<int>(month);
        int y = year;
        if (m < 3)
        {
            m += 12;
            --y;
        }
        int K = y % 100;
        int J = y / 100;
        int h = (day + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
        // Zeller's congruence returns 0 = Saturday. Adjust so 0 = Sunday.
        int dayOfWeek = ((h + 6) % 7);
        return dayOfWeek;
    }

    class CalendarView : public TView
    {
    public:
        CalendarView(const TRect &bounds)
            : TView(bounds)
        {
            options |= ofSelectable;
            eventMask |= evMouseAuto | evMouseDown | evKeyboard;

            auto now = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
            std::chrono::year_month_day ymd(now);
            year = static_cast<int>(ymd.year());
            month = static_cast<unsigned>(ymd.month());
            currentYear = year;
            currentMonth = month;
            currentDay = static_cast<unsigned>(ymd.day());
        }

        virtual void draw() override
        {
            TDrawBuffer buf;
            auto normal = getColor(6);
            auto highlight = getColor(7);

            buf.moveChar(0, ' ', normal, size.x);
            std::ostringstream header;
            header << std::setw(9) << kMonthNames[std::min<std::size_t>(month, kMonthNames.size() - 1)]
                   << ' ' << std::setw(4) << year
                   << ' ' << static_cast<char>(30) << "  " << static_cast<char>(31);
            buf.moveStr(0, header.str().c_str(), normal);
            writeLine(0, 0, size.x, 1, buf);

            buf.moveChar(0, ' ', normal, size.x);
            buf.moveStr(0, "Su Mo Tu We Th Fr Sa", normal);
            writeLine(0, 1, size.x, 1, buf);

            int firstWeekday = calendarDayOfWeek(1, month, year);
            int current = 1 - firstWeekday;
            int totalDays = static_cast<int>(daysInMonth(year, month));
            for (int row = 0; row < 6; ++row)
            {
                buf.moveChar(0, ' ', normal, size.x);
                for (int col = 0; col < 7; ++col)
                {
                    if (current < 1 || current > totalDays)
                    {
                        buf.moveStr(col * 3, "   ", normal);
                    }
                    else
                    {
                        std::ostringstream cell;
                        cell << std::setw(2) << current;
                        bool isToday = (year == currentYear && month == currentMonth && current == static_cast<int>(currentDay));
                        buf.moveStr(col * 3, cell.str().c_str(), isToday ? highlight : normal);
                    }
                    ++current;
                }
                writeLine(0, static_cast<short>(row + 2), size.x, 1, buf);
            }
        }

        virtual void handleEvent(TEvent &event) override
        {
            TView::handleEvent(event);
            if (event.what == evKeyboard)
            {
                bool handled = false;
                switch (event.keyDown.keyCode)
                {
                case kbLeft:
                    changeMonth(-1);
                    handled = true;
                    break;
                case kbRight:
                    changeMonth(1);
                    handled = true;
                    break;
                case kbUp:
                case kbPgUp:
                    changeMonth(-12);
                    handled = true;
                    break;
                case kbDown:
                case kbPgDn:
                    changeMonth(12);
                    handled = true;
                    break;
                case kbHome:
                    year = currentYear;
                    month = currentMonth;
                    handled = true;
                    break;
                default:
                    break;
                }
                if (handled)
                {
                    drawView();
                    clearEvent(event);
                }
            }
            else if (event.what == evMouseDown || event.what == evMouseAuto)
            {
                TPoint point = makeLocal(event.mouse.where);
                if (point.y == 0)
                {
                    if (point.x == 15)
                        changeMonth(1);
                    else if (point.x == 18)
                        changeMonth(-1);
                    drawView();
                }
                clearEvent(event);
            }
        }

    private:
        int year = 0;
        unsigned month = 1;
        unsigned currentDay = 1;
        int currentYear = 0;
        unsigned currentMonth = 1;

        void changeMonth(int delta)
        {
            int totalMonths = static_cast<int>(month) + delta;
            int newYear = year + (totalMonths - 1) / 12;
            int newMonth = (totalMonths - 1) % 12 + 1;
            if (newMonth <= 0)
            {
                newMonth += 12;
                --newYear;
            }
            year = newYear;
            month = static_cast<unsigned>(newMonth);
        }
    };

    class CalendarWindow : public TWindow
    {
    public:
        explicit CalendarWindow(LauncherApp &owner)
            : TWindowInit(&CalendarWindow::initFrame),
              TWindow(TRect(0, 0, 24, 10), "Calendar", wnNoNumber),
              launcher(&owner)
        {
            flags &= ~(wfGrow | wfZoom);
            growMode = 0;
            palette = wpGrayWindow;

            TRect inner = getExtent();
            inner.grow(-1, -1);
            insert(new CalendarView(inner));
        }

    protected:
        virtual void shutDown() override;

    private:
        LauncherApp *launcher = nullptr;
    };

    class AsciiInfoView : public TView
    {
    public:
        AsciiInfoView(const TRect &bounds)
            : TView(bounds)
        {
        }

        virtual void draw() override
        {
            TDrawBuffer buf;
            auto color = getColor(6);
            buf.moveChar(0, ' ', color, size.x);

            std::ostringstream line;
            char displayChar = static_cast<char>(selectedChar == 0 ? 0x20 : selectedChar);
            line << "  Char: ";
            if (selectedChar >= 32 && selectedChar < 127)
                line << displayChar;
            else if (selectedChar == 0)
                line << ' ';
            else
                line << '?';
            line << "  Decimal: " << std::setw(3) << selectedChar
                 << "  Hex " << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << selectedChar;
            buf.moveStr(0, line.str().c_str(), color);
            writeLine(0, 0, size.x, 1, buf);
        }

        virtual void handleEvent(TEvent &event) override
        {
            if (event.what == evBroadcast && event.message.command == cmAsciiSelectionChanged)
            {
                int value = static_cast<int>(reinterpret_cast<std::uintptr_t>(event.message.infoPtr));
                selectedChar = std::clamp(value, 0, 255);
                drawView();
                clearEvent(event);
                return;
            }
            TView::handleEvent(event);
        }

    private:
        int selectedChar = 0;
    };

    class AsciiTableView : public TView
    {
    public:
        AsciiTableView(const TRect &bounds)
            : TView(bounds)
        {
            options |= ofSelectable;
            eventMask |= evKeyboard | evMouseDown | evMouseAuto | evMouseMove;
            blockCursor();
            setCursor(0, 0);
        }

        virtual void draw() override
        {
            TDrawBuffer buf;
            auto color = getColor(6);
            for (short y = 0; y < size.y; ++y)
            {
                buf.moveChar(0, ' ', color, size.x);
                for (short x = 0; x < size.x; ++x)
                {
                    unsigned value = static_cast<unsigned>(x + y * size.x);
                    buf.moveChar(x, static_cast<char>(value), color, 1);
                }
                writeLine(0, y, size.x, 1, buf);
            }
            showCursor();
        }

        virtual void handleEvent(TEvent &event) override
        {
            TView::handleEvent(event);
            if (event.what == evMouseDown)
            {
                do
                {
                    if (mouseInView(event.mouse.where))
                    {
                        TPoint spot = makeLocal(event.mouse.where);
                        spot.x = std::clamp<short>(spot.x, 0, static_cast<short>(size.x - 1));
                        spot.y = std::clamp<short>(spot.y, 0, static_cast<short>(size.y - 1));
                        setCursor(spot.x, spot.y);
                        notifySelection();
                    }
                } while (mouseEvent(event, evMouseMove));
                clearEvent(event);
            }
            else if (event.what == evKeyboard)
            {
                bool handled = false;
                switch (event.keyDown.keyCode)
                {
                case kbHome:
                    setCursor(0, 0);
                    handled = true;
                    break;
                case kbEnd:
                    setCursor(static_cast<short>(size.x - 1), static_cast<short>(size.y - 1));
                    handled = true;
                    break;
                case kbUp:
                    if (cursor.y > 0)
                    {
                        setCursor(cursor.x, cursor.y - 1);
                        handled = true;
                    }
                    break;
                case kbDown:
                    if (cursor.y < size.y - 1)
                    {
                        setCursor(cursor.x, cursor.y + 1);
                        handled = true;
                    }
                    break;
                case kbLeft:
                    if (cursor.x > 0)
                    {
                        setCursor(cursor.x - 1, cursor.y);
                        handled = true;
                    }
                    break;
                case kbRight:
                    if (cursor.x < size.x - 1)
                    {
                        setCursor(cursor.x + 1, cursor.y);
                        handled = true;
                    }
                    break;
                default:
                {
                    unsigned char ch = static_cast<unsigned char>(event.keyDown.charScan.charCode);
                    setCursor(static_cast<short>(ch % size.x), static_cast<short>(ch / size.x));
                    handled = true;
                    break;
                }
                }
                if (handled)
                {
                    notifySelection();
                    clearEvent(event);
                }
            }
        }

    private:
        void notifySelection()
        {
            unsigned value = static_cast<unsigned>(cursor.x + cursor.y * size.x);
            message(owner, evBroadcast, cmAsciiSelectionChanged,
                    reinterpret_cast<void *>(static_cast<std::uintptr_t>(value)));
        }
    };

    class AsciiTableWindow : public TWindow
    {
    public:
        explicit AsciiTableWindow(LauncherApp &owner)
            : TWindowInit(&AsciiTableWindow::initFrame),
              TWindow(TRect(0, 0, 34, 12), "ASCII Table", wnNoNumber),
              launcher(&owner)
        {
            flags &= ~(wfGrow | wfZoom);
            growMode = 0;
            palette = wpGrayWindow;

            TRect bounds = getExtent();
            bounds.grow(-1, -1);

            TRect infoRect = bounds;
            infoRect.a.y = std::max<short>(bounds.b.y - 1, bounds.a.y);
            auto *info = new AsciiInfoView(infoRect);
            info->options |= ofFramed;
            info->eventMask |= evBroadcast;
            insert(info);

            TRect tableRect = bounds;
            tableRect.b.y = infoRect.a.y - 1;
            if (tableRect.b.y <= tableRect.a.y)
                tableRect.b.y = infoRect.a.y;
            auto *table = new AsciiTableView(tableRect);
            table->options |= ofFramed;
            insert(table);
            table->select();
            message(this, evBroadcast, cmAsciiSelectionChanged, reinterpret_cast<void *>(0));
        }

    protected:
        virtual void shutDown() override;

    private:
        LauncherApp *launcher = nullptr;
    };

    class CalculatorDisplay : public TView
    {
    public:
        CalculatorDisplay(const TRect &bounds)
            : TView(bounds)
        {
            options |= ofSelectable;
            eventMask = evKeyboard | evBroadcast;
            number = "0";
        }

        virtual TPalette &getPalette() const override
        {
            static TPalette palette("\x13", 1);
            return palette;
        }

        virtual void handleEvent(TEvent &event) override
        {
            TView::handleEvent(event);
            if (event.what == evKeyboard)
            {
                calcKey(static_cast<unsigned char>(event.keyDown.charScan.charCode));
                clearEvent(event);
            }
            else if (event.what == evBroadcast && event.message.command == cmCalcButtonCommand)
            {
                if (auto *button = static_cast<TButton *>(event.message.infoPtr))
                {
                    if (button->title && button->title[0])
                        calcKey(static_cast<unsigned char>(button->title[0]));
                }
                clearEvent(event);
            }
        }

        virtual void draw() override
        {
            TDrawBuffer buf;
            auto color = getColor(1);
            buf.moveChar(0, ' ', color, size.x);
            short padding = static_cast<short>(size.x - static_cast<short>(number.size()) - 2);
            if (padding < 0)
                padding = 0;
            buf.moveChar(0, ' ', color, padding);
            buf.moveChar(padding, sign, color, 1);
            buf.moveStr(padding + 1, number.c_str(), color);
            writeLine(0, 0, size.x, 1, buf);
        }

        void clear()
        {
            status = CalculatorState::First;
            number = "0";
            sign = ' ';
            pendingOperator = '=';
            operand = 0.0;
        }

    private:
        enum class CalculatorState
        {
            First,
            Valid,
            Error,
        };

        CalculatorState status = CalculatorState::First;
        std::string number;
        char sign = ' ';
        char pendingOperator = '=';
        double operand = 0.0;

        void calcKey(unsigned char key)
        {
            switch (key)
            {
            case '0': case '1': case '2': case '3': case '4':
            case '5': case '6': case '7': case '8': case '9':
                checkFirst();
                if (number.size() < 15)
                {
                    if (number == "0")
                        number.clear();
                    number.push_back(static_cast<char>(key));
                }
                break;
            case '.':
                checkFirst();
                if (number.find('.') == std::string::npos)
                    number.push_back('.');
                break;
            case 8:
            case 27:
            {
                checkFirst();
                if (number.size() <= 1)
                    number = "0";
                else
                    number.pop_back();
                break;
            }
            case '_':
            case 0xF1:
                sign = (sign == ' ') ? '-' : ' ';
                break;
            case '+': case '-': case '*': case '/':
            case '=': case '%': case 13:
                if (status == CalculatorState::Valid)
                {
                    status = CalculatorState::First;
                    double value = currentValue();
                    if (key == '%')
                    {
                        if (pendingOperator == '+' || pendingOperator == '-')
                            value = (operand * value) / 100.0;
                        else
                            value /= 100.0;
                    }
                    applyOperation(value);
                }
                pendingOperator = static_cast<char>(key == 13 ? '=' : key);
                operand = currentValue();
                break;
            case 'C':
            case 'c':
                clear();
                break;
            default:
                break;
            }
            drawView();
        }

        void applyOperation(double value)
        {
            switch (pendingOperator)
            {
            case '+':
                setDisplay(operand + value);
                break;
            case '-':
                setDisplay(operand - value);
                break;
            case '*':
                setDisplay(operand * value);
                break;
            case '/':
                if (value == 0.0)
                    showError();
                else
                    setDisplay(operand / value);
                break;
            case '=':
                setDisplay(value);
                break;
            default:
                break;
            }
        }

        void checkFirst()
        {
            if (status == CalculatorState::First)
            {
                status = CalculatorState::Valid;
                number = "0";
                sign = ' ';
            }
            else if (status == CalculatorState::Error)
            {
                clear();
                status = CalculatorState::Valid;
            }
        }

        double currentValue() const
        {
            double value = 0.0;
            std::stringstream ss(number);
            ss >> value;
            if (sign == '-')
                value = -value;
            return value;
        }

        void setDisplay(double value)
        {
            if (!std::isfinite(value))
            {
                showError();
                return;
            }
            sign = value < 0 ? '-' : ' ';
            double absValue = std::fabs(value);
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%.12g", absValue);
            number = buffer;
            status = CalculatorState::Valid;
        }

        void showError()
        {
            status = CalculatorState::Error;
            number = "Error";
            sign = ' ';
        }
    };

    class CalculatorDialog : public TDialog
    {
    public:
        explicit CalculatorDialog(LauncherApp &owner)
            : TWindowInit(&CalculatorDialog::initFrame),
              TDialog(TRect(5, 3, 29, 18), "Calculator"),
              launcher(&owner)
        {
            options |= ofFirstClick;

            static const std::array<const char *, 20> kButtonLabels = {
                "C", "\x1B", "%", "\xF1",
                "7", "8", "9", "/",
                "4", "5", "6", "*",
                "1", "2", "3", "-",
                "0", ".", "=", "+",
            };

            for (std::size_t i = 0; i < kButtonLabels.size(); ++i)
            {
                int x = static_cast<int>(i % 4) * 5 + 2;
                int y = static_cast<int>(i / 4) * 2 + 4;
                TRect r(x, y, x + 5, y + 2);
                auto *button = new TButton(r, kButtonLabels[i], cmCalcButtonCommand, bfNormal | bfBroadcast);
                button->options &= ~ofSelectable;
                insert(button);
            }

            insert(new CalculatorDisplay(TRect(3, 2, 21, 3)));
        }

    protected:
        virtual void shutDown() override;

    private:
        LauncherApp *launcher = nullptr;
    };

    class EventViewerWindow : public TWindow
    {
    public:
        using ClosedHandler = std::function<void(EventViewerWindow *)>;

        EventViewerWindow(const TRect &bounds, ushort bufferSize)
            : TWindowInit(&EventViewerWindow::initFrame),
              TWindow(bounds, "Event Viewer", wnNoNumber),
              bufferSize(bufferSize)
        {
            eventMask |= evBroadcast;
            palette = wpGrayWindow;
            title = currentTitle();

            scrollBar = standardScrollBar(sbVertical | sbHandleKeyboard);
            TRect inner = getExtent();
            inner.grow(-1, -1);
            terminal = new TTerminal(inner, 0, scrollBar, bufferSize);
            insert(terminal);
            output = std::make_unique<std::ostream>(terminal);
        }

        void setClosedHandler(ClosedHandler handler)
        {
            onClosed = std::move(handler);
        }

        void toggle()
        {
            stopped = !stopped;
            title = currentTitle();
            if (frame)
                frame->drawView();
        }

        void printEvent(const TEvent &event)
        {
            if (!output || stopped || event.what == evNothing)
                return;

            std::ostringstream os;
            os << "Event #" << ++eventCount << '\n';
            describeEvent(os, event);
            os << '\n';
            (*output) << os.str();
            output->flush();
        }

        virtual void handleEvent(TEvent &event) override
        {
            TWindow::handleEvent(event);
            if (event.what == evBroadcast && event.message.command == cmFindEventViewer)
            {
                event.message.infoPtr = this;
                clearEvent(event);
            }
        }

        virtual void shutDown() override
        {
            output.reset();
            terminal = nullptr;
            scrollBar = nullptr;
            if (onClosed)
                onClosed(this);
            TWindow::shutDown();
        }

    private:
        bool stopped = false;
        ushort bufferSize = 0;
        std::size_t eventCount = 0;
        TTerminal *terminal = nullptr;
        TScrollBar *scrollBar = nullptr;
        std::unique_ptr<std::ostream> output;
        ClosedHandler onClosed;

        const char *currentTitle() const
        {
            return stopped ? "Event Viewer (Stopped)" : "Event Viewer";
        }

        static void describeEvent(std::ostringstream &os, const TEvent &event)
        {
            os << "  what: 0x" << std::hex << std::setw(4) << std::setfill('0') << event.what << std::dec << std::setfill(' ') << '\n';
            if (event.what & evMouse)
            {
                os << "  mouse.where: (" << event.mouse.where.x << ", " << event.mouse.where.y << ")\n";
                os << "  mouse.buttons: 0x" << std::hex << std::setw(4) << std::setfill('0') << event.mouse.buttons << std::dec << std::setfill(' ') << '\n';
                os << "  mouse.eventFlags: 0x" << std::hex << std::setw(4) << std::setfill('0') << event.mouse.eventFlags << std::dec << std::setfill(' ') << '\n';
                os << "  mouse.controlKeyState: 0x" << std::hex << std::setw(4) << std::setfill('0') << event.mouse.controlKeyState << std::dec << std::setfill(' ') << '\n';
                os << "  mouse.wheel: 0x" << std::hex << std::setw(4) << std::setfill('0') << event.mouse.wheel << std::dec << std::setfill(' ') << '\n';
            }
            if (event.what & evKeyboard)
            {
                unsigned char charCode = static_cast<unsigned char>(event.keyDown.charScan.charCode);
                os << "  keyDown.keyCode: 0x" << std::hex << std::setw(4) << std::setfill('0') << event.keyDown.keyCode << std::dec << std::setfill(' ') << '\n';
                os << "  keyDown.charCode: " << static_cast<int>(charCode);
                if (charCode >= 32 && charCode < 127)
                    os << " ('" << static_cast<char>(charCode) << "')";
                os << '\n';
                os << "  keyDown.scanCode: " << static_cast<int>(static_cast<unsigned char>(event.keyDown.charScan.scanCode)) << '\n';
                os << "  keyDown.controlKeyState: 0x" << std::hex << std::setw(4) << std::setfill('0') << event.keyDown.controlKeyState << std::dec << std::setfill(' ') << '\n';
                os << "  keyDown.textLength: " << static_cast<int>(event.keyDown.textLength) << '\n';
                if (event.keyDown.textLength > 0)
                {
                    os << "  keyDown.text: ";
                    for (int i = 0; i < event.keyDown.textLength; ++i)
                    {
                        if (i)
                            os << ", ";
                        os << "0x" << std::hex << std::setw(2) << std::setfill('0')
                           << static_cast<int>(static_cast<unsigned char>(event.keyDown.text[i])) << std::dec << std::setfill(' ');
                    }
                    os << '\n';
                }
            }
            if (event.what & evCommand)
            {
                os << "  message.command: " << event.message.command << '\n';
                os << "  message.infoPtr: " << event.message.infoPtr << '\n';
            }
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

        virtual void getEvent(TEvent &event) override
        {
            TApplication::getEvent(event);
            if (eventViewer)
                eventViewer->printEvent(event);
        }

        virtual void handleEvent(TEvent &event) override
        {
            // Pre-handle our custom commands to avoid them being propagated
            // to the focused views first (which caused a re-post loop).
            if (event.what == evCommand)
            {
                if (event.message.command == cmLaunchTool)
                {
                    LauncherDialog *dialog = nullptr;
                    if (event.message.infoPtr)
                    {
                        auto *sourceView = static_cast<TView *>(event.message.infoPtr);
                        dialog = findLauncherDialogFromView(sourceView);
                    }
                    if (!dialog && deskTop)
                        dialog = findLauncherDialogFromView(deskTop->current);
                    if (dialog)
                    {
                        launchTool(dialog->currentTool());
                    }
                    clearEvent(event);
                    return; // Don't propagate further.
                }
                else if (event.message.command == cmNewLauncher)
                {
                    openLauncher();
                    clearEvent(event);
                    return;
                }
                else if (event.message.command == cmShowCalendar)
                {
                    openCalendarWindow();
                    clearEvent(event);
                    return;
                }
                else if (event.message.command == cmShowAsciiTable)
                {
                    openAsciiTable();
                    clearEvent(event);
                    return;
                }
                else if (event.message.command == cmShowCalculator)
                {
                    openCalculator();
                    clearEvent(event);
                    return;
                }
                else if (event.message.command == cmToggleEventViewer)
                {
                    toggleEventViewer();
                    clearEvent(event);
                    return;
                }
            }
            TApplication::handleEvent(event);
        }

        static TMenuBar *initMenuBar(TRect r)
        {
            r.b.y = r.a.y + 1;
            return new TMenuBar(r,
                                *new TSubMenu("~\360~", kbNoKey) +
                                    *new TMenuItem("Ca~l~endar", cmShowCalendar, kbNoKey, hcNoContext) +
                                    *new TMenuItem("Ascii ~T~able", cmShowAsciiTable, kbNoKey, hcNoContext) +
                                    *new TMenuItem("~C~alculator", cmShowCalculator, kbNoKey, hcNoContext) +
                                    *new TMenuItem("~E~vent Viewer", cmToggleEventViewer, kbAlt0, hcNoContext, "Alt-0") +
                                *new TSubMenu("~F~ile", kbAltF) +
                                    *new TMenuItem("~N~ew Launcher", cmNewLauncher, kbNoKey, hcNoContext) +
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
        EventViewerWindow *eventViewer = nullptr;
        std::vector<LauncherDialog *> launcherDialogs;
        std::vector<TWindow *> utilityWindows;

        void openLauncher()
        {
            std::vector<const ck::appinfo::ToolInfo *> tools;
            for (const auto &info : ck::appinfo::tools())
            {
                if (info.id == kLauncherId)
                    continue;
                tools.push_back(&info);
            }
            std::sort(tools.begin(), tools.end(), [](const auto *a, const auto *b)
                      { return a->displayName < b->displayName; });

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

            auto *dialog = new LauncherDialog(*this, dialogBounds, std::move(tools));
            deskTop->insert(dialog);
            dialog->select();
            onLauncherOpened(dialog);
        }

        void launchTool(const ck::appinfo::ToolInfo *info, const std::vector<std::string> &extraArgs = {})
        {
            if (!info)
                return;

            std::filesystem::path programPath = toolDirectory / std::filesystem::path(info->executable);
            if (auto resolved = locateProgramPath(toolDirectory, *info))
            {
                programPath = std::move(*resolved);
            }
            else
            {
                char buffer[256];
                std::snprintf(buffer, sizeof(buffer), "Unable to locate %s", programPath.c_str());
                messageBox(buffer, mfError | mfOKButton);
                return;
            }

            std::fflush(stdout);
            std::fflush(stderr);
            showLaunchBanner(programPath, extraArgs);

            suspend();

            std::vector<std::pair<std::string, std::string>> extraEnv = {
                {ck::launcher::kLauncherEnvVar, ck::launcher::kLauncherEnvValue},
            };
            int result = executeProgram(programPath, extraArgs, extraEnv);

            resume();
            redraw();

            bool report = false;
            char buffer[256];
            bool programFinished = false;
            bool returnToLauncher = false;

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
                programFinished = true;
            }
            else if (WIFEXITED(result))
            {
                programFinished = true;
                int exitStatus = WEXITSTATUS(result);
                if (exitStatus == ck::launcher::kReturnToLauncherExitCode)
                {
                    returnToLauncher = true;
                }
                else if (exitStatus != 0)
                {
                    std::snprintf(buffer, sizeof(buffer), "%s exited with status %d", programPath.c_str(), exitStatus);
                    report = true;
                }
            }

            if (report)
                messageBox(buffer, mfInformation | mfOKButton);

            if (programFinished && !returnToLauncher)
            {
                TEvent quitEvent{};
                quitEvent.what = evCommand;
                quitEvent.message.command = cmQuit;
                putEvent(quitEvent);
            }
        }

        void openCalendarWindow()
        {
            if (!deskTop)
                return;
            auto *window = new CalendarWindow(*this);
            deskTop->insert(window);
            onUtilityWindowOpened(window);
        }

        void openAsciiTable()
        {
            if (!deskTop)
                return;
            auto *window = new AsciiTableWindow(*this);
            deskTop->insert(window);
            onUtilityWindowOpened(window);
        }

        void openCalculator()
        {
            if (!deskTop)
                return;
            auto *dialog = new CalculatorDialog(*this);
            deskTop->insert(dialog);
            onUtilityWindowOpened(dialog);
        }

        void toggleEventViewer()
        {
            if (!deskTop)
                return;
            if (eventViewer)
            {
                eventViewer->toggle();
                return;
            }

            auto *viewer = new EventViewerWindow(deskTop->getExtent(), 0x0F00);
            viewer->setClosedHandler([this](EventViewerWindow *closed)
                                     {
                                         if (eventViewer == closed)
                                             eventViewer = nullptr;
                                     });
            eventViewer = viewer;
            deskTop->insert(viewer);
        }

    public:
        void onLauncherOpened(LauncherDialog *dialog)
        {
            if (!dialog)
                return;
            if (std::find(launcherDialogs.begin(), launcherDialogs.end(), dialog) == launcherDialogs.end())
                launcherDialogs.push_back(dialog);
            layoutLauncherWindows();
            layoutUtilityWindows();
        }

        void onLauncherClosed(LauncherDialog *dialog)
        {
            auto it = std::remove(launcherDialogs.begin(), launcherDialogs.end(), dialog);
            if (it != launcherDialogs.end())
                launcherDialogs.erase(it, launcherDialogs.end());
            layoutLauncherWindows();
            layoutUtilityWindows();
        }

        void onUtilityWindowOpened(TWindow *window)
        {
            if (!window)
                return;
            if (std::find(utilityWindows.begin(), utilityWindows.end(), window) == utilityWindows.end())
                utilityWindows.push_back(window);
            layoutLauncherWindows();
            layoutUtilityWindows();
        }

        void onUtilityWindowClosed(TWindow *window)
        {
            auto it = std::remove(utilityWindows.begin(), utilityWindows.end(), window);
            if (it != utilityWindows.end())
                utilityWindows.erase(it, utilityWindows.end());
            layoutLauncherWindows();
            layoutUtilityWindows();
        }

    private:
        void layoutLauncherWindows()
        {
            if (!deskTop)
                return;

            TRect desktopExtent = deskTop->getExtent();
            if (!utilityWindows.empty())
            {
                int availableHeight = desktopExtent.b.y - desktopExtent.a.y;
                if (availableHeight > kUtilityReserveLines)
                {
                    desktopExtent.b.y = static_cast<short>(desktopExtent.b.y - kUtilityReserveLines);
                    if (desktopExtent.b.y < desktopExtent.a.y)
                        desktopExtent.b.y = desktopExtent.a.y;
                }
            }

            TRect bounds = desktopExtent;
            if (bounds.b.x - bounds.a.x > 2)
            {
                ++bounds.a.x;
                --bounds.b.x;
            }
            if (bounds.b.y - bounds.a.y > 2)
            {
                ++bounds.a.y;
                --bounds.b.y;
            }

            for (auto *dialog : launcherDialogs)
            {
                if (!dialog)
                    continue;
                dialog->locate(bounds);
            }
        }

        void layoutUtilityWindows()
        {
            if (!deskTop || utilityWindows.empty())
                return;

            TRect desktopExtent = deskTop->getExtent();
            int availableHeight = desktopExtent.b.y - desktopExtent.a.y;
            int utilityTop = desktopExtent.a.y;
            if (availableHeight > kUtilityReserveLines)
                utilityTop = desktopExtent.b.y - kUtilityReserveLines;

            int currentX = desktopExtent.a.x;
            for (auto *window : utilityWindows)
            {
                if (!window)
                    continue;

                int width = std::max<int>(1, window->size.x);
                int height = std::max<int>(1, window->size.y);

                int left = currentX;
                int right = left + width;
                if (right > desktopExtent.b.x)
                {
                    right = desktopExtent.b.x;
                    left = right - width;
                    if (left < desktopExtent.a.x)
                    {
                        left = desktopExtent.a.x;
                        right = std::min(desktopExtent.b.x, left + width);
                    }
                }

                int top = std::max(utilityTop, desktopExtent.b.y - height);
                int bottom = top + height;
                if (bottom > desktopExtent.b.y)
                {
                    bottom = desktopExtent.b.y;
                    top = std::max(utilityTop, bottom - height);
                }

                TRect newBounds(static_cast<short>(left), static_cast<short>(top),
                                static_cast<short>(right), static_cast<short>(bottom));
                window->locate(newBounds);

                currentX = right + kUtilityWindowSpacing;
            }
        }
    };

    void LauncherDialog::shutDown()
    {
        if (launcher)
        {
            launcher->onLauncherClosed(this);
            launcher = nullptr;
        }
        TDialog::shutDown();
    }

    void CalendarWindow::shutDown()
    {
        if (launcher)
        {
            launcher->onUtilityWindowClosed(this);
            launcher = nullptr;
        }
        TWindow::shutDown();
    }

    void AsciiTableWindow::shutDown()
    {
        if (launcher)
        {
            launcher->onUtilityWindowClosed(this);
            launcher = nullptr;
        }
        TWindow::shutDown();
    }

    void CalculatorDialog::shutDown()
    {
        if (launcher)
        {
            launcher->onUtilityWindowClosed(this);
            launcher = nullptr;
        }
        TDialog::shutDown();
    }

} // namespace

int main(int argc, char **argv)
{
    std::filesystem::path toolDir = resolveToolDirectory(argc > 0 ? argv[0] : nullptr);

    std::string launchTarget;
    std::vector<std::string> launchArgs;
    bool directLaunch = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        if (arg == "--launch")
        {
            if (directLaunch)
            {
                std::fprintf(stderr, "--launch specified multiple times.\n");
                return EXIT_FAILURE;
            }
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "--launch requires a tool identifier.\n");
                return EXIT_FAILURE;
            }
            directLaunch = true;
            launchTarget = argv[++i];
            launchArgs.assign(argv + i + 1, argv + argc);
            break;
        }
        else if (arg == "--help" || arg == "-h")
        {
            const char *binaryName = (argc > 0 && argv[0]) ? argv[0] : "ck-utilities";
            std::printf("Usage: %s [--launch TOOL [ARGS...]]\n", binaryName);
            return 0;
        }
    }

    if (directLaunch)
    {
        const ck::appinfo::ToolInfo *info = ck::appinfo::findTool(launchTarget);
        if (!info)
            info = ck::appinfo::findToolByExecutable(launchTarget);
        if (!info)
        {
            std::fprintf(stderr, "Unknown tool '%s'.\n", launchTarget.c_str());
            return EXIT_FAILURE;
        }

        std::filesystem::path expectedPath = toolDir / std::filesystem::path(info->executable);
        auto resolved = locateProgramPath(toolDir, *info);
        if (!resolved)
        {
            std::fprintf(stderr, "Unable to locate %s\n", expectedPath.c_str());
            return EXIT_FAILURE;
        }

        int status = executeProgram(*resolved, launchArgs);
        if (status == -1)
            return EXIT_FAILURE;
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        return status;
    }

    LauncherApp app(argc, argv);
    app.run();
    app.shutDown();
    return 0;
}
