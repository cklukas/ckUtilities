#include "ck/ai/config.hpp"
#include "ck/ai/llm.hpp"
#include "ck/about_dialog.hpp"
#include "ck/app_info.hpp"
#include "ck/launcher.hpp"

#define Uses_TApplication
#define Uses_TDeskTop
#define Uses_TDialog
#define Uses_TDrawBuffer
#define Uses_TEvent
#define Uses_TButton
#define Uses_TMemo
#define Uses_TKeys
#define Uses_TLabel
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
#define Uses_TMessageBox
#define Uses_TScrollBar
#define Uses_TScroller
#define Uses_TStatusDef
#define Uses_TStatusItem
#define Uses_TStatusLine
#define Uses_TSubMenu
#define Uses_TWindow
#define Uses_TColorAttr
#define Uses_TPalette
#include <tvision/tv.h>

#include <algorithm>
#include <atomic>
#include <utility>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdio>
#include <unistd.h>

namespace
{

const ck::appinfo::ToolInfo &tool_info()
{
    return ck::appinfo::requireTool("ck-chat");
}

static constexpr ushort cmNewChat = 1000;
static constexpr ushort cmReturnToLauncher = 1001;
static constexpr ushort cmAbout = 1002;
static constexpr ushort cmSendPrompt = 1003;
static constexpr ushort cmCopyResponseBase = 2000;
static constexpr ushort cmSelectModel1 = 1100;
static constexpr ushort cmSelectModel2 = 1101;
static constexpr ushort cmSelectModel3 = 1102;
static constexpr ushort cmManageModels = 1110;

std::string read_prompt_from_stdin()
{
    std::cout << "Enter prompt: " << std::flush;
    std::string prompt;
    std::getline(std::cin, prompt);
    return prompt;
}

bool is_help_flag(std::string_view arg)
{
    return arg == "--help" || arg == "-h";
}

std::optional<std::string> parse_prompt_arg(int argc, char **argv, bool &showHelp)
{
    std::optional<std::string> prompt;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (is_help_flag(arg))
        {
            showHelp = true;
            continue;
        }
        if (arg == "--prompt" && i + 1 < argc)
        {
            prompt = std::string(argv[++i]);
            continue;
        }
        if (arg.rfind("--prompt=", 0) == 0)
        {
            prompt = std::string(arg.substr(9));
            continue;
        }
    }
    return prompt;
}

ck::ai::RuntimeConfig runtime_from_config(const ck::ai::Config &config)
{
    ck::ai::RuntimeConfig runtime = config.runtime;
    if (runtime.model_path.empty())
        runtime.model_path = "stub-model.gguf";
    return runtime;
}

void print_banner()
{
    const auto &info = tool_info();
    std::cout << "=== " << info.displayName << " ===\n";
    std::cout << info.shortDescription << "\n\n";
}

void stream_response(ck::ai::Llm &llm, const std::string &prompt)
{
    ck::ai::GenerationConfig config;
    std::cout << "\n[ck-chat] streaming response...\n";
    llm.generate(prompt, config, [](ck::ai::Chunk chunk) {
        std::cout << chunk.text << std::flush;
        if (chunk.is_last)
            std::cout << "\n" << std::flush;
    });
}

namespace clipboard
{

std::string base64Encode(const std::string &input)
{
    static const char base64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string encoded;
    int val = 0;
    int valb = -6;
    for (unsigned char c : input)
    {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0)
        {
            encoded.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6)
        encoded.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (encoded.size() % 4)
        encoded.push_back('=');
    return encoded;
}

bool osc52Likely()
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

    return termStr.find("xterm") != std::string::npos ||
           termStr.find("tmux") != std::string::npos ||
           termStr.find("screen") != std::string::npos ||
           termStr.find("rxvt") != std::string::npos ||
           termStr.find("alacritty") != std::string::npos ||
           termStr.find("foot") != std::string::npos ||
           termStr.find("kitty") != std::string::npos ||
           termStr.find("wezterm") != std::string::npos;
}

std::string statusMessage()
{
    if (osc52Likely())
        return "Response copied to clipboard!";
    if (std::getenv("TMUX") && !osc52Likely())
        return "Clipboard not supported - tmux needs OSC 52 configuration";
    return "Clipboard not supported by this terminal";
}

void copyToClipboard(const std::string &text)
{
    if (!osc52Likely())
        return;

    constexpr std::size_t maxOsc52Payload = 100000;
    std::string encoded = base64Encode(text);

    if (encoded.size() > maxOsc52Payload)
        return;

    FILE *out = std::fopen("/dev/tty", "w");
    if (!out && isatty(fileno(stdout)))
        out = stdout;

    if (!out)
        return;

    std::fprintf(out, "\033]52;c;%s\a", encoded.c_str());
    std::fflush(out);
    if (out != stdout)
        std::fclose(out);
}

} // namespace clipboard

