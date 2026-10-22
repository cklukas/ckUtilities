// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "calendar_clock.hpp"

#include "cvision/testing/cktest.hpp"

namespace {

using namespace ck::todo;

CalendarReading fixed_reading() {
    return {IsoTimestamp{"2026-08-25T12:34:56Z"}, IsoDate{"2026-08-25"}, IsoTime{"14:34"}};
}

}  // namespace

CK_TEST(todo_fixed_calendar_clock_is_deterministic_and_replaceable) {
    FixedCalendarClock clock(fixed_reading());
    CK_CHECK(clock.read().value == fixed_reading());
    const CalendarReading later{
        IsoTimestamp{"2026-08-26T00:02:03Z"}, IsoDate{"2026-08-26"}, IsoTime{"02:02"}};
    clock.set(later);
    CK_CHECK(clock.read().value == later);
}

CK_TEST(todo_fixed_calendar_clock_can_script_a_read_failure) {
    FixedCalendarClock clock(fixed_reading());
    clock.fail("clock unavailable");
    const auto failed = clock.read();
    CK_CHECK(!failed);
    CK_CHECK(failed.diagnostic == "clock unavailable");
    clock.set(fixed_reading());
    CK_CHECK(clock.read());
}

CK_TEST(todo_system_calendar_clock_returns_canonical_values) {
    SystemCalendarClock clock;
    const auto reading = clock.read();
    CK_CHECK(reading);
    if (!reading) return;
    CK_CHECK(is_valid(reading.value->utc_timestamp));
    CK_CHECK(is_valid(reading.value->local_date));
    CK_CHECK(is_valid(reading.value->local_time));
}
