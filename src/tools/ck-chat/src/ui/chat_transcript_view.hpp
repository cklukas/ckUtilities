#pragma once

#include "../chat_session.hpp"

#include "../tvision_include.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

class ChatTranscriptView : public TScroller
{
public:
    using Role = ck::chat::ChatSession::Role;

    ChatTranscriptView(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll);

    void setMessages(const std::vector<ck::chat::ChatSession::Message> &sessionMessages);
    void clearMessages();
    void scrollToBottom();
    void setLayoutChangedCallback(std::function<void()> cb);
    bool messageForCopy(std::size_t index, std::string &out) const;
    void setMessagePending(std::size_t index, bool pending);
    bool isMessagePending(std::size_t index) const;
    std::optional<int> firstRowForMessage(std::size_t index) const;

protected:
    virtual void draw() override;
    virtual void changeBounds(const TRect &bounds) override;
    virtual void handleEvent(TEvent &event) override;

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

    static std::string prefixForRole(Role role);
    void rebuildLayoutIfNeeded();
    void rebuildLayout();
    void notifyLayoutChanged();
};
