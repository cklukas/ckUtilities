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
#define Uses_TInputLine
#define Uses_TKeys
#define Uses_TLabel
#define Uses_TMenu
#define Uses_TMenuBar
#define Uses_TMenuItem
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
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

const ck::appinfo::ToolInfo &tool_info()
{
    return ck::appinfo::requireTool("ck-chat");
}

static constexpr ushort cmNewChat = 1000;
static constexpr ushort cmReturnToLauncher = 1001;
static constexpr ushort cmAbout = 1002;

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
    }

    void scrollToBottom()
    {
        rebuildLayoutIfNeeded();
        int totalRows = static_cast<int>(rows.size());
        if (totalRows <= 0)
            totalRows = 1;
        int desired = std::max(0, totalRows - size.y);
        scrollTo(delta.x, desired);
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
                switch (row.role)
                {
                case Role::User:
                    setFore(attr, TColorDesired(TColorBIOS(0x0E)));
                    break;
                case Role::Assistant:
                    setFore(attr, TColorDesired(TColorBIOS(0x0B)));
                    break;
                default:
                    break;
                }
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
    }

private:
    struct Message
    {
        Role role;
        std::string content;
    };

    struct DisplayRow
    {
        Role role;
        std::string text;
    };

    std::vector<Message> messages;
    std::vector<DisplayRow> rows;
    bool layoutDirty = true;

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
                    rows.push_back(DisplayRow{msg.role, std::string()});
                }
                else
                {
                    std::string remaining = line;
                    while (!remaining.empty())
                    {
                        std::size_t len = std::min<std::size_t>(static_cast<std::size_t>(width), remaining.size());
                        rows.push_back(DisplayRow{msg.role, remaining.substr(0, len)});
                        if (len >= remaining.size())
                            break;
                        remaining = indent + remaining.substr(len);
                    }
                }

                if (end == std::string::npos)
                    break;
                start = end + 1;
                firstLine = false;
                if (start > content.size())
                    break;
                if (start == content.size())
                {
                    std::string emptyLine = indent;
                    rows.push_back(DisplayRow{msg.role, emptyLine});
                    break;
                }
            }

            if (i + 1 < messages.size())
                rows.push_back(DisplayRow{Role::System, std::string()});
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
    }
};

class ChatWindow : public TWindow
{
public:
    ChatWindow(ChatApp &owner, const TRect &bounds, int number);

    virtual void handleEvent(TEvent &event) override;
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
    TInputLine *promptInput = nullptr;
    std::unique_ptr<ResponseTask> activeResponse;
    std::atomic<bool> pendingResponse{false};

    void newConversation();
    void sendPrompt();
    void startResponse(const std::string &prompt);
    void cancelActiveResponse();
    void runSimulatedResponse(ResponseTask &task, std::string prompt);
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

    TMenuItem &menuChain = fileMenu
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

    TRect transcriptRect = extent;
    transcriptRect.b.y -= 2;
    transcriptRect.b.x -= 1;

    TRect scrollRect(transcriptRect.b.x, transcriptRect.a.y, transcriptRect.b.x + 1, transcriptRect.b.y);
    auto *vScroll = new TScrollBar(scrollRect);
    vScroll->growMode = gfGrowHiY;
    insert(vScroll);

    transcript = new ChatTranscriptView(transcriptRect, nullptr, vScroll);
    insert(transcript);

    TRect inputRect(extent.a.x, extent.b.y - 1, extent.b.x - 1, extent.b.y);
    promptInput = new TInputLine(inputRect, 2048);
    promptInput->growMode = gfGrowHiX;
    insert(promptInput);

    TRect labelRect(extent.a.x, extent.b.y - 2, extent.a.x + 8, extent.b.y - 1);
    auto *label = new TLabel(labelRect, "Prompt:", promptInput);
    insert(label);

    promptInput->select();

    app.registerWindow(this);
    newConversation();
}

void ChatWindow::handleEvent(TEvent &event)
{
    TWindow::handleEvent(event);

    if (event.what == evKeyDown)
    {
        if (event.keyDown.keyCode == kbEnter && promptInput && (promptInput->state & sfFocused))
        {
            sendPrompt();
            clearEvent(event);
            return;
        }
    }
}

void ChatWindow::shutDown()
{
    cancelActiveResponse();
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
    }

    if (finished)
    {
        if (task->worker.joinable())
            task->worker.join();
        activeResponse.reset();
    }
}

void ChatWindow::newConversation()
{
    cancelActiveResponse();
    if (transcript)
    {
        transcript->clearMessages();
        transcript->beginMessage(ChatTranscriptView::Role::Assistant,
                                 "Welcome to ck-chat! Ask a question below and press Enter.");
        transcript->scrollToBottom();
        transcript->drawView();
    }
    if (promptInput)
        promptInput->setData(const_cast<char *>(""));
}

void ChatWindow::sendPrompt()
{
    if (!promptInput || !transcript)
        return;

    char buffer[4096];
    promptInput->getData(buffer);
    std::string prompt(buffer);
    if (prompt.empty())
        return;

    transcript->beginMessage(ChatTranscriptView::Role::User, prompt);
    transcript->scrollToBottom();
    transcript->drawView();
    promptInput->setData(const_cast<char *>(""));

    startResponse(prompt);
}

void ChatWindow::startResponse(const std::string &prompt)
{
    cancelActiveResponse();
    if (!transcript)
        return;

    auto task = std::make_unique<ResponseTask>();
    task->messageIndex = transcript->beginMessage(ChatTranscriptView::Role::Assistant, std::string());

    ResponseTask *rawTask = task.get();
    pendingResponse.store(false, std::memory_order_release);
    rawTask->worker = std::thread([this, rawTask, prompt]() mutable {
        runSimulatedResponse(*rawTask, prompt);
    });

    activeResponse = std::move(task);
    transcript->scrollToBottom();
    transcript->drawView();
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
    activeResponse.reset();
    pendingResponse.store(false, std::memory_order_release);
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

