// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "monotonic_clock.hpp"
#include <stdexcept>

namespace ck::testing {
// Deterministic test-only clock. Tests control advances within representable
// bounds; no sleeping, wall time or mutation of the native system clock.
class ManualMonotonicClock final : public ck::files::MonotonicClock {
public:
    mutable TimePoint value{};
    Duration per_read{};
    mutable int reads = 0;
    bool throw_on_read = false;
    TimePoint now() const override {
        ++reads;
        if (throw_on_read) throw std::runtime_error("injected clock exception");
        const auto result = value; value += per_read; return result;
    }
    void advance(Duration duration) { value += duration; }
};
} // namespace ck::testing