class ChatWindow;

class ChatApp : public TApplication
{
public:
    ChatApp(int argc, char **argv);

    virtual void handleEvent(TEvent &event) override;
    virtual void idle() override;

    static TMenuBar *initMenuBar(TRect r);
    static TStatusLine *initStatusLine(TRect r);

    void registerWindow(ChatWindow *window);
    void unregisterWindow(ChatWindow *window);

    const ck::ai::RuntimeConfig &runtime() const noexcept { return runtimeConfig; }

private:
    void openChatWindow();
    void showAboutDialog();

    std::vector<ChatWindow *> windows;
    int nextWindowNumber = 1;
    ck::ai::Config config;
    ck::ai::RuntimeConfig runtimeConfig;
};

class ChatTranscriptView : public TScroller
{
public:
    enum class Role
    {
        User,
        Assistant,
        System,
    };

    ChatTranscriptView(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll)
        : TScroller(bounds, hScroll, vScroll)
    {
        options |= ofFirstClick;
        growMode = gfGrowHiX | gfGrowHiY;
        setLimit(1, 1);
    }

    std::size_t beginMessage(Role role, std::string initialContent)
    {
        messages.push_back(Message{role, std::move(initialContent)});
        layoutDirty = true;
        return messages.size() - 1;
    }

    void appendToMessage(std::size_t index, std::string_view text)
    {
        if (index >= messages.size())
            return;
        messages[index].content.append(text);
        layoutDirty = true;
    }

    void clearMessages()
    {
        messages.clear();
        rows.clear();
        layoutDirty = true;
        setLimit(1, 1);
        scrollTo(0, 0);
        drawView();
        notifyLayoutChanged();
    }

    void scrollToBottom()
    {
        rebuildLayoutIfNeeded();
        int totalRows = static_cast<int>(rows.size());
        if (totalRows <= 0)
            totalRows = 1;
        int desired = std::max(0, totalRows - size.y);
        scrollTo(delta.x, desired);
        notifyLayoutChanged();
    }

    void setLayoutChangedCallback(std::function<void()> cb)
    {
        layoutChangedCallback = std::move(cb);
    }

    bool messageForCopy(std::size_t index, std::string &out) const
    {
        if (index >= messages.size())
            return false;
        const auto &msg = messages[index];
        if (msg.role != Role::Assistant)
            return false;
        out = msg.content;
        return true;
    }

    void setMessagePending(std::size_t index, bool pending)
    {
        if (index >= messages.size())
            return;
        messages[index].pending = pending;
    }

    bool isMessagePending(std::size_t index) const
    {
        if (index >= messages.size())
            return false;
        return messages[index].pending;
    }

    std::optional<int> firstRowForMessage(std::size_t index) const
    {
        for (std::size_t row = 0; row < rows.size(); ++row)
        {
            if (rows[row].messageIndex == index && rows[row].isFirstLine)
                return static_cast<int>(row);
        }
        return std::nullopt;
    }

protected:
    virtual void draw() override
    {
        rebuildLayoutIfNeeded();

        auto colors = getColor(1);
        TColorAttr baseAttr = colors[0];

        TDrawBuffer buffer;
        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', baseAttr, size.x);
            std::size_t rowIndex = static_cast<std::size_t>(delta.y + y);
            if (rowIndex < rows.size())
            {
                const auto &row = rows[rowIndex];
                TColorAttr attr = baseAttr;
                if (row.role == Role::Assistant)
                    setFore(attr, TColorDesired(TColorBIOS(0x01)));
                if (!row.text.empty())
                    buffer.moveStr(0, row.text.c_str(), attr);
            }
            writeLine(0, y, size.x, 1, buffer);
        }
    }

    virtual void changeBounds(const TRect &bounds) override
    {
        TScroller::changeBounds(bounds);
        layoutDirty = true;
        rebuildLayoutIfNeeded();
        notifyLayoutChanged();
    }

    virtual void handleEvent(TEvent &event) override
    {
        TPoint before = delta;
        TScroller::handleEvent(event);
        if (before.x != delta.x || before.y != delta.y)
            notifyLayoutChanged();
    }

