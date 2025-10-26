#include "ck/ui/clock_aware_application.hpp"

#include "ck/ui/calendar.hpp"
#include "ck/ui/clock_view.hpp"

#include <algorithm>
#include <tvision/system.h>

namespace ck::ui
{
    ClockAwareApplication::ClockAwareApplication()
        : TProgInit(&TApplication::initStatusLine,
                    &TApplication::initMenuBar,
                    &TApplication::initDeskTop),
          TApplication()
    {
    }

    void ClockAwareApplication::idle()
    {
        TApplication::idle();
        updateClocks();
        windowMenuController_.update(*this, deskTop);
    }

    ClockView *ClockAwareApplication::insertMenuClock()
    {
        auto bounds = clockBoundsFrom(getExtent());
        auto *clock = new ClockView(bounds);
        clock->growMode = gfGrowLoX | gfGrowHiX;
        clock->setHost(this);
        insert(clock);
        registerClockView(clock);
        bringClockToFront(clock);
        clock->update();
        return clock;
    }

    void ClockAwareApplication::registerClockView(ClockView *clock)
    {
        if (!clock)
            return;

        if (std::find(clockViews_.begin(), clockViews_.end(), clock) == clockViews_.end())
            clockViews_.push_back(clock);
    }

    void ClockAwareApplication::unregisterClockView(ClockView *clock)
    {
        auto it = std::remove(clockViews_.begin(), clockViews_.end(), clock);
        clockViews_.erase(it, clockViews_.end());
    }

    bool ClockAwareApplication::handleClockMouseClick(ClockView &clock, const TEvent &event)
    {
        const auto buttons = event.mouse.buttons;
#ifdef mbMiddleButton
        constexpr unsigned short kMiddleButtonMask = mbMiddleButton;
#else
        constexpr unsigned short kMiddleButtonMask = 0x04;
#endif

        if (buttons & kMiddleButtonMask)
        {
            onClockModeCycle(clock);
            return true;
        }

        if (buttons & mbLeftButton)
        {
            onClockPrimaryClick(clock);
            return true;
        }
        return false;
    }

    void ClockAwareApplication::onClockPrimaryClick(ClockView &clock)
    {
        toggleCalendarVisibility();
    }

    void ClockAwareApplication::onClockModeCycle(ClockView &clock)
    {
        clock.advanceDisplayMode();
    }

    void ClockAwareApplication::updateClocks()
    {
        for (auto *clock : clockViews_)
        {
            if (clock)
                clock->update();
        }
    }

    void ClockAwareApplication::promoteClocksToFront()
    {
        for (auto *clock : clockViews_)
            bringClockToFront(clock);
    }

    void ClockAwareApplication::bringClockToFront(ClockView *clock)
    {
        if (!clock || clock->owner != this)
            return;

        const bool wasVisible = (clock->state & sfVisible) != 0;
        remove(clock);
        insert(clock);
        if (wasVisible)
            clock->show();
        else
            clock->hide();
    }

    void ClockAwareApplication::ensureCalendarWindow()
    {
        if (calendarWindow_)
            return;

        if (!deskTop)
            return;

        calendarWindow_ = createCalendarWindow();
        if (!calendarWindow_)
            return;

        calendarWindow_->setCloseHandler([this](CalendarWindow *closed) {
            if (calendarWindow_ == closed)
                calendarWindow_ = nullptr;
        });
        repositionCalendarWindow();
        deskTop->insert(calendarWindow_);
    }

    void ClockAwareApplication::toggleCalendarVisibility()
    {
        if (calendarWindow_ && calendarWindow_->owner && (calendarWindow_->state & sfVisible) != 0)
        {
            hideCalendarWindow();
        }
        else
        {
            showCalendarWindow();
        }
    }

    void ClockAwareApplication::showCalendarWindow()
    {
        ensureCalendarWindow();
        if (!calendarWindow_)
            return;
        if (!deskTop)
            return;

        repositionCalendarWindow();
        calendarWindow_->show();
        calendarWindow_->makeFirst();
        if (deskTop)
            deskTop->setCurrent(calendarWindow_, normalSelect);
    }

    void ClockAwareApplication::hideCalendarWindow()
    {
        if (!calendarWindow_)
            return;
        calendarWindow_->hide();
    }

    void ClockAwareApplication::repositionCalendarWindow()
    {
        if (!calendarWindow_ || !deskTop)
            return;
        placeCalendarWindow(*deskTop, *calendarWindow_);
    }
} // namespace ck::ui
