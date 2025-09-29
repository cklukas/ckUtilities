#include "chat_transcript_view.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string_view>
#include <utility>

namespace {
struct ChannelSegment
{
  std::string channel;
  std::string text;
  bool from_marker = false;
};

std::string_view trim_view(std::string_view view)
{
  const char *whitespace = " \t\r\n";
  auto begin = view.find_first_not_of(whitespace);
  if (begin == std::string_view::npos)
    return std::string_view();
  auto end = view.find_last_not_of(whitespace);
  return view.substr(begin, end - begin + 1);
}

std::string trim_copy(std::string_view view)
{
  std::string_view trimmed = trim_view(view);
  return std::string(trimmed);
}

std::string to_lower_copy(std::string_view view)
{
  std::string lowered;
  lowered.reserve(view.size());
  for (char ch : view)
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  return lowered;
}

bool is_final_channel(std::string_view channel)
{
  std::string trimmed = trim_copy(channel);
  std::string lowered = to_lower_copy(trimmed);
  return lowered.empty() || lowered == "final";
}

std::string sanitize_for_display(const std::string &input)
{
  std::string output;
  output.reserve(input.size());

  const unsigned char *data = reinterpret_cast<const unsigned char *>(input.data());
  std::size_t len = input.size();
  std::size_t i = 0;

  auto append_ascii = [&](char ch) {
    if (ch == '\r')
      return;
    if (ch >= 0x20 || ch == '\n' || ch == '\t')
      output.push_back(ch);
    else
      output.push_back(' ');
  };

  while (i < len)
  {
    unsigned char byte = data[i];
    if (byte < 0x80)
    {
      append_ascii(static_cast<char>(byte));
      ++i;
      continue;
    }

    std::size_t startIndex = i;
    std::size_t extra = 0;
    uint32_t codepoint = 0;
    if ((byte & 0xE0) == 0xC0)
    {
      extra = 1;
      codepoint = byte & 0x1F;
    }
    else if ((byte & 0xF0) == 0xE0)
    {
      extra = 2;
      codepoint = byte & 0x0F;
    }
    else if ((byte & 0xF8) == 0xF0)
    {
      extra = 3;
      codepoint = byte & 0x07;
    }
    else
    {
      output.push_back('?');
      ++i;
      continue;
    }

    if (i + extra >= len)
    {
      output.push_back('?');
      break;
    }

    bool valid = true;
    for (std::size_t j = 1; j <= extra; ++j)
    {
      unsigned char follow = data[i + j];
      if ((follow & 0xC0) != 0x80)
      {
        valid = false;
        break;
      }
      codepoint = (codepoint << 6) | (follow & 0x3F);
    }

    if (!valid)
    {
      output.push_back('?');
      ++i;
      continue;
    }

    i += extra + 1;

    auto append_replacement = [&](char ch) {
      if (ch == '\r')
        return;
      output.push_back(ch);
    };

    switch (codepoint)
    {
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x2032:
      append_replacement('\'');
      break;
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x2033:
      append_replacement('"');
      break;
    case 0x2013:
    case 0x2014:
    case 0x2212:
      append_replacement('-');
      break;
    case 0x2026:
      output.append("...");
      break;
    case 0x2122:
      output.append("(TM)");
      break;
    case 0x00A9:
      output.append("(C)");
      break;
    case 0x00AE:
      output.append("(R)");
      break;
    case 0x00A0:
    case 0x2007:
    case 0x2009:
    case 0x200A:
    case 0x200B:
      output.push_back(' ');
      break;
    default:
      if (codepoint <= 0x7E)
        append_replacement(static_cast<char>(codepoint));
      else if (codepoint == '\n')
        output.push_back('\n');
      else if (codepoint == '\t')
        output.push_back('\t');
      else
        output.append(input, startIndex, extra + 1);
      break;
    }
  }

  return output;
}

std::string last_non_empty_line(const std::string &text)
{
  std::string sanitized = sanitize_for_display(text);
  std::string_view view(sanitized);
  if (view.empty())
    return std::string();

  std::size_t pos = view.size();
  while (pos > 0)
  {
    std::size_t newlinePos = view.rfind('\n', pos - 1);
    std::size_t start = (newlinePos == std::string_view::npos) ? 0 : newlinePos + 1;
    std::string_view candidate = view.substr(start, pos - start);
    candidate = trim_view(candidate);
    if (!candidate.empty())
      return std::string(candidate);
    if (newlinePos == std::string_view::npos)
      break;
    pos = newlinePos;
  }
  return std::string(trim_view(view));
}

std::vector<ChannelSegment> parse_harmony_segments(const std::string &content)
{
  std::vector<ChannelSegment> segments;
  const std::string startToken = "<|start|>assistant<|channel|>";
  const std::string messageToken = "<|message|>";
  const std::string endToken = "<|end|>";

  std::size_t pos = 0;
  while (pos < content.size())
  {
    std::size_t start = content.find("<|start|>", pos);
    if (start == std::string::npos)
    {
      std::string trailing = content.substr(pos);
      if (!trailing.empty())
        segments.push_back(ChannelSegment{"", std::move(trailing), false});
      break;
    }

    if (start > pos)
    {
      std::string plain = content.substr(pos, start - pos);
      if (!plain.empty())
        segments.push_back(ChannelSegment{"", std::move(plain), false});
    }

    if (content.compare(start, startToken.size(), startToken) != 0)
    {
      // Not a harmony assistant marker; treat as literal character.
      std::string literal = content.substr(start, 1);
      segments.push_back(ChannelSegment{"", std::move(literal)});
      pos = start + 1;
      continue;
    }

    std::size_t channelStart = start + startToken.size();
    std::size_t messagePos = content.find(messageToken, channelStart);
    if (messagePos == std::string::npos)
    {
      std::string remainder = content.substr(start);
      if (!remainder.empty())
        segments.push_back(ChannelSegment{"", std::move(remainder), false});
      break;
    }

    std::string channel = content.substr(channelStart, messagePos - channelStart);
    std::size_t bodyStart = messagePos + messageToken.size();
    std::size_t endPos = content.find(endToken, bodyStart);
    if (endPos == std::string::npos)
    {
      std::string body = content.substr(bodyStart);
      segments.push_back(ChannelSegment{std::move(channel), std::move(body)});
      break;
    }

    std::string body = content.substr(bodyStart, endPos - bodyStart);
    segments.push_back(ChannelSegment{std::move(channel), std::move(body), true});
    pos = endPos + endToken.size();
  }

  return segments;
}
} // namespace

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

