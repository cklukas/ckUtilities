#include "ck/ui/calendar.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

namespace ck::ui
{
    namespace
    {
        constexpr std::array<const char *, 13> kMonthNames = {
            "",
            "January",
            "February",
            "March",
            "April",
            "May",
            "June",
            "July",
            "August",
            "September",
            "October",
            "November",
            "December",
        };

        constexpr std::array<unsigned, 13> kMonthLengths = {
            0,
            31,
            28,
            31,
            30,
            31,
            30,
            31,
            31,
            30,
            31,
            30,
            31,
        };

        bool isLeapYear(int year)
        {
            if (year % 400 == 0)
                return true;
            if (year % 100 == 0)
                return false;
            return year % 4 == 0;
        }

        unsigned daysInMonth(int year, unsigned month)
        {
            if (month == 0 || month >= kMonthLengths.size())
                return 30;
            unsigned days = kMonthLengths[month];
            if (month == 2 && isLeapYear(year))
                ++days;
            return days;
        }

        int calendarDayOfWeek(int day, unsigned month, int year)
        {
            int m = static_cast<int>(month);
            int y = year;
            if (m < 3)
            {
                m += 12;
                --y;
            }
            int K = y % 100;
            int J = y / 100;
            int h = (day + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
            int dayOfWeek = ((h + 6) % 7);
            return dayOfWeek;
        }

    } // namespace

    class CalendarWindow::CalendarView : public TView
    {
    public:
        explicit CalendarView(const TRect &bounds)
            : TView(bounds)
        {
            options |= ofSelectable;
            eventMask |= evMouseAuto | evMouseDown | evKeyboard;

            auto now = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
            std::chrono::year_month_day ymd(now);
            year = static_cast<int>(ymd.year());
            month = static_cast<unsigned>(ymd.month());
            currentYear = year;
            currentMonth = month;
            currentDay = static_cast<unsigned>(ymd.day());
        }

        void draw() override
        {
            TDrawBuffer buf;
            auto normal = getColor(6);
            auto highlight = getColor(7);

            buf.moveChar(0, ' ', normal, size.x);
            std::ostringstream header;
            header << std::setw(9) << kMonthNames[std::min<std::size_t>(month, kMonthNames.size() - 1)]
                   << ' ' << std::setw(4) << year
                   << ' ' << static_cast<char>(30) << "  " << static_cast<char>(31);
            buf.moveStr(0, header.str().c_str(), normal);
            writeLine(0, 0, size.x, 1, buf);

            buf.moveChar(0, ' ', normal, size.x);
            buf.moveStr(0, "Su Mo Tu We Th Fr Sa", normal);
            writeLine(0, 1, size.x, 1, buf);

            int firstWeekday = calendarDayOfWeek(1, month, year);
            int current = 1 - firstWeekday;
            int totalDays = static_cast<int>(daysInMonth(year, month));
            for (int row = 0; row < 6; ++row)
            {
                buf.moveChar(0, ' ', normal, size.x);
                for (int col = 0; col < 7; ++col)
                {
                    if (current < 1 || current > totalDays)
                    {
                        buf.moveStr(col * 3, "   ", normal);
                    }
                    else
                    {
                        std::ostringstream cell;
                        cell << std::setw(2) << current;
                        bool isToday = (year == currentYear && month == currentMonth && current == static_cast<int>(currentDay));
                        buf.moveStr(col * 3, cell.str().c_str(), isToday ? highlight : normal);
                    }
                    ++current;
                }
                writeLine(0, static_cast<short>(row + 2), size.x, 1, buf);
            }
        }

        void handleEvent(TEvent &event) override
        {
            TView::handleEvent(event);
            if (event.what == evKeyboard)
            {
                bool handled = false;
                switch (event.keyDown.keyCode)
                {
                case kbLeft:
                    changeMonth(-1);
                    handled = true;
                    break;
                case kbRight:
                    changeMonth(1);
                    handled = true;
                    break;
                case kbUp:
                case kbPgUp:
                    changeMonth(-12);
                    handled = true;
                    break;
                case kbDown:
                case kbPgDn:
                    changeMonth(12);
                    handled = true;
                    break;
                case kbHome:
                    year = currentYear;
                    month = currentMonth;
                    handled = true;
                    break;
                default:
                    break;
                }
                if (handled)
                {
                    drawView();
                    clearEvent(event);
                }
            }
            else if (event.what == evMouseDown || event.what == evMouseAuto)
            {
                TPoint point = makeLocal(event.mouse.where);
                if (point.y == 0)
                {
                    if (point.x == 15)
                        changeMonth(1);
                    else if (point.x == 18)
                        changeMonth(-1);
                    drawView();
                }
                clearEvent(event);
            }
        }

    private:
        int year = 0;
        unsigned month = 1;
        unsigned currentDay = 1;
        int currentYear = 0;
        unsigned currentMonth = 1;

        void changeMonth(int delta)
        {
            int totalMonths = static_cast<int>(month) + delta;
            int newYear = year + (totalMonths - 1) / 12;
            int newMonth = (totalMonths - 1) % 12 + 1;
            if (newMonth <= 0)
            {
                newMonth += 12;
                --newYear;
            }
            year = newYear;
            month = static_cast<unsigned>(newMonth);
        }
    };
    CalendarWindow::CalendarWindow()
        : TWindowInit(&CalendarWindow::initFrame),
          TWindow(TRect(0, 0, 24, 10), "Calendar", wnNoNumber)
    {
        flags &= ~(wfGrow | wfZoom);
        growMode = 0;
        palette = wpGrayWindow;

        TRect inner = getExtent();
        inner.grow(-1, -1);
        insert(new CalendarView(inner));
    }

    void CalendarWindow::setCloseHandler(CloseHandler handler)
    {
        if (!handler)
            return;

        if (closeHandler_)
        {
            auto previous = std::move(closeHandler_);
            closeHandler_ = [prev = std::move(previous), current = std::move(handler)](CalendarWindow *self) {
                if (current)
                    current(self);
                if (prev)
                    prev(self);
            };
        }
        else
        {
            closeHandler_ = std::move(handler);
        }
    }

    bool CalendarWindow::bringToTop() noexcept
    {
        if (!owner)
            return false;
        owner->remove(this);
        owner->insert(this);
        show();
        return true;
    }

    void CalendarWindow::shutDown()
    {
        if (closeHandler_)
            closeHandler_(this);
        TWindow::shutDown();
    }

    CalendarWindow *createCalendarWindow()
    {
        return new CalendarWindow();
    }

    void placeCalendarWindow(TDeskTop &deskTop, CalendarWindow &window)
    {
        const TRect desktopBounds = deskTop.getExtent();
        TRect bounds = window.getBounds();
        const short width = bounds.b.x - bounds.a.x;
        const short height = bounds.b.y - bounds.a.y;

        const short right = desktopBounds.b.x;
        const short left = static_cast<short>(std::max<int>(desktopBounds.a.x, right - width));
        const short topLimit = static_cast<short>(desktopBounds.a.y);

        short top = topLimit;
        short bottom = static_cast<short>(top + height);
        if (bottom > desktopBounds.b.y)
        {
            bottom = desktopBounds.b.y;
            top = static_cast<short>(std::max<int>(desktopBounds.a.y, bottom - height));
        }

        bounds.a.x = left;
        bounds.b.x = static_cast<short>(left + width);
        bounds.a.y = top;
        bounds.b.y = bottom;

        if (window.owner != nullptr)
            window.locate(bounds);
        else
            window.setBounds(bounds);
    }
} // namespace ck::ui
