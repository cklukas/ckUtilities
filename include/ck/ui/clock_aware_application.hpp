#pragma once

#define Uses_TApplication
#define Uses_TDeskTop
#define Uses_TEvent
#define Uses_TRect
#include <tvision/tv.h>

#include <vector>

namespace ck::ui
{
    class ClockView;
    class CalendarWindow;

    class ClockAwareApplication : public TApplication
    {
    public:
        ClockAwareApplication();
        ~ClockAwareApplication() override = default;

        friend class ClockView;

    protected:
        void idle() override;

        ClockView *insertMenuClock();
        void registerClockView(ClockView *clock);
        void unregisterClockView(ClockView *clock);
        void promoteClocksToFront();
        bool handleClockMouseClick(ClockView &clock, const TEvent &event);
        virtual void onClockPrimaryClick(ClockView &clock, bool withShift);

    private:
        void updateClocks();
        void bringClockToFront(ClockView *clock);
        void ensureCalendarWindow();
        void toggleCalendarVisibility();
        void showCalendarWindow();
        void hideCalendarWindow();
        void repositionCalendarWindow();

        std::vector<ClockView *> clockViews_;
        CalendarWindow *calendarWindow_ = nullptr;
    };
} // namespace ck::ui
