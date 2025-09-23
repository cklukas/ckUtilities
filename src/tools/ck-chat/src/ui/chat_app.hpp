#pragma once

#include "../../../../include/ck/ai/config.hpp"

#include "../tvision_include.hpp"

#include <vector>

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
