#include "ck/ui/clock_view.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

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
        lastTime_.fill('\0');
        currentTime_.fill('\0');
        refreshTime();
        lastTime_ = currentTime_;
    }

    void ClockView::draw()
    {
        TDrawBuffer buffer;
        TColorAttr color = getColor(2);

        buffer.moveChar(0, ' ', color, size.x);

        const short textLength = static_cast<short>(std::strlen(currentTime_.data()));
        const short startColumn = std::max<short>(0, static_cast<short>(size.x - textLength));
        buffer.moveStr(startColumn, currentTime_.data(), color);

        writeLine(0, 0, size.x, 1, buffer);
    }

    void ClockView::update()
    {
        refreshTime();
        if (std::strncmp(lastTime_.data(), currentTime_.data(), kTimeStringSize) != 0)
        {
            drawView();
            lastTime_ = currentTime_;
        }
    }

    void ClockView::refreshTime()
    {
        const auto now = std::chrono::system_clock::now();
        const auto currentTime = std::chrono::system_clock::to_time_t(now);
        const std::tm local = localTime(currentTime);

        std::strftime(currentTime_.data(), currentTime_.size(), "%H:%M:%S", &local);
    }

    TRect clockBoundsFrom(const TRect &extent)
    {
        TRect bounds = extent;
        const short right = extent.b.x;
        const short left = static_cast<short>(std::max<int>(extent.a.x, right - ClockView::kViewWidth));
        bounds.a.x = left;
        bounds.b.x = right;
        bounds.b.y = static_cast<short>(bounds.a.y + 1);
        return bounds;
    }
} // namespace ck::ui

