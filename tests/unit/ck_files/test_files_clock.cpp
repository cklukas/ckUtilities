// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "monotonic_clock.hpp"
#include "manual_monotonic_clock.hpp"
#include "cvision/testing/cktest.hpp"
#include <limits>
#include <stdexcept>
namespace {
using namespace ck::files;
using Duration = MonotonicClock::Duration;
using Point = MonotonicClock::TimePoint;
using Rep = Duration::rep;
bool expired(CooperativeTimeSlice& slice) {
    const auto value = slice.expired(); if (!value.value()) throw std::runtime_error("expected available clock result"); return *value.value();
}
}
CK_TEST(time_slice_compares_exact_boundaries_with_negative_epochs_and_extreme_elapsed_ranges) {
    ck::testing::ManualMonotonicClock clock;
    clock.value = Point(Duration(std::numeric_limits<Rep>::min())); CooperativeTimeSlice slice(clock, Duration(10));
    clock.value = Point(Duration(std::numeric_limits<Rep>::min() + 9)); CK_CHECK(!expired(slice));
    clock.value = Point(Duration(std::numeric_limits<Rep>::min() + 10)); CK_CHECK(expired(slice));
    clock.value = Point(Duration(std::numeric_limits<Rep>::min())); CooperativeTimeSlice wide(clock, Duration::max());
    clock.value = Point(Duration(std::numeric_limits<Rep>::max())); CK_CHECK(expired(wide));
    clock.value = Point(Duration(-5)); CooperativeTimeSlice crossing(clock, Duration(10));
    clock.value = Point(Duration(4)); CK_CHECK(!expired(crossing)); clock.value = Point(Duration(5)); CK_CHECK(expired(crossing));
}
CK_TEST(time_slice_clock_reversal_is_sticky_and_prior_slice_readings_are_checked) {
    ck::testing::ManualMonotonicClock clock; clock.value = Point(Duration(10)); CooperativeTimeSlice slice(clock, Duration(5));
    clock.value = Point(Duration(9)); CK_CHECK(slice.expired().state() == ObservationState::Failed);
    const int reads = clock.reads; clock.value = Point(Duration(20)); CK_CHECK(slice.expired().state() == ObservationState::Failed && clock.reads == reads);
    clock.value = Point(Duration(8)); CooperativeTimeSlice next(clock, Duration(5), Point(Duration(10)));
    CK_CHECK(next.expired().state() == ObservationState::Failed && next.last_reading() == Point(Duration(10)));
}
CK_TEST(time_slice_rejects_nonpositive_budgets_before_clock_use_and_preserves_clock_exceptions) {
    ck::testing::ManualMonotonicClock clock;
    for (auto duration : {Duration::zero(), Duration(-1)}) {
        bool rejected = false; try { CooperativeTimeSlice slice(clock, duration); } catch (const std::invalid_argument&) { rejected = true; } CK_CHECK(rejected);
    }
    CK_CHECK(clock.reads == 0); clock.throw_on_read = true; bool caught = false;
    try { CooperativeTimeSlice slice(clock, Duration(1)); } catch (const std::runtime_error&) { caught = true; } CK_CHECK(caught);
}
CK_TEST(system_monotonic_clock_is_owned_and_nondecreasing_without_wall_time_or_sleep) {
    const auto clock = system_monotonic_clock(); CK_CHECK(clock && clock == system_monotonic_clock());
    auto previous = clock->now();
    for (int index = 0; index < 64; ++index) { const auto current = clock->now(); CK_CHECK(current >= previous); previous = current; }
}