void ChatTranscriptView::setHiddenDetailCallback(
    std::function<void(std::size_t, const std::string &, const std::string &)> cb)
{
    hiddenDetailCallback_ = std::move(cb);
}

bool ChatTranscriptView::messageForCopy(std::size_t index, std::string &out) const
{
    if (index >= messages.size())
        return false;
    const auto &msg = messages[index];
    if (msg.role != Role::Assistant)
        return false;
    auto segments = parse_harmony_segments(msg.content);
    if (!segments.empty())
    {
        std::string assembled;
        for (const auto &segment : segments)
        {
            if (is_final_channel(segment.channel))
                assembled += segment.text;
        }
        if (!assembled.empty())
        {
            out = std::move(assembled);
            return true;
        }
    }
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

void ChatTranscriptView::setShowThinking(bool show)
{
    if (showThinking_ == show)
        return;
    showThinking_ = show;
    layoutDirty = true;
    rebuildLayoutIfNeeded();
    drawView();
    notifyLayoutChanged();
}

void ChatTranscriptView::setShowAnalysis(bool show)
{
    if (showAnalysis_ == show)
        return;
    showAnalysis_ = show;
    layoutDirty = true;
    rebuildLayoutIfNeeded();
    drawView();
    notifyLayoutChanged();
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
            {
                if (row.isThinking)
                    setFore(attr, TColorDesired(TColorBIOS(0x08)));
                else
                    setFore(attr, TColorDesired(TColorBIOS(0x01)));
            }
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
    spinnerFrame_ = (spinnerFrame_ + 1) % 4;

    for (std::size_t i = 0; i < messages.size(); ++i)
    {
        const auto &msg = messages[i];
        bool messageFirstRow = true;

        if (msg.role == Role::Assistant)
        {
            auto segments = parse_harmony_segments(msg.content);
            bool hasMarker = std::any_of(segments.begin(), segments.end(),
                                         [](const ChannelSegment &seg)
                                         { return seg.from_marker; });
            bool finalSeen = false;

            struct HiddenAggregate
            {
                std::string channel;
                std::string text;
                bool pending = false;
                bool thinking = true;
            };
            std::vector<HiddenAggregate> hidden;
            std::unordered_map<std::string, std::size_t> hiddenIndex;

            auto recordHidden = [&](const std::string &channelLabel,
                                    const std::string &segmentText,
                                    bool thinking)
            {
                std::string key = channelLabel.empty() ? std::string("analysis") : channelLabel;
                auto it = hiddenIndex.find(key);
                if (it == hiddenIndex.end())
                {
                    hidden.push_back(HiddenAggregate{key, std::string(), msg.pending, thinking});
                    it = hiddenIndex.emplace(key, hidden.size() - 1).first;
                }
                auto &agg = hidden[it->second];
                if (!agg.text.empty())
                    agg.text.push_back('\n');
                agg.text += segmentText;
                agg.pending = msg.pending;
                agg.thinking = thinking;
            };

            auto handleVisibleSegment = [&](const std::string &channel, const std::string &text,
                                            bool thinking)
            {
                std::string label = channel.empty() ? std::string("analysis") : channel;
                std::string prefix = thinking ? "Assistant (" + label + "): " : "Assistant: ";
                appendVisibleSegment(Role::Assistant, prefix, text, i, messageFirstRow, width, thinking, label);
            };

            auto normalizedChannel = [](const std::string &label) {
                std::string printable = sanitize_for_display(label);
                if (printable.empty())
                    printable = "analysis";
                return printable;
            };

            std::function<bool(const std::string &, bool)> shouldHide = [&](const std::string &channelLower,
                                                                            bool thinking) {
                if (channelLower == "analysis")
                    return !showAnalysis_;
                if (thinking)
                    return !showThinking_;
                return false;
            };

            if (segments.empty())
            {
                std::string original = msg.content;
                std::string channelLabel;

                const std::string channelToken = "<|channel|>";
                const std::string messageToken = "<|message|>";

                auto channelPos = original.find(channelToken);
                if (channelPos != std::string::npos)
                {
                    std::size_t headerStart = channelPos + channelToken.size();
                    auto messagePos = original.find(messageToken, headerStart);
                    if (messagePos != std::string::npos)
                    {
                        channelLabel = trim_copy(original.substr(headerStart, messagePos - headerStart));
                        original.erase(channelPos, (messagePos + messageToken.size()) - channelPos);
                    }
                }

                std::size_t endTag = original.find("<|end|");
                if (endTag != std::string::npos)
                    original.erase(endTag);

                std::size_t startTag = original.find("<|start|");
                if (startTag != std::string::npos)
                    original.erase(startTag, std::string::npos);

                std::string cleaned = sanitize_for_display(original);

                bool thinkingSegment = hasMarker || !channelLabel.empty() ||
                                        msg.content.find("<|channel|") != std::string::npos;
                std::string printableChannel = normalizedChannel(channelLabel);
                std::string channelLower = to_lower_copy(printableChannel);

                if (shouldHide(channelLower, thinkingSegment))
                {
                    recordHidden(printableChannel, cleaned, thinkingSegment);
                    messageFirstRow = false;
                }
                else
                {
                    handleVisibleSegment(printableChannel, cleaned, thinkingSegment);
                }
            }
            else
            {
                for (const auto &segment : segments)
                {
                    std::string channelLabel = trim_copy(segment.channel);
                    std::string text = segment.text;

                    if (channelLabel.empty())
                    {
                        const std::string channelToken = "<|channel|>";
                        const std::string messageToken = "<|message|>";
                        auto channelPos = text.find(channelToken);
                        if (channelPos != std::string::npos)
                        {
                            std::size_t headerStart = channelPos + channelToken.size();
                            auto messagePos = text.find(messageToken, headerStart);
                            if (messagePos != std::string::npos)
                            {
                                channelLabel = trim_copy(text.substr(headerStart, messagePos - headerStart));
                                text.erase(channelPos, (messagePos + messageToken.size()) - channelPos);
                            }
                        }
                        auto endTag = text.find("<|end|");
                        if (endTag != std::string::npos)
                            text.erase(endTag);
                        auto startTag = text.find("<|start|");
                        if (startTag != std::string::npos)
                            text.erase(startTag, std::string::npos);
                    }

                    bool finalSegment = (!channelLabel.empty() && is_final_channel(channelLabel));
                    bool thinkingSegment = channelLabel.empty()
                                                ? (segment.from_marker && !finalSegment) || (hasMarker && !finalSeen)
                                                : !finalSegment;

                    std::string printableChannel = normalizedChannel(channelLabel);
                    std::string channelLower = to_lower_copy(printableChannel);
                    std::string segmentText = sanitize_for_display(text);

                    if (shouldHide(channelLower, thinkingSegment))
                    {
                        recordHidden(printableChannel, segmentText, thinkingSegment);
                        messageFirstRow = false;
                    }
                    else
                    {
                        handleVisibleSegment(printableChannel, segmentText, thinkingSegment);
                    }

                    if ((segment.from_marker || !channelLabel.empty()) && finalSegment)
                        finalSeen = true;
                }
            }

            for (auto &agg : hidden)
            {
                std::string trimmed = trim_copy(agg.text);
                appendHiddenPlaceholder("Assistant (" + agg.channel + "): ", agg.channel,
                                        std::string(trimmed), i, agg.pending, agg.thinking);
                messageFirstRow = false;
            }
        }
        else
        {
            std::string prefix = prefixForRole(msg.role);
            std::string sanitized = sanitize_for_display(msg.content);
            appendVisibleSegment(msg.role, prefix, sanitized, i, messageFirstRow, width, false, std::string());
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
        total = 1;
    setLimit(1, total);
    layoutDirty = false;
    notifyLayoutChanged();
}

void ChatTranscriptView::appendVisibleSegment(Role role,
                                              const std::string &prefix,
                                              const std::string &text,
                                              std::size_t messageIndex,
                                              bool &messageFirstRow,
                                              int width,
                                              bool thinking,
                                              const std::string &channelLabel)
{
    std::string indent(prefix.size(), ' ');
    std::string_view content(text);
    std::size_t start = 0;
    bool segmentFirstLine = true;

    auto emitWrapped = [&](const std::string &line, bool firstLineInSegment) {
        const std::string &currentPrefix = firstLineInSegment ? prefix : indent;
        std::string fullLine = currentPrefix + line;
        auto wrappedLines = wrapLines(fullLine, width);
        if (wrappedLines.empty())
            wrappedLines.push_back(currentPrefix);
        for (std::size_t idx = 0; idx < wrappedLines.size(); ++idx)
        {
            DisplayRow row;
            row.role = role;
            row.messageIndex = messageIndex;
            row.isFirstLine = messageFirstRow && firstLineInSegment && idx == 0;
            row.isThinking = thinking;
            row.channelLabel = channelLabel;
            row.text = wrappedLines[idx];
            rows.push_back(std::move(row));
            messageFirstRow = false;
        }
    };

    if (content.empty())
    {
        emitWrapped(std::string(), true);
        return;
    }

    while (true)
    {
        std::size_t end = content.find('\n', start);
        std::string segment;
        if (end == std::string_view::npos)
            segment = std::string(content.substr(start));
        else
            segment = std::string(content.substr(start, end - start));

        emitWrapped(segment, segmentFirstLine);

        if (end == std::string_view::npos)
            break;
        start = end + 1;
        segmentFirstLine = false;
        if (start >= content.size())
        {
            DisplayRow row;
            row.role = role;
            row.messageIndex = messageIndex;
            row.isFirstLine = false;
            row.isThinking = thinking;
            row.channelLabel = channelLabel;
            row.text = indent;
            rows.push_back(std::move(row));
            break;
        }
    }
}

void ChatTranscriptView::appendHiddenPlaceholder(const std::string &prefix,
                                                 const std::string &channelLabel,
                                                 const std::string &content,
                                                 std::size_t messageIndex,
                                                 bool pending,
                                                 bool thinking)
{
    DisplayRow row;
    row.role = Role::Assistant;
    row.messageIndex = messageIndex;
    row.isFirstLine = true;
    row.isThinking = thinking;
    row.isPlaceholder = true;
    row.isPending = pending;
    row.channelLabel = channelLabel;
    row.hiddenContent = content.empty() ? std::string("(no content)") : content;

    static const char spinnerChars[] = {'|', '/', '-', '\\'};
    if (pending)
    {
        char frame = spinnerChars[spinnerFrame_ % (sizeof(spinnerChars) / sizeof(spinnerChars[0]))];
        row.text = prefix + "Generating… " + frame;
    }
    else
    {
        row.text = prefix + "[Analysis finished – click to view]";
    }

    rows.push_back(std::move(row));
}

void ChatTranscriptView::openHiddenRow(std::size_t rowIndex)
{
    if (rowIndex >= rows.size())
        return;
    const auto &row = rows[rowIndex];
    if (!row.isPlaceholder || row.isPending || !hiddenDetailCallback_)
        return;
    hiddenDetailCallback_(row.messageIndex, row.channelLabel, row.hiddenContent);
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