private:
    struct Message
    {
        Role role;
        std::string content;
        bool pending = false;
    };

    struct DisplayRow
    {
        Role role;
        std::string text;
        std::size_t messageIndex = 0;
        bool isFirstLine = false;
    };

    std::vector<Message> messages;
    std::vector<DisplayRow> rows;
    bool layoutDirty = true;
    std::function<void()> layoutChangedCallback;

    static std::string prefixForRole(Role role)
    {
        switch (role)
        {
        case Role::User:
            return "You: ";
        case Role::Assistant:
            return "Assistant: ";
        default:
            return std::string{};
        }
    }

    void rebuildLayoutIfNeeded()
    {
        if (!layoutDirty)
            return;
        rebuildLayout();
    }

    void rebuildLayout()
    {
        rows.clear();
        int width = std::max(1, size.x);

        for (std::size_t i = 0; i < messages.size(); ++i)
        {
            const auto &msg = messages[i];
            std::string prefix = prefixForRole(msg.role);
            std::string indent(prefix.size(), ' ');
            bool firstLine = true;

            std::string_view content(msg.content);
            std::size_t start = 0;
            while (true)
            {
                std::size_t end = content.find('\n', start);
                std::string_view segment;
                if (end == std::string::npos)
                    segment = content.substr(start);
                else
                    segment = content.substr(start, end - start);

                std::string currentPrefix = firstLine ? prefix : indent;
                std::string line = currentPrefix + std::string(segment);

                if (line.empty())
                {
                    DisplayRow row;
                    row.role = msg.role;
                    row.text = std::string();
                    row.messageIndex = i;
                    row.isFirstLine = firstLine;
                    rows.push_back(std::move(row));
                }
                else
                {
                    std::string remaining = line;
                    bool firstSegment = true;
                    while (!remaining.empty())
                    {
                        DisplayRow row;
                        row.role = msg.role;
                        row.messageIndex = i;
                        row.isFirstLine = firstLine && firstSegment;

                        std::size_t len = std::min<std::size_t>(static_cast<std::size_t>(width), remaining.size());
                        row.text = remaining.substr(0, len);
                        rows.push_back(row);

                        if (len >= remaining.size())
                            break;
                        remaining = indent + remaining.substr(len);
                        firstSegment = false;
                    }
                }

                if (end == std::string::npos)
                    break;
                start = end + 1;
                firstLine = false;
                if (start >= content.size())
                {
                    DisplayRow row;
                    row.role = msg.role;
                    row.text = indent;
                    row.messageIndex = i;
                    row.isFirstLine = false;
                    rows.push_back(std::move(row));
                    break;
                }
            }

            if (i + 1 < messages.size())
            {
                DisplayRow spacer;
                spacer.role = Role::System;
                spacer.text = std::string();
                spacer.messageIndex = i;
                spacer.isFirstLine = false;
                rows.push_back(std::move(spacer));
            }
        }

        int total = static_cast<int>(rows.size());
        if (total <= 0)
        {
            total = 1;
            setLimit(1, total);
        }
        else
        {
            setLimit(1, total);
        }
        layoutDirty = false;
        notifyLayoutChanged();
    }

    void notifyLayoutChanged()
    {
        if (layoutChangedCallback)
            layoutChangedCallback();
    }
};

class PromptInputView : public TMemo
{
public:
    static constexpr ushort kBufferSize = 8192;

    PromptInputView(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll)
        : TMemo(bounds, hScroll, vScroll, nullptr, kBufferSize)
    {
        options |= ofFirstClick;
    }

    virtual TPalette &getPalette() const override
    {
        return TEditor::getPalette();
    }

    std::string text() const
    {
        auto *self = const_cast<PromptInputView *>(this);
        std::vector<char> raw(self->dataSize());
        self->TMemo::getData(raw.data());
        const auto *memo = reinterpret_cast<const TMemoData *>(raw.data());
        return decodeEditorText(memo->buffer, memo->length);
    }

    void setText(const std::string &value)
    {
        setFromEncoded(encodeEditorText(value));
    }

    void clearText()
    {
        setFromEncoded(std::string{});
    }

private:
    static std::string decodeEditorText(const char *data, ushort length)
    {
        std::string result;
        result.reserve(length);
        for (ushort i = 0; i < length; ++i)
        {
            char ch = data[i];
            if (ch == '\r')
            {
                if (i + 1 < length && data[i + 1] == '\n')
                    ++i;
                result.push_back('\n');
            }
            else
            {
                result.push_back(ch);
            }
        }
        return result;
    }

    static std::string encodeEditorText(const std::string &text)
    {
        std::string encoded;
        encoded.reserve(text.size());
        for (char ch : text)
        {
            if (ch == '\n')
                encoded.push_back('\r');
            else
                encoded.push_back(ch);
        }
        return encoded;
    }

    void setFromEncoded(const std::string &encoded)
    {
        const std::size_t limited = std::min<std::size_t>(encoded.size(), static_cast<std::size_t>(kBufferSize));
        std::vector<char> raw(sizeof(ushort) + std::max<std::size_t>(limited, static_cast<std::size_t>(1)));
        auto *memo = reinterpret_cast<TMemoData *>(raw.data());
        memo->length = static_cast<ushort>(std::min<std::size_t>(limited, static_cast<std::size_t>(std::numeric_limits<ushort>::max())));
        if (memo->length > 0)
            std::memcpy(memo->buffer, encoded.data(), memo->length);
        TMemo::setData(memo);
        trackCursor(true);
    }
};

