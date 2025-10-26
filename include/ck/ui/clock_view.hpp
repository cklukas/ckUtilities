#pragma once

#define Uses_TEvent
#define Uses_TGroup
#define Uses_TRect
#define Uses_TView
#include <tvision/tv.h>

#include <array>
#include <cstddef>
#include <string>
#include <ctime>

namespace ck::ui
{
    class ClockAwareApplication;

    class ClockView : public TView
    {
    public:
        static constexpr short kViewWidth = 9;
        static constexpr std::size_t kTimeStringSize = 9;

        enum class DisplayMode
        {
            Time,
            Date,
            Icon
        };

        explicit ClockView(const TRect &bounds);

        void draw() override;
        void update();
        void handleEvent(TEvent &event) override;
        void setHost(ClockAwareApplication *host) noexcept { host_ = host; }
        void advanceDisplayMode();

    private:
        void refreshTime();
        std::string formatDisplay(DisplayMode mode) const;
        void cycleMode();
        void applyMode(DisplayMode mode);
        void ensureWidthFor(const std::string &text);
        void bringIntoViewBounds(short desiredWidth);
        void clearDisplay();

        std::array<char, kTimeStringSize> currentTime_{};
        std::time_t currentEpoch_{};
        DisplayMode mode_{DisplayMode::Time};
        std::string displayedText_;
        ClockAwareApplication *host_{nullptr};
    };

    TRect clockBoundsFrom(const TRect &extent, short width = ClockView::kViewWidth);
} // namespace ck::ui
