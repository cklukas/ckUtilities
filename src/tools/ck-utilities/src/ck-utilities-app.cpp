#include "ck/app_info.hpp"
#include "ck/launcher.hpp"

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
#include <cstdint>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace
{

constexpr std::string_view kLauncherId = "ck-utilities";

const ck::appinfo::ToolInfo &launcherInfo()
{
    return ck::appinfo::requireTool(kLauncherId);
}

constexpr ushort cmLaunchTool = 6000;
constexpr ushort cmNewLauncher = 6001;

std::string quoteArgument(std::string_view value)
{
#ifdef _WIN32
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('"');
    for (char ch : value)
    {
        if (ch == '"')
            quoted.push_back('\\');
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
#else
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
#endif
}

class ScopedEnvironment
{
public:
    explicit ScopedEnvironment(const std::vector<std::pair<std::string, std::string>> &entries)
    {
        previous.reserve(entries.size());
        for (const auto &entry : entries)
        {
            const char *existing = std::getenv(entry.first.c_str());
            std::optional<std::string> priorValue;
            if (existing)
                priorValue = std::string(existing);
            previous.emplace_back(entry.first, priorValue);
            if (!apply(entry))
            {
                failed = true;
                break;
            }
            ++applied;
        }

        if (failed)
            rollback();
    }

    ~ScopedEnvironment()
    {
        rollback();
    }

    [[nodiscard]] bool ok() const { return !failed; }

private:
    bool apply(const std::pair<std::string, std::string> &entry)
    {
#ifdef _WIN32
        return _putenv_s(entry.first.c_str(), entry.second.c_str()) == 0;
#else
        return setenv(entry.first.c_str(), entry.second.c_str(), 1) == 0;
#endif
    }

    void rollback()
    {
        while (applied > 0)
        {
            --applied;
            auto &entry = previous[applied];
#ifdef _WIN32
            if (entry.second)
                _putenv_s(entry.first.c_str(), entry.second->c_str());
            else
                _putenv_s(entry.first.c_str(), "");
#else
            if (entry.second)
                setenv(entry.first.c_str(), entry.second->c_str(), 1);
            else
                unsetenv(entry.first.c_str());
#endif
        }
    }

    std::vector<std::pair<std::string, std::optional<std::string>>> previous;
    std::size_t applied = 0;
    bool failed = false;
};

}

class TurboVisionSuspendGuard
{
public:
    explicit TurboVisionSuspendGuard(TApplication &application)
        : app(application)
    {
        app.suspend();
        std::fflush(stdout);
        std::fflush(stderr);
    }

    TurboVisionSuspendGuard(const TurboVisionSuspendGuard &) = delete;
    TurboVisionSuspendGuard &operator=(const TurboVisionSuspendGuard &) = delete;

    ~TurboVisionSuspendGuard()
    {
        app.resume();
        app.redraw();
    }

private:
    TApplication &app;
};

void showLaunchBanner(const std::filesystem::path &programPath,
                      const std::vector<std::string> &arguments)
{
#ifndef _WIN32
    if (!::isatty(STDOUT_FILENO))
        return;
#endif

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
    ScopedEnvironment env(extraEnv);
    if (!env.ok()) {
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
    else
    {
        int status;
        if (waitpid(pid, &status, 0) == -1)
            return -1;
        return status;
    }
#else
    std::vector<std::pair<std::string, std::optional<std::string>>> previousValues;
    previousValues.reserve(extraEnv.size());
    for (const auto &entry : extraEnv)
    {
        const char *existing = std::getenv(entry.first.c_str());
        if (existing)
            previousValues.emplace_back(entry.first, std::string(existing));
        else
            previousValues.emplace_back(entry.first, std::optional<std::string>());
        _putenv_s(entry.first.c_str(), entry.second.c_str());
    }

    std::string command = quoteArgument(programPath.string());
    for (const auto &arg : arguments)
    {
        command.push_back(' ');
        command.append(quoteArgument(arg));
    }

    return std::system(command.c_str());
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

    auto needsContinuation = [&](unsigned char byte) { return (byte & 0xC0) == 0x80; };

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
        TColorAttr background{TColorBIOS(0x0), TColorBIOS(0x7)};  // Spaces: black on light gray.
        TColorAttr blueText{TColorBIOS(0x1), TColorBIOS(0x7)};    // Banner glyphs: blue on light gray.
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
        {
            listView->focusItem(0);
            listView->select();
        }

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
            if (TProgram::application)
            {
                TEvent launchEvent{};
                launchEvent.what = evCommand;
                launchEvent.message.command = cmLaunchTool;
                launchEvent.message.infoPtr = this;
                TProgram::application->putEvent(launchEvent);
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
        if (event.what == evCommand)
        {
            switch (event.message.command)
            {
            case cmLaunchTool:
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
                    launchTool(dialog->currentTool());
                clearEvent(event);
                break;
            }
            case cmNewLauncher:
                openLauncher();
                clearEvent(event);
                break;
            default:
                break;
            }
        }
    }

    static TMenuBar *initMenuBar(TRect r)
    {
        r.b.y = r.a.y + 1;
        return new TMenuBar(r,
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

        int result = 0;
        {
            TurboVisionSuspendGuard guard(*this);
            showLaunchBanner(programPath, extraArgs);
            std::vector<std::pair<std::string, std::string>> extraEnv = {
                {ck::launcher::kLauncherEnvVar, ck::launcher::kLauncherEnvValue},
            };
            result = executeProgram(programPath, extraArgs, extraEnv);
        }

        bool report = false;
        bool suppressReport = false;
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
        else if (WIFEXITED(result))
        {
            int exitStatus = WEXITSTATUS(result);
            if (exitStatus == ck::launcher::kReturnToLauncherExitCode)
            {
                suppressReport = true;
            }
            else if (exitStatus != 0)
            {
                std::snprintf(buffer, sizeof(buffer), "%s exited with status %d", programPath.c_str(), exitStatus);
                report = true;
            }
        }
#else
        if (result == -1)
        {
            std::snprintf(buffer, sizeof(buffer), "Failed to launch %s", programPath.c_str());
            report = true;
        }
        else if (result == ck::launcher::kReturnToLauncherExitCode)
        {
            suppressReport = true;
        }
        else if (result != 0)
        {
            std::snprintf(buffer, sizeof(buffer), "%s exited with status %d", programPath.c_str(), result);
            report = true;
        }
#endif

        if (report && !suppressReport)
            messageBox(buffer, mfInformation | mfOKButton);
    }

};

} // namespace

int main(int argc, char **argv)
{
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
        std::filesystem::path toolDir = resolveToolDirectory(argc > 0 ? argv[0] : nullptr);
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
#ifndef _WIN32
        if (status == -1)
            return EXIT_FAILURE;
        if (WIFSIGNALED(status))
            return 128 + WTERMSIG(status);
        if (WIFEXITED(status))
            return WEXITSTATUS(status);
        return status;
#else
        return status;
#endif
    }

    LauncherApp app(argc, argv);
    app.run();
    app.shutDown();
    return 0;
}