class SolidColorView : public TView
{
public:
    SolidColorView(const TRect &bounds)
        : TView(bounds)
    {
        options &= ~ofSelectable;
    }

    virtual void draw() override
    {
        TColorAttr attr{TColorBIOS(0x0), TColorBIOS(0x7)};
        TDrawBuffer buffer;
        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', attr, size.x);
            writeLine(0, y, size.x, 1, buffer);
        }
    }
};

class ChatWindow : public TWindow
{
public:
    ChatWindow(ChatApp &owner, const TRect &bounds, int number);

    virtual void handleEvent(TEvent &event) override;
    virtual void sizeLimits(TPoint &min, TPoint &max) override;
    virtual void shutDown() override;

    void processPendingResponses();

private:
    struct ResponseTask
    {
        std::mutex mutex;
        std::deque<std::string> chunks;
        std::thread worker;
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::size_t messageIndex = 0;
    };

    ChatApp &app;
    ChatTranscriptView *transcript = nullptr;
    PromptInputView *promptInput = nullptr;
    TScrollBar *promptScrollBar = nullptr;
    TButton *submitButton = nullptr;
    TScrollBar *transcriptScrollBar = nullptr;
    SolidColorView *copyColumnBackground = nullptr;
    SolidColorView *submitBackground = nullptr;
    std::unique_ptr<ResponseTask> activeResponse;
    std::atomic<bool> pendingResponse{false};
    struct CopyButtonInfo
    {
        std::size_t messageIndex;
        TButton *button = nullptr;
        ushort command = 0;
    };
    std::vector<CopyButtonInfo> copyButtons;

    void newConversation();
    void sendPrompt();
    void startResponse(const std::string &prompt);
    void cancelActiveResponse();
    void runSimulatedResponse(ResponseTask &task, std::string prompt);
    void copyAssistantMessage(std::size_t messageIndex);
    void ensureCopyButton(std::size_t messageIndex);
    void updateCopyButtonState(std::size_t messageIndex);
    void updateCopyButtonPositions();
    void updateCopyButtons();
    void clearCopyButtons();
    CopyButtonInfo *findCopyButton(std::size_t messageIndex);
    CopyButtonInfo *findCopyButtonByCommand(ushort command);
    static void setButtonTitle(TButton &button, const char *title);
    TRect copyColumnBounds() const;
};

ChatApp::ChatApp(int, char **)
    : TProgInit(&ChatApp::initStatusLine, &ChatApp::initMenuBar, &TApplication::initDeskTop),
      TApplication()
{
    config = ck::ai::ConfigLoader::load_or_default();
    runtimeConfig = runtime_from_config(config);

    openChatWindow();
}

void ChatApp::registerWindow(ChatWindow *window)
{
    windows.push_back(window);
}

void ChatApp::unregisterWindow(ChatWindow *window)
{
    auto it = std::remove(windows.begin(), windows.end(), window);
    windows.erase(it, windows.end());
}

void ChatApp::openChatWindow()
{
    if (!deskTop)
        return;

    TRect bounds = deskTop->getExtent();
    bounds.grow(-2, -1);
    if (bounds.b.x <= bounds.a.x + 10 || bounds.b.y <= bounds.a.y + 5)
        bounds = TRect(0, 0, 70, 20);

    auto *window = new ChatWindow(*this, bounds, nextWindowNumber++);
    deskTop->insert(window);
    window->select();
}

void ChatApp::handleEvent(TEvent &event)
{
    TApplication::handleEvent(event);
    if (event.what == evCommand)
    {
        switch (event.message.command)
        {
        case cmNewChat:
            openChatWindow();
            clearEvent(event);
            break;
        case cmReturnToLauncher:
            std::exit(ck::launcher::kReturnToLauncherExitCode);
            break;
        case cmAbout:
            showAboutDialog();
            clearEvent(event);
            break;
        default:
            break;
        }
    }
}

void ChatApp::idle()
{
    TApplication::idle();
    for (auto *window : windows)
    {
        if (window)
            window->processPendingResponses();
    }
}

