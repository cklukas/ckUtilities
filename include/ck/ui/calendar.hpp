#pragma once

#define Uses_TDeskTop
#define Uses_TEvent
#define Uses_TKeys
#define Uses_TRect
#define Uses_TView
#define Uses_TWindow
#include <tvision/tv.h>

#include <functional>

namespace ck::ui
{
    class CalendarWindow : public TWindow
    {
    public:
        CalendarWindow();

        using CloseHandler = std::function<void(CalendarWindow *)>;
        void setCloseHandler(CloseHandler handler);

        bool bringToTop() noexcept;

    protected:
        void shutDown() override;

    private:
        class CalendarView;

        CloseHandler closeHandler_;
    };

    CalendarWindow *createCalendarWindow();

    void placeCalendarWindow(TDeskTop &deskTop, CalendarWindow &window);
} // namespace ck::ui
