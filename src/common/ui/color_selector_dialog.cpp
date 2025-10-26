#include "ck/ui/color_selector_dialog.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#define Uses_TButton
#define Uses_TColorAttr
#define Uses_TDialog
#define Uses_TDrawBuffer
#define Uses_TEvent
#define Uses_TKeys
#define Uses_TPoint
#define Uses_TRect
#define Uses_TStaticText
#define Uses_TView
#define Uses_TWindow
#include <tvision/tv.h>
#include <tvision/colors.h>
#include <tvision/drawbuf.h>
#include <tvision/system.h>
#include <tvision/tkeys.h>

namespace ck::ui
{
namespace
{
    struct ColorInfo
    {
        const char *name;
        std::uint8_t index;
    };

    constexpr std::array<ColorInfo, 16> kColorInfo = {{
        {"Black", 0x00},
        {"Blue", 0x01},
        {"Green", 0x02},
        {"Cyan", 0x03},
        {"Red", 0x04},
        {"Magenta", 0x05},
        {"Brown", 0x06},
        {"LightGray", 0x07},
        {"DarkGray", 0x08},
        {"LightBlue", 0x09},
        {"LightGreen", 0x0A},
        {"LightCyan", 0x0B},
        {"LightRed", 0x0C},
        {"LightMagenta", 0x0D},
        {"Yellow", 0x0E},
        {"White", 0x0F},
    }};

    constexpr std::uint8_t toIndex(std::size_t value) noexcept
    {
        return static_cast<std::uint8_t>(value & 0x0F);
    }

    std::uint8_t clampIndex(std::uint8_t color) noexcept
    {
        return static_cast<std::uint8_t>(std::min<std::size_t>(color, kColorInfo.size() - 1));
    }

    std::uint8_t contrastingForeground(std::uint8_t background) noexcept
    {
        return (background < 0x08) ? 0x0F : 0x00;
    }

    const ColorInfo &colorInfo(std::uint8_t index) noexcept
    {
        return kColorInfo[clampIndex(index)];
    }

} // namespace

class ColorSelectorDialog::ColorGridView : public TView
{
public:
    static constexpr int kColumns = 8;
    static constexpr int kRows = 2;
    static constexpr int kCellWidth = 4;
    static constexpr int kCellHeight = 2;
    static constexpr int kWidth = kColumns * kCellWidth;
    static constexpr int kHeight = kRows * kCellHeight;

    ColorGridView(const TRect &bounds, ColorSelectorDialog &owner) noexcept
        : TView(bounds), owner_(owner)
    {
        options |= ofSelectable | ofFirstClick;
        eventMask |= evMouseDown | evKeyboard;
    }

    void setCursorIndex(std::uint8_t index) noexcept
    {
        cursorIndex_ = clampIndex(index);
        drawView();
    }

    void draw() override
    {
        TDrawBuffer buffer;
        const TColorAttr base = getColor(1);
        const TColorAttr highlight = getColor(2);

        for (int row = 0; row < kRows; ++row)
        {
            buffer.moveChar(0, ' ', base, size.x);
            for (int col = 0; col < kColumns; ++col)
            {
                const int colorIdx = row * kColumns + col;
                if (colorIdx >= static_cast<int>(kColorInfo.size()))
                    continue;
                const auto info = colorInfo(toIndex(colorIdx));
                const short cellX = static_cast<short>(col * kCellWidth);
                const TColorAttr cellAttr{TColorBIOS(contrastingForeground(info.index)), TColorBIOS(info.index)};
                buffer.moveChar(cellX, ' ', cellAttr, kCellWidth);
                if (owner_.backgroundColor() == info.index)
                    buffer.moveChar(cellX, 'B', cellAttr, 1);
                if (owner_.foregroundColor() == info.index)
                    buffer.moveChar(static_cast<int>(cellX + kCellWidth - 1), 'F', cellAttr, 1);
            }
            writeLine(0, static_cast<short>(row * kCellHeight), size.x, 1, buffer);

            buffer.moveChar(0, ' ', base, size.x);
            for (int col = 0; col < kColumns; ++col)
            {
                const int colorIdx = row * kColumns + col;
                if (colorIdx >= static_cast<int>(kColorInfo.size()))
                    continue;
                const auto info = colorInfo(toIndex(colorIdx));
                const short cellX = static_cast<short>(col * kCellWidth);
                char label[5];
                std::snprintf(label, sizeof(label), "0x%X", info.index);
                const int length = static_cast<int>(std::strlen(label));
                const int start = cellX + std::max(0, (kCellWidth - length) / 2);
                const TColorAttr &labelAttr = (cursorIndex_ == info.index) ? highlight : base;
                buffer.moveStr(start, label, labelAttr);
            }
            writeLine(0, static_cast<short>(row * kCellHeight + 1), size.x, 1, buffer);
        }
    }