TMenuBar *ChatApp::initMenuBar(TRect r)
{
    r.b.y = r.a.y + 1;

    TSubMenu &fileMenu = *new TSubMenu("~F~ile", hcNoContext)
                         + *new TMenuItem("~N~ew Chat...", cmNewChat, kbCtrlN, hcNoContext, "Ctrl-N")
                         + *new TMenuItem("~C~lose Window", cmClose, kbAltF3, hcNoContext, "Alt-F3")
                         + newLine();
    if (ck::launcher::launchedFromCkLauncher())
        fileMenu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher, kbCtrlL, hcNoContext, "Ctrl-L");
    fileMenu + *new TMenuItem("E~x~it", cmQuit, kbAltX, hcNoContext, "Alt-X");

    TSubMenu &modelsMenu = *new TSubMenu("~M~odels", hcNoContext)
                           + *new TMenuItem("Model ~1~", cmSelectModel1, kbNoKey, hcNoContext)
                           + *new TMenuItem("Model ~2~", cmSelectModel2, kbNoKey, hcNoContext)
                           + *new TMenuItem("Model ~3~", cmSelectModel3, kbNoKey, hcNoContext)
                           + newLine()
                           + *new TMenuItem("~M~anage models...", cmManageModels, kbNoKey, hcNoContext);

    TMenuItem &menuChain = fileMenu
                           + modelsMenu
                           + *new TSubMenu("~W~indows", hcNoContext)
                                 + *new TMenuItem("~R~esize/Move", cmResize, kbCtrlF5, hcNoContext, "Ctrl-F5")
                                 + *new TMenuItem("~Z~oom", cmZoom, kbF5, hcNoContext, "F5")
                                 + *new TMenuItem("~N~ext", cmNext, kbF6, hcNoContext, "F6")
                                 + *new TMenuItem("~C~lose", cmClose, kbAltF3, hcNoContext, "Alt-F3")
                                 + *new TMenuItem("~T~ile", cmTile, kbNoKey)
                                 + *new TMenuItem("C~a~scade", cmCascade, kbNoKey)
                           + *new TSubMenu("~H~elp", hcNoContext)
                                 + *new TMenuItem("~A~bout", cmAbout, kbF1, hcNoContext, "F1");

    return new TMenuBar(r, static_cast<TSubMenu &>(menuChain));
}

TStatusLine *ChatApp::initStatusLine(TRect r)
{
    r.a.y = r.b.y - 1;

    auto *newItem = new TStatusItem("~Ctrl-N~ New Chat", kbCtrlN, cmNewChat);
    auto *closeItem = new TStatusItem("~Alt-F3~ Close", kbAltF3, cmClose);
    newItem->next = closeItem;
    TStatusItem *tail = closeItem;

    if (ck::launcher::launchedFromCkLauncher())
    {
        auto *returnItem = new TStatusItem("~Ctrl-L~ Return", kbCtrlL, cmReturnToLauncher);
        tail->next = returnItem;
        tail = returnItem;
    }

    auto *quitItem = new TStatusItem("~Alt-X~ Quit", kbAltX, cmQuit);
    tail->next = quitItem;

    return new TStatusLine(r, *new TStatusDef(0, 0xFFFF, newItem));
}

void ChatApp::showAboutDialog()
{
    const auto &info = tool_info();
#ifdef CK_CHAT_VERSION
    ck::ui::showAboutDialog(info.executable, CK_CHAT_VERSION, info.aboutDescription);
#else
    ck::ui::showAboutDialog(info.executable, "dev", info.aboutDescription);
#endif
}

