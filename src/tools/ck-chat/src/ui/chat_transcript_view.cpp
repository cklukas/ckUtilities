#include "chat_transcript_view.hpp"

#include <algorithm>

ChatTranscriptView::ChatTranscriptView(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll)
    : TScroller(bounds, hScroll, vScroll)
{
    options |= ofFirstClick;
    growMode = gfGrowHiX | gfGrowHiY;
    setLimit(1, 1);
}

void ChatTranscriptView::setMessages(const std::vector<ck::chat::ChatSession::Message> &sessionMessages)
{
    messages.clear();
    messages.reserve(sessionMessages.size());
    for (const auto &msg : sessionMessages)
    {
        messages.push_back(Message{msg.role, msg.content, msg.pending});
    }
    layoutDirty = true;
    rebuildLayout();
}

void ChatTranscriptView::clearMessages()
{
    messages.clear();
    rows.clear();
    layoutDirty = true;
    setLimit(1, 1);
    scrollTo(0, 0);
    drawView();
    notifyLayoutChanged();
}

void ChatTranscriptView::scrollToBottom()
{
    rebuildLayoutIfNeeded();
    int totalRows = static_cast<int>(rows.size());
    if (totalRows <= 0)
        totalRows = 1;
    int desired = std::max(0, totalRows - size.y);
    scrollTo(delta.x, desired);
    notifyLayoutChanged();
}

void ChatTranscriptView::setLayoutChangedCallback(std::function<void()> cb)
{
    layoutChangedCallback = std::move(cb);
}

bool ChatTranscriptView::messageForCopy(std::size_t index, std::string &out) const
{
    if (index >= messages.size())
        return false;
    const auto &msg = messages[index];
    if (msg.role != Role::Assistant)
        return false;
    out = msg.content;
    return true;
}

void ChatTranscriptView::setMessagePending(std::size_t index, bool pending)
{
    if (index >= messages.size())
        return;
    messages[index].pending = pending;
}

bool ChatTranscriptView::isMessagePending(std::size_t index) const
{
    if (index >= messages.size())
        return false;
    return messages[index].pending;
}

std::optional<int> ChatTranscriptView::firstRowForMessage(std::size_t index) const
{
    for (std::size_t row = 0; row < rows.size(); ++row)
    {
        if (rows[row].messageIndex == index && rows[row].isFirstLine)
            return static_cast<int>(row);
    }
    return std::nullopt;
}

void ChatTranscriptView::draw()
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

void ChatTranscriptView::changeBounds(const TRect &bounds)
{
    TScroller::changeBounds(bounds);
    layoutDirty = true;
    rebuildLayoutIfNeeded();
    notifyLayoutChanged();
}

void ChatTranscriptView::handleEvent(TEvent &event)
{
    TPoint before = delta;
    TScroller::handleEvent(event);
    if (before.x != delta.x || before.y != delta.y)
        notifyLayoutChanged();
}

std::string ChatTranscriptView::prefixForRole(Role role)
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

void ChatTranscriptView::rebuildLayoutIfNeeded()
{
    if (!layoutDirty)
        return;
    rebuildLayout();
}

void ChatTranscriptView::rebuildLayout()
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

void ChatTranscriptView::notifyLayoutChanged()
{
    if (layoutChangedCallback)
        layoutChangedCallback();
}
