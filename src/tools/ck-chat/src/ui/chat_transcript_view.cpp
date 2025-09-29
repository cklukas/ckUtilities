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
            std::string segment;
            if (end == std::string::npos)
                segment = std::string(content.substr(start));
            else
                segment = std::string(content.substr(start, end - start));

            std::string currentPrefix = firstLine ? prefix : indent;
            std::string fullLine = currentPrefix + segment;

            auto wrappedLines = wrapLines(fullLine, width);
            for (std::size_t idx = 0; idx < wrappedLines.size(); ++idx)
            {
                DisplayRow row;
                row.role = msg.role;
                row.messageIndex = i;
                row.isFirstLine = firstLine && idx == 0;
                row.text = wrappedLines[idx];
                rows.push_back(std::move(row));
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

std::vector<std::string> ChatTranscriptView::wrapLines(const std::string &text, int width) const
{
    std::vector<std::string> result;
    if (width <= 0)
    {
        result.push_back(text);
        return result;
    }

    std::string current;
    current.reserve(static_cast<std::size_t>(width));
    const char *ptr = text.c_str();
    std::size_t len = text.size();
    std::size_t pos = 0;

    while (pos < len)
    {
        std::size_t remaining = len - pos;
        std::size_t spaceLeft = static_cast<std::size_t>(width > 0 ? width : 1);

        if (remaining <= spaceLeft)
        {
            current.append(ptr + pos, remaining);
            result.push_back(current);
            break;
        }

        std::size_t wrapPos = pos + spaceLeft;
        std::size_t lastSpace = std::string::npos;
        for (std::size_t i = pos; i < wrapPos; ++i)
        {
            if (std::isspace(static_cast<unsigned char>(ptr[i])))
                lastSpace = i;
        }

        if (lastSpace != std::string::npos && lastSpace >= pos)
        {
            current.append(ptr + pos, lastSpace - pos);
            result.push_back(current);
            current.clear();
            pos = lastSpace + 1;
            while (pos < len && std::isspace(static_cast<unsigned char>(ptr[pos])))
                ++pos;
        }
        else
        {
            current.append(ptr + pos, spaceLeft);
            result.push_back(current);
            current.clear();
            pos += spaceLeft;
        }
    }

    if (result.empty())
        result.push_back(std::string());

    return result;
}
