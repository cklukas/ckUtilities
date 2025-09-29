#pragma once

#include "../chat_session.hpp"
#include "../tvision_include.hpp"

#include <string>
#include <vector>

class ChatApp;
class ChatTranscriptView;
class PromptInputView;
class TButton;
class TScrollBar;

class ChatWindow : public TWindow
{
public:
    ChatWindow(ChatApp &owner, const TRect &bounds, int number);

    virtual void handleEvent(TEvent &event) override;
    virtual void sizeLimits(TPoint &min, TPoint &max) override;
    virtual void shutDown() override;
    virtual TPalette& getPalette() const override;

    void processPendingResponses();
    void applySystemPrompt(const std::string &prompt);
    void applyConversationSettings(const ck::chat::ChatSession::ConversationSettings &settings);
    void refreshWindowTitle();

private:
    ChatApp &app;
    ck::chat::ChatSession session;
    ChatTranscriptView *transcript = nullptr;
    PromptInputView *promptInput = nullptr;
    TScrollBar *promptScrollBar = nullptr;
    TButton *submitButton = nullptr;
    TScrollBar *transcriptScrollBar = nullptr;
    struct CopyButtonInfo
    {
        std::size_t messageIndex;
        TButton *button = nullptr;
        ushort command = 0;
    };
    std::vector<CopyButtonInfo> copyButtons;

    void newConversation();
    void sendPrompt();
    void updateTranscriptFromSession(bool forceScroll);
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

    ck::chat::ChatSession::ConversationSettings conversationSettings_{};
    std::string lastWindowTitle_;
};
