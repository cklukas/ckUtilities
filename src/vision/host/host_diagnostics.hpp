// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The examples' two diagnostic switches, read from their own environment. The library reads
// no environment variable to decide what to do (D-077); an application that wants a trace or
// an output capture creates the sinks itself and hands them over, and this is how the
// examples do it:
//
//   CKVISION_GRAPHICS_LOG=<file>|stderr|-
//       a live trace of the graphics path: what a child asked about graphics and was told,
//       which pictures decoded, each encode and frame with its timing, and why a picture
//       did not reach the terminal. The first example process to open the file starts it
//       empty; a contained child it launches appends, so host and child read as one log.
//   CKVISION_OUTPUT_CAPTURE=<file>
//       every byte written to the terminal (native backends), so a frame a host renders
//       wrongly can be replayed into ckVision's own decoder.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "cvision/core/clock.hpp"
#include "cvision/core/diagnostics.hpp"
#include "cvision/term/file_trace_sink.hpp"
#include "cvision/ui/application.hpp"
#include "native_host.hpp"

namespace ck::host {

class HostDiagnostics {
public:
    // Reads both switches and opens what they name. `clock` stamps trace lines and must
    // outlive this object; so must this object outlive every Application and terminal it
    // is attached to.
    explicit HostDiagnostics(const ckv::Clock& clock) {
        if (const std::string_view trace = variable("CKVISION_GRAPHICS_LOG"); !trace.empty()) {
            if (trace == "-" || trace == "stderr") {
                trace_sink_ = ckv::term::FileTraceSink::standard_error(clock);
            } else {
                // A contained child inherits both variables. The marker says a host has
                // already started this file, so the child appends to it rather than erasing
                // the host's lines.
                const bool host_started_it = !variable(kOwnerMarker).empty();
                trace_sink_ = ckv::term::FileTraceSink::open(
                    std::string(trace),
                    host_started_it ? ckv::term::FileTraceSink::OpenMode::Append : ckv::term::FileTraceSink::OpenMode::Truncate,
                    clock);
                if (trace_sink_ != nullptr && !host_started_it) mark_owned();
            }
        }
        if (const std::string_view capture = variable("CKVISION_OUTPUT_CAPTURE"); !capture.empty())
            capture_.reset(std::fopen(std::string(capture).c_str(), "wb"));
        clock_ = &clock;
    }

    // The trace to hand over, or an empty one when tracing is off.
    ckv::GraphicsTrace trace() const noexcept { return ckv::GraphicsTrace{trace_sink_.get(), clock_}; }

    // Gives `app` the trace, which it passes on to every terminal session it owns.
    void attach(ckv::ui::Application& app) const noexcept { app.set_graphics_trace(trace()); }

    // Gives the backend the trace and the output capture.
    void attach(NativeTerminal& terminal) const {
        terminal.set_graphics_trace(trace());
        if (capture_ != nullptr) {
            std::FILE* const stream = capture_.get();
            terminal.set_output_capture([stream](std::string_view bytes) {
                std::fwrite(bytes.data(), 1, bytes.size(), stream);
                std::fflush(stream);
            });
        }
    }

private:
    static constexpr const char* kOwnerMarker = "CKVISION_GRAPHICS_LOG_OWNED";

    static std::string_view variable(const char* name) noexcept {
        const char* const value = std::getenv(name);
        return value != nullptr ? std::string_view(value) : std::string_view();
    }

    static void mark_owned() noexcept {
#if defined(_WIN32)
        (void)::_putenv_s(kOwnerMarker, "1");
#else
        (void)::setenv(kOwnerMarker, "1", 1);
#endif
    }

    struct CloseFile {
        void operator()(std::FILE* stream) const noexcept { std::fclose(stream); }
    };

    std::unique_ptr<ckv::term::FileTraceSink> trace_sink_;
    std::unique_ptr<std::FILE, CloseFile> capture_;
    const ckv::Clock* clock_ = nullptr;
};

// Runs an example and reports an environmental failure (D-078): a terminal that refuses the
// session or goes away throws, the objects `run` built are destroyed first — restoring the
// terminal — and only then is the message written, so it lands on a usable screen. `run`
// either returns nothing (success) or the exit status itself.
template <class Run>
int run_reporting_failure(Run&& run) {
    try {
        if constexpr (std::is_void_v<std::invoke_result_t<Run&>>) {
            run();
            return EXIT_SUCCESS;
        } else {
            return run();
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return EXIT_FAILURE;
    }
}

}  // namespace ck::host