ChatWindow::ChatWindow(ChatApp &owner, const TRect &bounds, int number)
    : TWindowInit(&ChatWindow::initFrame),
      TWindow(bounds, "Chat", number),
      app(owner)
{
    palette = wpGrayWindow;
    options |= ofTileable;

    TRect extent = getExtent();
    extent.grow(-1, -1);

    const int inputLines = 4;
    const int labelHeight = 1;
    const int transcriptScrollWidth = 1;
    const int copyButtonColumnWidth = 6;
    const int inputScrollWidth = 1;
    const int buttonWidth = 14;

    TRect transcriptRect = extent;
    transcriptRect.b.y -= (inputLines + labelHeight);
    transcriptRect.b.x -= (transcriptScrollWidth + copyButtonColumnWidth);

    TRect copyColumnRect(transcriptRect.b.x, transcriptRect.a.y,
                         transcriptRect.b.x + copyButtonColumnWidth, transcriptRect.b.y);
    copyColumnBackground = new SolidColorView(copyColumnRect);
    copyColumnBackground->growMode = gfGrowLoY | gfGrowHiY;
    insert(copyColumnBackground);

    TRect transcriptScrollRect(copyColumnRect.b.x, transcriptRect.a.y,
                               copyColumnRect.b.x + transcriptScrollWidth, transcriptRect.b.y);
    auto *transcriptScroll = new TScrollBar(transcriptScrollRect);
    transcriptScroll->growMode = gfGrowHiY;
    transcriptScroll->setState(sfVisible, True);
    insert(transcriptScroll);
    transcriptScrollBar = transcriptScroll;

    transcript = new ChatTranscriptView(transcriptRect, nullptr, transcriptScroll);
    transcript->growMode = gfGrowHiX | gfGrowHiY;
    transcript->setLayoutChangedCallback([this]() { updateCopyButtons(); });
    insert(transcript);

    int labelTop = transcriptRect.b.y;
    int inputTop = labelTop + labelHeight;
    int promptRight = extent.b.x - (buttonWidth + inputScrollWidth);
    if (promptRight <= extent.a.x + 1)
        promptRight = extent.a.x + 2;
    int scrollLeft = promptRight;
    int buttonLeft = std::max(scrollLeft + inputScrollWidth, extent.b.x - buttonWidth);
    if (buttonLeft >= extent.b.x)
        buttonLeft = extent.b.x - 1;

    TRect promptScrollRect(scrollLeft, inputTop, scrollLeft + inputScrollWidth, extent.b.y);
    promptScrollBar = new TScrollBar(promptScrollRect);
    promptScrollBar->growMode = gfGrowLoY | gfGrowHiY | gfGrowLoX | gfGrowHiX;
    promptScrollBar->setState(sfVisible, True);
    insert(promptScrollBar);

    TRect promptRect(extent.a.x, inputTop, scrollLeft, extent.b.y);
    promptInput = new PromptInputView(promptRect, nullptr, promptScrollBar);
    promptInput->growMode = gfGrowHiX | gfGrowLoY | gfGrowHiY;
    insert(promptInput);

    TRect labelRect(extent.a.x, labelTop, scrollLeft, inputTop);
    auto *label = new TLabel(labelRect, "Prompt:", promptInput);
    label->growMode = gfGrowLoY | gfGrowHiY | gfGrowHiX;
    insert(label);

    const int buttonHeight = 2;
    int buttonTop = inputTop + std::max(0, (inputLines - buttonHeight) / 2);
    int buttonBottom = buttonTop + buttonHeight;
    if (buttonBottom > extent.b.y)
        buttonBottom = extent.b.y;
    if (buttonTop >= buttonBottom)
    {
        buttonTop = std::max(inputTop, extent.b.y - 1);
        buttonBottom = std::min(extent.b.y, buttonTop + 1);
    }

    TRect submitBackgroundRect(buttonLeft, inputTop, extent.b.x, extent.b.y);
    submitBackground = new SolidColorView(submitBackgroundRect);
    submitBackground->growMode = gfGrowLoX | gfGrowHiX | gfGrowLoY | gfGrowHiY;
    insert(submitBackground);

    TRect buttonRect(buttonLeft, buttonTop, extent.b.x, buttonBottom);
    submitButton = new TButton(buttonRect, "~S~ubmit", cmSendPrompt, 0);
    submitButton->growMode = gfGrowLoX | gfGrowHiX | gfGrowLoY | gfGrowHiY;
    insert(submitButton);

    promptInput->select();

    app.registerWindow(this);
    newConversation();
}

void ChatWindow::handleEvent(TEvent &event)
{
    TWindow::handleEvent(event);

    if (event.what == evKeyDown && event.keyDown.keyCode == kbAltS)
    {
        sendPrompt();
        clearEvent(event);
        return;
    }

    if (event.what == evCommand)
    {
        ushort command = event.message.command;
        if (command == cmSendPrompt)
        {
            sendPrompt();
            clearEvent(event);
            return;
        }
        if (command >= cmCopyResponseBase)
        {
            if (auto *info = findCopyButtonByCommand(command))
            {
                copyAssistantMessage(info->messageIndex);
                clearEvent(event);
                return;
            }
        }
    }
}

void ChatWindow::sizeLimits(TPoint &min, TPoint &max)
{
    TWindow::sizeLimits(min, max);
    constexpr short minWidth = 50;
    constexpr short minHeight = 16;
    if (min.x < minWidth)
        min.x = minWidth;
    if (min.y < minHeight)
        min.y = minHeight;
    (void)max;
}

void ChatWindow::shutDown()
{
    cancelActiveResponse();
    clearCopyButtons();
    app.unregisterWindow(this);
    TWindow::shutDown();
}

void ChatWindow::processPendingResponses()
{
    auto *task = activeResponse.get();
    if (!task)
        return;

    if (!pendingResponse.exchange(false, std::memory_order_acq_rel))
        return;

    std::deque<std::string> pending;
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        pending.swap(task->chunks);
        finished = task->finished.load(std::memory_order_acquire) && task->chunks.empty();
    }

    bool updated = false;
    for (auto &chunk : pending)
    {
        transcript->appendToMessage(task->messageIndex, chunk);
        updated = true;
    }

    if (updated)
    {
        transcript->scrollToBottom();
        transcript->drawView();
        ensureCopyButton(task->messageIndex);
        updateCopyButtons();
    }

    if (finished)
    {
        transcript->setMessagePending(task->messageIndex, false);
        updateCopyButtonState(task->messageIndex);
        updateCopyButtons();
        if (task->worker.joinable())
            task->worker.join();
        activeResponse.reset();
    }
}

