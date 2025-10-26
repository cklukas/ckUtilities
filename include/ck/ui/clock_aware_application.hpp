#pragma once

#define Uses_TApplication
#define Uses_TRect
#include <tvision/tv.h>

#include <vector>

namespace ck::ui
{
    class ClockView;

    class ClockAwareApplication : public TApplication
    {
    public:
        ClockAwareApplication();
        ~ClockAwareApplication() override = default;

    protected:
        void idle() override;

        ClockView *insertMenuClock();
        void registerClockView(ClockView *clock);
        void unregisterClockView(ClockView *clock);

    private:
        void updateClocks();

        std::vector<ClockView *> clockViews_;
    };
} // namespace ck::ui
