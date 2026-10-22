// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <utility>

#include "todo_model.hpp"

namespace ck::todo {

struct CalendarReading {
    IsoTimestamp utc_timestamp;
    IsoDate local_date;
    IsoTime local_time;
    friend bool operator==(const CalendarReading&, const CalendarReading&) = default;
};

struct CalendarReadResult {
    std::optional<CalendarReading> value;
    std::string diagnostic;

    explicit operator bool() const noexcept { return value.has_value(); }

    static CalendarReadResult success(CalendarReading reading) {
        CalendarReadResult result;
        result.value.emplace(std::move(reading));
        return result;
    }

    static CalendarReadResult failure(std::string message) {
        CalendarReadResult result;
        result.diagnostic = std::move(message);
        return result;
    }
};

class CalendarClock {
public:
    virtual ~CalendarClock() = default;
    virtual CalendarReadResult read() const = 0;
};

class FixedCalendarClock final : public CalendarClock {
public:
    explicit FixedCalendarClock(CalendarReading reading) : reading_(std::move(reading)) {}

    CalendarReadResult read() const override {
        if (failure_) return CalendarReadResult::failure(*failure_);
        return CalendarReadResult::success(reading_);
    }

    void set(CalendarReading reading) {
        reading_ = std::move(reading);
        failure_.reset();
    }

    void fail(std::string diagnostic) { failure_ = std::move(diagnostic); }

private:
    CalendarReading reading_;
    std::optional<std::string> failure_;
};

class SystemCalendarClock final : public CalendarClock {
public:
    CalendarReadResult read() const override;
};

}  // namespace ck::todo