void ChatWindow::newConversation()
{
    cancelActiveResponse();
    clearCopyButtons();
    if (transcript)
    {
        transcript->clearMessages();
        std::size_t welcomeIndex = transcript->beginMessage(ChatTranscriptView::Role::Assistant,
                                                            "Welcome to ck-chat! Type a prompt below and press Alt+S or click Submit.");
        transcript->setMessagePending(welcomeIndex, false);
        transcript->scrollToBottom();
        transcript->drawView();
        ensureCopyButton(welcomeIndex);
    }
    if (promptInput)
    {
        promptInput->clearText();
        promptInput->select();
    }
    updateCopyButtons();
}

void ChatWindow::sendPrompt()
{
    if (!promptInput || !transcript)
        return;

    std::string prompt = promptInput->text();
    if (prompt.empty())
        return;

    transcript->beginMessage(ChatTranscriptView::Role::User, prompt);
    transcript->scrollToBottom();
    transcript->drawView();
    promptInput->clearText();

    startResponse(prompt);
}

void ChatWindow::copyAssistantMessage(std::size_t messageIndex)
{
    if (!transcript)
        return;

    if (transcript->isMessagePending(messageIndex))
        return;

    std::string content;
    if (!transcript->messageForCopy(messageIndex, content))
        return;

    clipboard::copyToClipboard(content);
    messageBox(clipboard::statusMessage().c_str(), mfOKButton);
}

void ChatWindow::ensureCopyButton(std::size_t messageIndex)
{
    if (!transcript)
        return;

    if (auto *info = findCopyButton(messageIndex))
    {
        updateCopyButtonState(messageIndex);
        return;
    }

    ushort command = static_cast<ushort>(cmCopyResponseBase + copyButtons.size());
    TRect column = copyColumnBounds();
    if (column.b.x <= column.a.x)
        column.b.x = column.a.x + 4;
    TRect initialBounds(column.a.x, column.a.y,
                        column.b.x, column.a.y + 2);
    bool pending = transcript->isMessagePending(messageIndex);
    const char *label = pending ? " ⏳ " : " 📋 ";
    auto *button = new TButton(initialBounds, label, command, 0);
    button->growMode = 0;
    button->setState(sfVisible, False);
    button->setState(sfDisabled, pending ? True : False);
    insert(button);
    copyButtons.push_back(CopyButtonInfo{messageIndex, button, command});
    updateCopyButtons();
}

void ChatWindow::updateCopyButtonState(std::size_t messageIndex)
{
    if (!transcript)
        return;

    auto *info = findCopyButton(messageIndex);
    if (!info || !info->button)
        return;

    bool pending = transcript->isMessagePending(messageIndex);
    const char *label = pending ? " ⏳ " : " 📋 ";
    if (!info->button->title || std::strcmp(info->button->title, label) != 0)
        setButtonTitle(*info->button, label);
    info->button->setState(sfDisabled, pending ? True : False);
}

void ChatWindow::updateCopyButtonPositions()
{
    if (!transcript)
        return;

    TRect column = copyColumnBounds();
    if (column.b.x <= column.a.x)
        column.b.x = column.a.x + 4;

    for (auto &info : copyButtons)
    {
        if (!info.button)
            continue;
        auto row = transcript->firstRowForMessage(info.messageIndex);
        if (!row.has_value())
        {
            info.button->setState(sfVisible, False);
            continue;
        }

        int relativeY = row.value() - transcript->delta.y;
        if (relativeY < 0 || relativeY >= transcript->size.y)
        {
            info.button->setState(sfVisible, False);
            continue;
        }

        int top = column.a.y + relativeY;
        if (top + 2 > column.b.y)
            top = column.b.y - 2;
        if (top < column.a.y)
            top = column.a.y;
        TRect desired(column.a.x,
                      top,
                      column.b.x,
                      top + 2);

        TRect current = info.button->getBounds();
        if (desired != current)
            info.button->changeBounds(desired);
        info.button->setState(sfVisible, True);
    }
}

void ChatWindow::updateCopyButtons()
{
    if (!transcript)
        return;
    for (auto &info : copyButtons)
        updateCopyButtonState(info.messageIndex);
    updateCopyButtonPositions();
}

void ChatWindow::clearCopyButtons()
{
    for (auto &info : copyButtons)
    {
        if (info.button)
            TObject::destroy(info.button);
    }
    copyButtons.clear();
}

ChatWindow::CopyButtonInfo *ChatWindow::findCopyButton(std::size_t messageIndex)
{
    for (auto &info : copyButtons)
    {
        if (info.messageIndex == messageIndex)
            return &info;
    }
    return nullptr;
}

