#pragma once

#include <string_view>

namespace ck::config
{
    class OptionRegistry;
}

namespace ck::ui
{

    enum class ClockDisplayMode
    {
        Time,
        Date,
        Icon
    };

    void registerClockOptions(config::OptionRegistry &registry);

    ClockDisplayMode loadClockDisplayMode();
    void persistClockDisplayMode(ClockDisplayMode mode);

    ClockDisplayMode clockDisplayModeFromString(std::string_view value, ClockDisplayMode fallback);
    std::string_view clockDisplayModeToString(ClockDisplayMode mode) noexcept;

} // namespace ck::ui