    void handleEvent(TEvent &event) override
    {
        if (event.what == evMouseDown)
        {
            const TPoint where = makeLocal(event.mouse.where);
            const auto hit = hitTest(where);
            if (hit.first)
            {
                cursorIndex_ = hit.second;
                owner_.onColorCellClicked(hit.second,
                                          event.mouse.buttons,
                                          event.mouse.controlKeyState);
                clearEvent(event);
            }
            return;
        }
        else if (event.what == evKeyDown)
        {
            bool handled = false;
            const unsigned key = event.keyDown.keyCode;
            if (key == kbLeft)
            {
                handled = moveCursor(-1, 0);
            }
            else if (key == kbRight)
            {
                handled = moveCursor(1, 0);
            }
            else if (key == kbUp)
            {
                handled = moveCursor(0, -1);
            }
            else if (key == kbDown)
            {
                handled = moveCursor(0, 1);
            }
            else if (key == kbEnter || event.keyDown.charScan.charCode == ' ')
            {
                owner_.onColorCellClicked(cursorIndex_,
                                          mbLeftButton,
                                          event.keyDown.controlKeyState);
                handled = true;
            }
#ifdef kbCtrlEnter
            else if (key == kbCtrlEnter)
            {
                unsigned short mask = 0;
#ifdef mbMiddleButton
                mask = mbMiddleButton;
#endif
                    owner_.onColorCellClicked(cursorIndex_,
                                              mask,
                                              event.keyDown.controlKeyState);
                handled = true;
            }
#endif
            if (handled)
            {
                clearEvent(event);
                return;
            }
        }
        TView::handleEvent(event);
    }

private:
    ColorSelectorDialog &owner_;
    std::uint8_t cursorIndex_ = 0;

    std::pair<bool, std::uint8_t> hitTest(const TPoint &point) const noexcept
    {
        if (point.x < 0 || point.y < 0 || point.x >= size.x || point.y >= size.y)
            return {false, 0};
        const int column = point.x / kCellWidth;
        const int row = point.y / kCellHeight;
        const int idx = row * kColumns + column;
        if (idx < 0 || idx >= static_cast<int>(kColorInfo.size()))
            return {false, 0};
        return {true, colorInfo(toIndex(idx)).index};
    }

    bool moveCursor(int dx, int dy) noexcept
    {
        int current = cursorIndex_;
        int column = current % kColumns;
        int row = current / kColumns;
        column = std::clamp(column + dx, 0, kColumns - 1);
        row = std::clamp(row + dy, 0, kRows - 1);
        int next = row * kColumns + column;
        next = std::clamp(next, 0, static_cast<int>(kColorInfo.size()) - 1);
        std::uint8_t nextIndex = colorInfo(toIndex(next)).index;
        if (nextIndex == cursorIndex_)
            return false;
        cursorIndex_ = nextIndex;
        drawView();
        return true;
    }
};

class ColorSelectorDialog::ColorDemoView : public TView
{
public:
    explicit ColorDemoView(const TRect &bounds) noexcept
        : TView(bounds)
    {
        options |= ofFramed;
    }

    void setColors(std::uint8_t background, std::uint8_t foreground) noexcept
    {
        background_ = clampIndex(background);
        foreground_ = clampIndex(foreground);
        drawView();
    }

    void draw() override
    {
        TDrawBuffer buffer;
        std::string text = " Turbo Vision Color Preview ";
        const int centerLine = size.y / 2;
        const TColorAttr attr{TColorBIOS(foreground_), TColorBIOS(background_)};
        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', attr, size.x);
            if (y == centerLine)
            {
                if (static_cast<int>(text.size()) > size.x)
                    text.resize(static_cast<std::size_t>(size.x));
                int start = std::max(0, (size.x - static_cast<int>(text.size())) / 2);
                if (!text.empty())
                    buffer.moveStr(start, text.c_str(), attr);
            }
            writeLine(0, static_cast<short>(y), size.x, 1, buffer);
        }
    }

private:
    std::uint8_t background_ = 0x00;
    std::uint8_t foreground_ = 0x0F;
};

class ColorSelectorDialog::ColorHintView : public TView
{
public:
    explicit ColorHintView(const TRect &bounds) noexcept
        : TView(bounds)
    {
        growMode = gfFixed;
    }

    void setColors(std::uint8_t background, std::uint8_t foreground) noexcept
    {
        background_ = clampIndex(background);
        foreground_ = clampIndex(foreground);
        drawView();
    }

    void draw() override
    {
        TDrawBuffer buffer;
        const TColorAttr attr = getColor(1);
        const std::string bgLine = formatLine("Background", background_);
        const std::string fgLine = formatLine("Foreground", foreground_);
        for (int y = 0; y < size.y; ++y)
        {
            buffer.moveChar(0, ' ', attr, size.x);
            if (y == 0)
                buffer.moveStr(0, bgLine.c_str(), attr);
            else if (y == 1)
                buffer.moveStr(0, fgLine.c_str(), attr);
            writeLine(0, static_cast<short>(y), size.x, 1, buffer);
        }
    }

private:
    std::uint8_t background_ = 0x00;
    std::uint8_t foreground_ = 0x0F;

