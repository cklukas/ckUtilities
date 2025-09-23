#pragma once

#include "../tvision_include.hpp"
#include <string>

class PromptInputView : public TMemo
{
public:
    static constexpr ushort kBufferSize = 8192;

    PromptInputView(const TRect &bounds, TScrollBar *hScroll, TScrollBar *vScroll);

    virtual TPalette &getPalette() const override;

    std::string text() const;
    void setText(const std::string &value);
    void clearText();

private:
    static std::string decodeEditorText(const char *data, ushort length);
    static std::string encodeEditorText(const std::string &text);
    void setFromEncoded(const std::string &encoded);
};
