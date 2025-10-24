#pragma once

#include "../chat_session.hpp"

#include "../tvision_include.hpp"

#include <cstdint>
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
    bool isAtBottom() const noexcept;
    void setLayoutChangedCallback(std::function<void(bool)> cb);
    void setHiddenDetailCallback(std::function<void(std::size_t, const std::string &,
                                                   const std::string &)> cb);
    bool messageForCopy(std::size_t index, std::string &out) const;
    void setMessagePending(std::size_t index, bool pending);
    bool isMessagePending(std::size_t index) const;
    std::optional<int> firstRowForMessage(std::size_t index) const;
    void setShowThinking(bool show);
    void setShowAnalysis(bool show);
    bool showThinking() const noexcept { return showThinking_; }
    bool showAnalysis() const noexcept { return showAnalysis_; }

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
        bool isThinking = false;
        bool isPlaceholder = false;
        bool isPending = false;
        std::string channelLabel;
        std::string hiddenContent;
        std::vector<std::uint16_t> styleMask;
        struct GlyphInfo
        {
            std::size_t start = 0;
            std::size_t end = 0;
            int width = 0;
        };
        std::vector<GlyphInfo> glyphs;
        int displayWidth = 0;
    };

    std::vector<Message> messages;
    std::vector<DisplayRow> rows;
    bool layoutDirty = true;
    std::function<void(bool)> layoutChangedCallback;
    bool showThinking_ = true;
    bool showAnalysis_ = true;
    int spinnerFrame_ = 0;
    std::function<void(std::size_t, const std::string &, const std::string &)> hiddenDetailCallback_;

    static std::string prefixForRole(Role role);
    void rebuildLayoutIfNeeded();
    void rebuildLayout();
    void notifyLayoutChanged(bool userScroll);
    void appendVisibleSegment(Role role,
                              const std::string &prefix,
                              const std::string &text,
                              std::size_t messageIndex,
                              bool &messageFirstRow,
                              int width,
                              bool thinking,
                              const std::string &channelLabel);
    std::size_t appendHiddenPlaceholder(const std::string &prefix,
                                        const std::string &channelLabel,
                                        const std::string &content,
                                        std::size_t messageIndex,
                                        bool pending,
                                        bool thinking,
                                        bool isFirstRow);
    void updateHiddenPlaceholder(std::size_t rowIndex,
                                 const std::string &prefix,
                                 const std::string &channelLabel,
                                 const std::string &content,
                                 std::size_t messageIndex,
                                 bool pending,
                                 bool thinking);
    void openHiddenRow(std::size_t rowIndex);

    static std::vector<DisplayRow::GlyphInfo> buildGlyphs(
        const std::string &text);
    static void finalizeDisplayRow(DisplayRow &row);
    static std::uint16_t glyphStyleMask(
        const DisplayRow &row, const DisplayRow::GlyphInfo &glyph);
};
