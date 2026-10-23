// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "monotonic_clock.hpp"
#include <stdexcept>
#include <type_traits>

namespace ck::files {
namespace {
class SystemClock final : public MonotonicClock {
public:
    TimePoint now() const noexcept override { return std::chrono::steady_clock::now(); }
};
}
std::shared_ptr<const MonotonicClock> system_monotonic_clock() {
    static_assert(std::chrono::steady_clock::is_steady);
    static const auto clock = std::make_shared<const SystemClock>();
    return clock;
}
CooperativeTimeSlice::CooperativeTimeSlice(const MonotonicClock& clock, MonotonicClock::Duration duration,
    std::optional<MonotonicClock::TimePoint> previous)
    : clock_(clock), start_(), previous_(), duration_(duration) {
    if (duration <= MonotonicClock::Duration::zero()) throw std::invalid_argument("nonpositive time slice");
    start_ = clock_.now(); previous_ = previous.value_or(start_);
    reversed_ = start_ < previous_;
    if (!reversed_) previous_ = start_;
}
Observation<bool> CooperativeTimeSlice::expired() {
    const auto current = reversed_ ? previous_ : clock_.now();
    if (reversed_ || current < previous_) {
        reversed_ = true;
        return Observation<bool>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "clock.reversed"});
    }
    previous_ = current;
    using Rep = MonotonicClock::Duration::rep;
    static_assert(std::is_integral_v<Rep> && std::is_signed_v<Rep>);
    using Unsigned = std::make_unsigned_t<Rep>;
    const Unsigned elapsed = static_cast<Unsigned>(current.time_since_epoch().count()) -
        static_cast<Unsigned>(start_.time_since_epoch().count());
    return Observation<bool>::available(elapsed >= static_cast<Unsigned>(duration_.count()));
}
} // namespace ck::files
