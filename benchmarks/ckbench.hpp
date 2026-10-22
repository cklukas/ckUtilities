// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ckbench — ckVision's minimal, dependency-free benchmark harness.
// Note: benchmarks are tooling and may read the steady clock; library
// code may not (the architecture §10).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>

namespace ckbench {

// What a benchmark run is for. Timed runs each benchmark its full iteration count and
// reports the time it took. Gate runs each one only kGateIterations times: a CTest budget
// gate asserts deterministic work (cells touched, bytes written, repaints), which every
// iteration checks, not speed, so it must not take a timing run's time under Debug or a
// sanitizer.
enum class Mode { Timed, Gate };

// Enough iterations for a benchmark whose body alternates between states to visit each
// of them more than once.
inline constexpr int kGateIterations = 4;

// Gate when the command line is exactly `--gate`, Timed when it is empty. Anything else
// is a usage error, reported as nullopt.
inline std::optional<Mode> mode_from_arguments(int argc, char** argv) noexcept {
    if (argc == 1) return Mode::Timed;
    if (argc == 2 && std::strcmp(argv[1], "--gate") == 0) return Mode::Gate;
    return std::nullopt;
}

// Runs benchmark bodies in one Mode and prints one line per benchmark.
class Runner {
public:
    explicit Runner(Mode mode) noexcept : mode_(mode) {}

    // Calls `fn` `iterations` times (at most kGateIterations in Gate mode) and prints
    // the label, the count and the time per call.
    template <typename F>
    void run(const char* label, int iterations, F&& fn) const {
        using clock = std::chrono::steady_clock;
        const int count = mode_ == Mode::Gate ? std::min(iterations, kGateIterations) : iterations;
        const auto start = clock::now();
        for (int i = 0; i < count; ++i) fn();
        const auto stop = clock::now();
        const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count();
        const double per_op = count > 0 ? static_cast<double>(ns) / count : 0.0;
        std::printf("%-32s %8d iters %12lld ns total %12.1f ns/op\n", label, count, ns, per_op);
    }

private:
    Mode mode_;
};

}  // namespace ckbench
