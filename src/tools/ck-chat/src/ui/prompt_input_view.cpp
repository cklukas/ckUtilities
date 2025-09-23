#include "prompt_input_view.hpp"
#include <vector>
#include <cstring>
#include <limits>
#include <algorithm>

PromptInputView::PromptInputView(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll)
    : TMemo(bounds, hScroll, vScroll, nullptr, kBufferSize)
{
    options |= ofFirstClick;
}

TPalette &PromptInputView::getPalette() const
{
    return TEditor::getPalette();
}

std::string PromptInputView::text() const
{
    auto *self = const_cast<PromptInputView *>(this);
    std::vector<char> raw(self->dataSize());
    self->TMemo::getData(raw.data());
    const auto *memo = reinterpret_cast<const TMemoData *>(raw.data());
    return decodeEditorText(memo->buffer, memo->length);
}

void PromptInputView::setText(const std::string &value)
{
    setFromEncoded(encodeEditorText(value));
}

void PromptInputView::clearText()
{
    setFromEncoded(std::string{});
}

std::string PromptInputView::decodeEditorText(const char *data, ushort length)
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

std::string PromptInputView::encodeEditorText(const std::string &text)
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

void PromptInputView::setFromEncoded(const std::string &encoded)
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
