#include "ck/ui/clock_settings.hpp"

#include "ck/options.hpp"

#include <string>

namespace config = ck::config;

namespace ck::ui
{
    namespace
    {
        constexpr std::string_view kClockAppId = "ck-ui";
        constexpr std::string_view kDisplayModeKey = "clock.display-mode";
        constexpr ClockDisplayMode kDefaultDisplayMode = ClockDisplayMode::Time;

        config::OptionDefinition clockDisplayModeDefinition()
        {
            return config::OptionDefinition{
                std::string(kDisplayModeKey),
                config::OptionKind::String,
                config::OptionValue(std::string(clockDisplayModeToString(kDefaultDisplayMode))),
                "Clock Display Mode",
                "Controls how the menu bar clock displays time and date information."};
        }

        config::OptionRegistry &clockRegistry()
        {
            static config::OptionRegistry registry{std::string(kClockAppId)};
            static bool initialized = false;
            if (!initialized)
            {
                registry.registerOption(clockDisplayModeDefinition());
                registry.loadDefaults();
                initialized = true;
            }
            return registry;
        }
    } // namespace

    void registerClockOptions(config::OptionRegistry &registry)
    {
        registry.registerOption(clockDisplayModeDefinition());
    }

    ClockDisplayMode clockDisplayModeFromString(std::string_view value, ClockDisplayMode fallback)
    {
        if (value == "time")
            return ClockDisplayMode::Time;
        if (value == "date")
            return ClockDisplayMode::Date;
        if (value == "icon")
            return ClockDisplayMode::Icon;
        return fallback;
    }

    std::string_view clockDisplayModeToString(ClockDisplayMode mode) noexcept
    {
        switch (mode)
        {
        case ClockDisplayMode::Time:
            return "time";
        case ClockDisplayMode::Date:
            return "date";
        case ClockDisplayMode::Icon:
            return "icon";
        }
        return "time";
    }

    ClockDisplayMode loadClockDisplayMode()
    {
        auto &registry = clockRegistry();
        std::string stored = registry.getString(std::string(kDisplayModeKey),
                                               std::string(clockDisplayModeToString(kDefaultDisplayMode)));
        return clockDisplayModeFromString(stored, kDefaultDisplayMode);
    }

    void persistClockDisplayMode(ClockDisplayMode mode)
    {
        auto &registry = clockRegistry();
        std::string value{clockDisplayModeToString(mode)};
        registry.set(std::string(kDisplayModeKey), config::OptionValue(value));
        registry.saveDefaults();
    }

} // namespace ck::ui