ChatWindow::CopyButtonInfo *ChatWindow::findCopyButtonByCommand(ushort command)
{
    for (auto &info : copyButtons)
    {
        if (info.command == command)
            return &info;
    }
    return nullptr;
}

void ChatWindow::setButtonTitle(TButton &button, const char *title)
{
    delete[] const_cast<char *>(button.title);
    button.title = newStr(title);
    button.drawView();
}

TRect ChatWindow::copyColumnBounds() const
{
    if (!transcript)
        return TRect(0, 0, 0, 0);

    if (copyColumnBackground)
        return copyColumnBackground->getBounds();

    TRect transcriptBounds = transcript->getBounds();
    if (transcriptScrollBar)
    {
        TRect scrollBounds = transcriptScrollBar->getBounds();
        return TRect(transcriptBounds.b.x, transcriptBounds.a.y, scrollBounds.a.x, transcriptBounds.b.y);
    }
    return TRect(transcriptBounds.b.x, transcriptBounds.a.y, transcriptBounds.b.x, transcriptBounds.b.y);
}

void ChatWindow::startResponse(const std::string &prompt)
{
    cancelActiveResponse();
    if (!transcript)
        return;

    auto task = std::make_unique<ResponseTask>();
    task->messageIndex = transcript->beginMessage(ChatTranscriptView::Role::Assistant, std::string());
    transcript->setMessagePending(task->messageIndex, true);

    ResponseTask *rawTask = task.get();
    pendingResponse.store(false, std::memory_order_release);
    rawTask->worker = std::thread([this, rawTask, prompt]() mutable {
        runSimulatedResponse(*rawTask, prompt);
    });

    activeResponse = std::move(task);
    transcript->scrollToBottom();
    transcript->drawView();
    ensureCopyButton(rawTask->messageIndex);
    updateCopyButtons();
}

void ChatWindow::cancelActiveResponse()
{
    if (!activeResponse)
        return;

    activeResponse->cancel.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(activeResponse->mutex);
        activeResponse->chunks.clear();
    }
    if (activeResponse->worker.joinable())
        activeResponse->worker.join();
    if (transcript)
    {
        transcript->setMessagePending(activeResponse->messageIndex, false);
        updateCopyButtonState(activeResponse->messageIndex);
    }
    activeResponse.reset();
    pendingResponse.store(false, std::memory_order_release);
    updateCopyButtons();
}

void ChatWindow::runSimulatedResponse(ResponseTask &task, std::string prompt)
{
    using namespace std::chrono_literals;

    for (int repeat = 0; repeat < 5; ++repeat)
    {
        for (char ch : prompt)
        {
            if (task.cancel.load(std::memory_order_acquire))
                break;

            {
                std::lock_guard<std::mutex> lock(task.mutex);
                task.chunks.emplace_back(1, ch);
            }
            pendingResponse.store(true, std::memory_order_release);
            std::this_thread::sleep_for(80ms);
        }

        if (task.cancel.load(std::memory_order_acquire))
            break;

        {
            std::lock_guard<std::mutex> lock(task.mutex);
            task.chunks.emplace_back("\n");
        }
        pendingResponse.store(true, std::memory_order_release);
        std::this_thread::sleep_for(160ms);
    }

    task.finished.store(true, std::memory_order_release);
    pendingResponse.store(true, std::memory_order_release);
}

struct CliOptions
{
    bool showHelp = false;
    std::optional<std::string> prompt;
};

CliOptions parse_cli(int argc, char **argv)
{
    CliOptions options;
    options.prompt = parse_prompt_arg(argc, argv, options.showHelp);
    return options;
}

int run_cli(const CliOptions &options)
{
    print_banner();

    if (options.showHelp && !options.prompt)
    {
        std::cout << "Usage: " << tool_info().executable << " --prompt <TEXT>\n";
        std::cout << "Launch the Turbo Vision interface without --prompt." << std::endl;
        return 0;
    }

    std::string prompt;
    if (options.prompt)
        prompt = *options.prompt;
    else
        prompt = read_prompt_from_stdin();

    if (prompt.empty())
    {
        std::cout << "No prompt provided.\n";
        return 0;
    }

    auto cfg = ck::ai::ConfigLoader::load_or_default();
    auto runtime = runtime_from_config(cfg);
    auto llm = ck::ai::Llm::open(runtime.model_path, runtime);
    llm->set_system_prompt("You are the ck-ai scaffolding.");

    stream_response(*llm, prompt);
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    CliOptions options = parse_cli(argc, argv);
    if (options.prompt || options.showHelp)
        return run_cli(options);

    ChatApp app(argc, argv);
    app.run();
    app.shutDown();
    return 0;
}
