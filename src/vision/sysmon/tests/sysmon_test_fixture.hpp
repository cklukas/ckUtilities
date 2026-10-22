// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sysmon_app.hpp"
#include "cvision/term/headless_terminal.hpp"
#include "cvision/core/golden.hpp"
#include "cvision/scene/golden_capture.hpp"
#include <memory>

namespace ck::testing {
inline ck::sysmon::Sample monitor_sample() {
    ck::sysmon::Sample sample;
    sample.reachable = true; sample.system = "Linux / x86_64 / 12 threads";
    sample.cpu_load = 16.3;
    sample.memory_used = 21474836480ULL; sample.memory_total = 68719476736ULL;
    sample.gpus = {{"gpu-a", "NVIDIA GTX 1080 Ti", 47.0, 6442450944ULL, 11811160064ULL, 858993459ULL, 73.0, 187.0},
                   {"gpu-b", "NVIDIA GTX 1080 Ti", 46.0, 6335076762ULL, 11811160064ULL, 107374182ULL, 73.0, 158.0}};
    sample.ollama_status = "running"; sample.models = {{"apertus:8b-instruct-2509-q4_k_m", 10093173146ULL}};
    return sample;
}
inline ck::sysmon::Sample apple_monitor_sample() {
    auto sample = monitor_sample();
    sample.system = "Darwin / arm64 / 10 threads";
    sample.memory_used = 20ULL << 30; sample.memory_total = 32ULL << 30;
    sample.gpus = {{"metal:fixture", "Apple M1 Max", 47.0, 8ULL << 30, 32ULL << 30, {}, {}, {}, ck::sysmon::GpuMemoryKind::UnifiedDriver}};
    return sample;
}
inline ck::sysmon::Sample v3d_monitor_sample() {
    auto sample = monitor_sample();
    sample.system = "Linux / aarch64 / 4 threads";
    sample.memory_used = 1ULL << 30; sample.memory_total = 4ULL << 30;
    sample.gpus = {{"v3d:fixture", "V3D card1", {}, 119750656ULL, {}, {}, {}, {}, ck::sysmon::GpuMemoryKind::V3dBuffers,
        {{"bin", 1000, 100}, {"render", 1000, 200}}}};
    sample.ollama_status = "not running"; sample.models.clear();
    return sample;
}
class FixedMonitorSource : public ck::sysmon::MonitorSource {
public:
    std::vector<std::string> hosts() override { return {"render-node", "offline-node"}; }
    ck::sysmon::Sample sample(const std::string& endpoint) override {
        if (endpoint == "offline-node") { ck::sysmon::Sample result; result.diagnostic = "Passwordless SSH unavailable"; return result; }
        return monitor_sample();
    }
};
struct SysmonFixture {
    ckv::term::HeadlessTerminal terminal;
    ckv::ManualClock clock;
    ckv::ui::Application app;
    FixedMonitorSource source;
    std::unique_ptr<ck::sysmon::SysmonApp> monitor;
    explicit SysmonFixture(ckv::Size size = {120, 34}, ck::sysmon::SettingsBinding settings = {}) : terminal(size), app(terminal, clock) {
        monitor = std::make_unique<ck::sysmon::SysmonApp>(app, source, ck::sysmon::CalendarProviders{
            [] { return ckv::widgets::TimeValue{18, 40, 12}; }, [] { return ckv::widgets::DateValue{2026, 10, 21}; }}, std::move(settings));
        monitor->wait_idle(); app.step(0);
    }
    bool command(std::string_view key) {
        const auto id = app.commands().id_for("sysmon." + std::string(key));
        return id && app.execute_command(*id);
    }
    std::string frame() {
        app.step(0);
        return ckv::golden::serialize(ckv::scene::capture(app.composed_surface(), app.current_cursor()));
    }
};
} // namespace ck::testing
