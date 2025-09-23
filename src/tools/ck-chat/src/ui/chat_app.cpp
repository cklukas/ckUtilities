#include "chat_app.hpp"
#include "chat_window.hpp"
#include "../commands.hpp"
#include "ck/app_info.hpp"
#include "ck/launcher.hpp"
#include "ck/about_dialog.hpp"
#include "ck/ai/config.hpp"
#include "ck/ai/llm.hpp"




#include <cstdlib>
#include <algorithm>

namespace
{
    const ck::appinfo::ToolInfo &tool_info()
    {
        return ck::appinfo::requireTool("ck-chat");
    }

    ck::ai::RuntimeConfig runtime_from_config(const ck::ai::Config &config)
    {
        ck::ai::RuntimeConfig runtime = config.runtime;
        if (runtime.model_path.empty())
            runtime.model_path = "stub-model.gguf";
        return runtime;
    }
}

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

    TSubMenu &fileMenu = *new TSubMenu("~F~ile", hcNoContext) + *new TMenuItem("~N~ew Chat...", cmNewChat, kbCtrlN, hcNoContext, "Ctrl-N") + *new TMenuItem("~C~lose Window", cmClose, kbAltF3, hcNoContext, "Alt-F3") + newLine();
    if (ck::launcher::launchedFromCkLauncher())
        fileMenu + *new TMenuItem("Return to ~L~auncher", cmReturnToLauncher, kbCtrlL, hcNoContext, "Ctrl-L");
    fileMenu + *new TMenuItem("E~x~it", cmQuit, kbAltX, hcNoContext, "Alt-X");

    TSubMenu &modelsMenu = *new TSubMenu("~M~odels", hcNoContext) + *new TMenuItem("Model ~1~", cmSelectModel1, kbNoKey, hcNoContext) + *new TMenuItem("Model ~2~", cmSelectModel2, kbNoKey, hcNoContext) + *new TMenuItem("Model ~3~", cmSelectModel3, kbNoKey, hcNoContext) + newLine() + *new TMenuItem("~M~anage models...", cmManageModels, kbNoKey, hcNoContext);

    TMenuItem &menuChain = fileMenu + modelsMenu + *new TSubMenu("~W~indows", hcNoContext) + *new TMenuItem("~R~esize/Move", cmResize, kbCtrlF5, hcNoContext, "Ctrl-F5") + *new TMenuItem("~Z~oom", cmZoom, kbF5, hcNoContext, "F5") + *new TMenuItem("~N~ext", cmNext, kbF6, hcNoContext, "F6") + *new TMenuItem("~C~lose", cmClose, kbAltF3, hcNoContext, "Alt-F3") + *new TMenuItem("~T~ile", cmTile, kbNoKey) + *new TMenuItem("C~a~scade", cmCascade, kbNoKey) + *new TSubMenu("~H~elp", hcNoContext) + *new TMenuItem("~A~bout", cmAbout, kbF1, hcNoContext, "F1");

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
