#pragma once

#define Uses_TRect
#define Uses_TView
#include <tvision/tv.h>

#include <array>
#include <cstddef>

namespace ck::ui
{
    class ClockView : public TView
    {
    public:
        static constexpr short kViewWidth = 9;
        static constexpr std::size_t kTimeStringSize = 9;

        explicit ClockView(const TRect &bounds);

        void draw() override;
        void update();

    private:
        void refreshTime();

        std::array<char, kTimeStringSize> lastTime_{};
        std::array<char, kTimeStringSize> currentTime_{};
    };

    TRect clockBoundsFrom(const TRect &extent);
} // namespace ck::ui
