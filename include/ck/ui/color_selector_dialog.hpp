#pragma once

#include <cstdint>

#ifndef Uses_TDialog
#define CK_UI_COLOR_SELECTOR_DIALOG_DEFINE_USES_TDIALOG
#define Uses_TDialog
#endif
#ifndef Uses_TEvent
#define CK_UI_COLOR_SELECTOR_DIALOG_DEFINE_USES_TEVENT
#define Uses_TEvent
#endif
#include <tvision/tv.h>
#ifdef CK_UI_COLOR_SELECTOR_DIALOG_DEFINE_USES_TEVENT
#undef Uses_TEvent
#undef CK_UI_COLOR_SELECTOR_DIALOG_DEFINE_USES_TEVENT
#endif
#ifdef CK_UI_COLOR_SELECTOR_DIALOG_DEFINE_USES_TDIALOG
#undef Uses_TDialog
#undef CK_UI_COLOR_SELECTOR_DIALOG_DEFINE_USES_TDIALOG
#endif

namespace ck::ui
{

class ColorSelectorDialog : public TDialog
{
public:
    explicit ColorSelectorDialog(std::uint8_t background = 0x00,
                                 std::uint8_t foreground = 0x0F) noexcept;

    [[nodiscard]] std::uint8_t backgroundColor() const noexcept;
    [[nodiscard]] std::uint8_t foregroundColor() const noexcept;

protected:
    void handleEvent(TEvent &event) override;

private:
    class ColorGridView;
    class ColorDemoView;
    class ColorHintView;

    friend class ColorGridView;
    friend class ColorDemoView;
    friend class ColorHintView;

    void onColorCellClicked(std::uint8_t colorIndex,
                            unsigned short buttons,
                            unsigned short controlKeyState) noexcept;
    void updateColorViews();

    static std::uint8_t defaultForegroundFor(std::uint8_t background) noexcept;

    std::uint8_t background_;
    std::uint8_t foreground_;
    std::uint8_t cursorIndex_;
    ColorGridView *gridView_ = nullptr;
    ColorDemoView *demoView_ = nullptr;
    ColorHintView *hintView_ = nullptr;
};

ColorSelectorDialog *createColorSelectorDialog() noexcept;

} // namespace ck::ui
