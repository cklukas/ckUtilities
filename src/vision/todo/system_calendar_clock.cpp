// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "calendar_clock.hpp"

#include <chrono>
#include <ctime>
#include <string>

namespace ck::todo {
namespace {

bool split_utc(std::time_t value, std::tm& output) noexcept {
#if defined(_WIN32)
    return ::gmtime_s(&output, &value) == 0;
#else
    return ::gmtime_r(&value, &output) != nullptr;
#endif
}

bool split_local(std::time_t value, std::tm& output) noexcept {
#if defined(_WIN32)
    return ::localtime_s(&output, &value) == 0;
#else
    return ::localtime_r(&value, &output) != nullptr;
#endif
}

void append_two(std::string& output, int value) {
    output.push_back(static_cast<char>('0' + value / 10));
    output.push_back(static_cast<char>('0' + value % 10));
}

bool append_date(std::string& output, const std::tm& fields) {
    const int year = fields.tm_year + 1900;
    if (year < 0 || year > 9999 || fields.tm_mon < 0 || fields.tm_mon > 11 ||
        fields.tm_mday < 1 || fields.tm_mday > 31) {
        return false;
    }
    output.push_back(static_cast<char>('0' + year / 1000));
    output.push_back(static_cast<char>('0' + (year / 100) % 10));
    output.push_back(static_cast<char>('0' + (year / 10) % 10));
    output.push_back(static_cast<char>('0' + year % 10));
    output.push_back('-');
    append_two(output, fields.tm_mon + 1);
    output.push_back('-');
    append_two(output, fields.tm_mday);
    return true;
}

}  // namespace

CalendarReadResult SystemCalendarClock::read() const {
    const auto now = std::chrono::system_clock::now();
    const std::time_t value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    std::tm local{};
    if (!split_utc(value, utc) || !split_local(value, local)) {
        return CalendarReadResult::failure("system calendar conversion failed");
    }

    CalendarReading reading;
    reading.utc_timestamp.value.reserve(20);
    if (!append_date(reading.utc_timestamp.value, utc) || utc.tm_hour < 0 || utc.tm_hour > 23 ||
        utc.tm_min < 0 || utc.tm_min > 59 || utc.tm_sec < 0 || utc.tm_sec > 59) {
        return CalendarReadResult::failure("system UTC calendar value is outside supported range");
    }
    reading.utc_timestamp.value.push_back('T');
    append_two(reading.utc_timestamp.value, utc.tm_hour);
    reading.utc_timestamp.value.push_back(':');
    append_two(reading.utc_timestamp.value, utc.tm_min);
    reading.utc_timestamp.value.push_back(':');
    append_two(reading.utc_timestamp.value, utc.tm_sec);
    reading.utc_timestamp.value.push_back('Z');

    reading.local_date.value.reserve(10);
    if (!append_date(reading.local_date.value, local)) {
        return CalendarReadResult::failure("system local calendar value is outside supported range");
    }
    reading.local_time.value.reserve(5);
    if (local.tm_hour < 0 || local.tm_hour > 23 || local.tm_min < 0 || local.tm_min > 59) {
        return CalendarReadResult::failure("system local clock value is outside supported range");
    }
    append_two(reading.local_time.value, local.tm_hour);
    reading.local_time.value.push_back(':');
    append_two(reading.local_time.value, local.tm_min);
    if (!is_valid(reading.utc_timestamp) || !is_valid(reading.local_date) || !is_valid(reading.local_time)) {
        return CalendarReadResult::failure("system calendar produced an invalid reading");
    }
    return CalendarReadResult::success(std::move(reading));
}

}  // namespace ck::todo
