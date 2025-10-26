#include "ck/ui/clock_view.hpp"

#include "ck/ui/calendar.hpp"
#include "ck/ui/clock_aware_application.hpp"
#include "ck/ui/clock_settings.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <tvision/system.h>

namespace ck::ui
{
    namespace
    {
        std::tm localTime(std::time_t time)
        {
            std::tm result{};
#if defined(_WIN32)
            localtime_s(&result, &time);
#else
            localtime_r(&time, &result);
#endif
            return result;
        }
    } // namespace

    ClockView::ClockView(const TRect &bounds)
        : TView(bounds)
    {
        eventMask |= evMouseDown;
        currentTime_.fill('\0');
        refreshTime();
        mode_ = loadClockDisplayMode();
        displayedText_.clear();
    }

    void ClockView::draw()
    {
        TDrawBuffer buffer;
        TColorAttr color = getColor(2);

        buffer.moveChar(0, ' ', color, size.x);

        const short textLength = static_cast<short>(displayedText_.size());
        const short startColumn = std::max<short>(0, static_cast<short>(size.x - textLength));
        buffer.moveStr(startColumn, displayedText_.c_str(), color);

        writeLine(0, 0, size.x, 1, buffer);
    }

    void ClockView::update()
    {
        refreshTime();
        const std::string nextDisplay = formatDisplay(mode_);
        if (nextDisplay != displayedText_)
        {
            ensureWidthFor(nextDisplay);
            displayedText_ = nextDisplay;
            drawView();
        }
    }

    void ClockView::refreshTime()
    {
        const auto now = std::chrono::system_clock::now();
        const auto currentTime = std::chrono::system_clock::to_time_t(now);
        currentEpoch_ = currentTime;
        const std::tm local = localTime(currentTime);

        std::strftime(currentTime_.data(), currentTime_.size(), "%H:%M:%S", &local);
    }

    std::string ClockView::formatDisplay(ClockDisplayMode mode) const
    {
        switch (mode)
        {
        case ClockDisplayMode::Time:
            return std::string(currentTime_.data());
        case ClockDisplayMode::Date:
        {
            char buffer[64]{};
            std::tm local = localTime(currentEpoch_);
            if (std::strftime(buffer, sizeof(buffer), "%a %x", &local) == 0)
                return "";
            return std::string(buffer);
        }
        case ClockDisplayMode::Icon:
            return "\xF0\x9F\x93\x85"; // Calendar icon
        }
        return std::string();
    }

    void ClockView::cycleMode()
    {
        ClockDisplayMode next = mode_;
        switch (mode_)
        {
        case ClockDisplayMode::Time:
            next = ClockDisplayMode::Date;
            break;
        case ClockDisplayMode::Date:
            next = ClockDisplayMode::Icon;
            break;
        case ClockDisplayMode::Icon:
            next = ClockDisplayMode::Time;
            break;
        }
        applyMode(next);
    }

    void ClockView::applyMode(ClockDisplayMode mode)
    {
        if (mode_ == mode && !displayedText_.empty())
            return;

        mode_ = mode;
        refreshTime();
        const std::string nextDisplay = formatDisplay(mode_);
        ensureWidthFor(nextDisplay);
        displayedText_ = nextDisplay;
        drawView();
    }

    void ClockView::setDisplayMode(ClockDisplayMode mode)
    {
        applyMode(mode);
    }

    void ClockView::ensureWidthFor(const std::string &text)
    {
        const short desiredWidth = static_cast<short>(std::max<std::size_t>(1, text.size()));
        if (desiredWidth == size.x)
            return;
        bringIntoViewBounds(desiredWidth);
    }

    void ClockView::bringIntoViewBounds(short desiredWidth)
    {
        if (desiredWidth <= 0)
            return;

        if (owner)
        {
            const TRect current = getBounds();
            TRect parentExtent = owner->getExtent();
            const short right = parentExtent.b.x;
            const short width = std::min<short>(desiredWidth, static_cast<short>(right - parentExtent.a.x));
            const short left = static_cast<short>(std::max<int>(parentExtent.a.x, right - width));
            TRect newBounds{left, current.a.y, right, current.b.y};
            changeBounds(newBounds);
        }
        else
        {
            size.x = desiredWidth;
        }
    }

    void ClockView::clearDisplay()
    {
        if (size.x <= 0)
            return;

        displayedText_.assign(static_cast<std::size_t>(size.x), ' ');
        TDrawBuffer buffer;
        TColorAttr color = getColor(2);
        buffer.moveChar(0, ' ', color, size.x);
        writeLine(0, 0, size.x, 1, buffer);
    }

    void ClockView::handleEvent(TEvent &event)
    {
        if (event.what == evMouseDown)
        {
            bool handled = false;
            if (host_)
            {
                handled = host_->handleClockMouseClick(*this, event);
            }
            else
            {
                auto buttons = event.mouse.buttons;
#ifdef mbMiddleButton
                constexpr unsigned short kMiddleButtonMask = mbMiddleButton;
#else
                constexpr unsigned short kMiddleButtonMask = 0x04;
#endif

                if (buttons & kMiddleButtonMask)
                {
                    handled = true;
                    advanceDisplayMode();
                }
                else if (buttons & mbLeftButton)
                {
                    handled = true;
                    if (auto *window = createCalendarWindow())
                    {
                        if (TProgram::deskTop)
                        {
                            placeCalendarWindow(*TProgram::deskTop, *window);
                            TProgram::deskTop->insert(window);
                        }
                        else
                        {
                            delete window;
                        }
                    }
                }
            }
            if (handled)
                clearEvent(event);
            return;
        }
        TView::handleEvent(event);
    }

    void ClockView::advanceDisplayMode()
    {
        const ClockDisplayMode previous = mode_;
        clearDisplay();
        cycleMode();
        if (mode_ != previous)
            persistClockDisplayMode(mode_);
    }

    TRect clockBoundsFrom(const TRect &extent, short width)
    {
        TRect bounds = extent;
        const short right = extent.b.x;
        const short actualWidth = std::max<short>(1, width);
        const short left = static_cast<short>(std::max<int>(extent.a.x, right - actualWidth));
        bounds.a.x = left;
        bounds.b.x = right;
        bounds.b.y = static_cast<short>(bounds.a.y + 1);
        return bounds;
    }
} // namespace ck::ui
