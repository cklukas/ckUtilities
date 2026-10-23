// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "observation.hpp"
#include <chrono>
#include <memory>
#include <optional>

namespace ck::files {
class MonotonicClock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;
    using Duration = std::chrono::steady_clock::duration;
    virtual ~MonotonicClock() = default;
    virtual TimePoint now() const = 0;
};
std::shared_ptr<const MonotonicClock> system_monotonic_clock();
// Checked elapsed comparison without signed subtraction or deadline addition.
// Arbitrary (including negative) clock epochs are valid; backwards readings
// fail explicitly and permanently. This cannot interrupt an OS call.
class CooperativeTimeSlice final {
public:
    CooperativeTimeSlice(const MonotonicClock& clock, MonotonicClock::Duration duration,
        std::optional<MonotonicClock::TimePoint> previous = {});
    Observation<bool> expired();
    MonotonicClock::TimePoint last_reading() const noexcept { return previous_; }
private:
    const MonotonicClock& clock_; // must outlive this slice
    MonotonicClock::TimePoint start_, previous_;
    MonotonicClock::Duration duration_;
    bool reversed_ = false;
};
} // namespace ck::files
