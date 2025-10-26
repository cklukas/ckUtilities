#include "ck/ui/clock_aware_application.hpp"

#include "ck/ui/clock_view.hpp"

#include <algorithm>

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
    }

    ClockView *ClockAwareApplication::insertMenuClock()
    {
        auto bounds = clockBoundsFrom(getExtent());
        auto *clock = new ClockView(bounds);
        clock->growMode = gfGrowLoX | gfGrowHiX;
        insert(clock);
        registerClockView(clock);
        bringClockToFront(clock);
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
} // namespace ck::ui