    static std::string formatLine(const char *label, std::uint8_t index)
    {
        const auto &info = colorInfo(index);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s: TColorBIOS::%s (%u)", label, info.name, info.index);
        return buffer;
    }
};

ColorSelectorDialog::ColorSelectorDialog(std::uint8_t background, std::uint8_t foreground) noexcept
    : TWindowInit(&ColorSelectorDialog::initFrame),
      TDialog(TRect(0, 0, 46, 19), "Color Selector"),
      background_(clampIndex(background)),
      foreground_(clampIndex(foreground)),
      cursorIndex_(clampIndex(background))
{
    flags &= ~(wfGrow | wfZoom);
    growMode = gfGrowHiX | gfGrowHiY;
    palette = dpGrayDialog;

    if (foreground_ == background_)
        foreground_ = defaultForegroundFor(background_);

    const short marginX = 2;
    const short marginY = 2;
    const short contentWidth = static_cast<short>(size.x - marginX * 2);

    const short gridWidth = ColorGridView::kWidth;
    const short gridLeft = static_cast<short>(marginX + std::max<short>(0, (contentWidth - gridWidth) / 2));
    const short gridTop = marginY;
    const short gridBottom = static_cast<short>(gridTop + ColorGridView::kHeight);
    const short gridRight = static_cast<short>(gridLeft + gridWidth);
    auto *grid = new ColorGridView(TRect(gridLeft, gridTop, gridRight, gridBottom), *this);
    gridView_ = grid;
    insert(grid);
    grid->select();

    TRect instructionsRect(marginX,
                           static_cast<short>(gridBottom + 1),
                           static_cast<short>(size.x - marginX),
                           static_cast<short>(gridBottom + 2));
    auto *instructions = new TStaticText(instructionsRect,
                                         "Left click: foregr.  Shift/Middle: backgr.");
    insert(instructions);

    TRect demoRect(marginX, static_cast<short>(instructionsRect.b.y + 1),
                   static_cast<short>(size.x - marginX),
                   static_cast<short>(instructionsRect.b.y + 3));
    auto *demo = new ColorDemoView(demoRect);
    demoView_ = demo;
    insert(demo);

    TRect hintRect(marginX, static_cast<short>(demoRect.b.y + 1), static_cast<short>(size.x - marginX),
                   static_cast<short>(demoRect.b.y + 3));
    auto *hint = new ColorHintView(hintRect);
    hintView_ = hint;
    insert(hint);

    TRect closeRect(static_cast<short>(size.x - marginX - 12), static_cast<short>(size.y - 3),
                    static_cast<short>(size.x - marginX), static_cast<short>(size.y - 1));
    insert(new TButton(closeRect, "~C~lose", cmClose, bfDefault));

    updateColorViews();
}

std::uint8_t ColorSelectorDialog::backgroundColor() const noexcept
{
    return background_;
}

std::uint8_t ColorSelectorDialog::foregroundColor() const noexcept
{
    return foreground_;
}

void ColorSelectorDialog::handleEvent(TEvent &event)
{
    TDialog::handleEvent(event);
    if (event.what == evCommand && event.message.command == cmClose)
        clearEvent(event);
}

void ColorSelectorDialog::onColorCellClicked(std::uint8_t colorIndex,
                                             unsigned short buttons,
                                             unsigned short controlKeyState) noexcept
{
    cursorIndex_ = clampIndex(colorIndex);
    bool updated = false;
    const bool shiftPressed = (controlKeyState & kbShift) != 0;

    bool applyBackground = shiftPressed;
#ifdef mbMiddleButton
    applyBackground = applyBackground || ((buttons & mbMiddleButton) != 0);
#endif

    bool applyForeground = false;
    if (!applyBackground)
    {
        if ((buttons & mbLeftButton) != 0 || buttons == 0)
            applyForeground = true;
    }

    if (applyBackground)
    {
        const std::uint8_t newBackground = cursorIndex_;
        if (background_ != newBackground)
        {
            background_ = newBackground;
            updated = true;
        }
        if (foreground_ == background_)
        {
            const std::uint8_t fallback = defaultForegroundFor(background_);
            if (foreground_ != fallback)
            {
                foreground_ = fallback;
                updated = true;
            }
        }
    }

    if (applyForeground)
    {
        const std::uint8_t newForeground = cursorIndex_;
        if (foreground_ != newForeground)
        {
            foreground_ = newForeground;
            updated = true;
        }
    }

    if (updated)
        updateColorViews();
    else if (gridView_)
        gridView_->setCursorIndex(cursorIndex_);
}

void ColorSelectorDialog::updateColorViews()
{
    if (gridView_)
        gridView_->setCursorIndex(cursorIndex_);
    if (demoView_)
        demoView_->setColors(background_, foreground_);
    if (hintView_)
        hintView_->setColors(background_, foreground_);
}

std::uint8_t ColorSelectorDialog::defaultForegroundFor(std::uint8_t background) noexcept
{
    return contrastingForeground(background);
}

ColorSelectorDialog *createColorSelectorDialog() noexcept
{
    return new ColorSelectorDialog();
}

} // namespace ck::ui
