// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ck::sysmon {

struct CpuTicks {
    std::uint64_t total = 0;
    std::uint64_t idle = 0;
};
enum class GpuMemoryKind { Dedicated, UnifiedDriver, V3dBuffers };
struct GpuEngineTicks {
    std::string name;
    std::uint64_t timestamp = 0, runtime = 0;
};
std::optional<std::vector<GpuEngineTicks>> parse_v3d_activity(std::string_view text);
std::optional<std::uint64_t> parse_v3d_buffers(std::string_view text);
struct GpuSample {
    std::string id;
    std::string name;
    std::optional<double> load;
    std::optional<std::uint64_t> used, total, shared;
    std::optional<double> temperature, watts;
    GpuMemoryKind memory_kind = GpuMemoryKind::Dedicated;
    std::vector<GpuEngineTicks> engines{};
};
struct ModelInfo {
    std::string name;
    std::optional<std::uint64_t> vram;
};
struct Sample {
    bool reachable = false;
    std::string system;
    std::string diagnostic;
    std::optional<CpuTicks> cpu;
    std::optional<double> cpu_load;
    std::optional<std::uint64_t> memory_used, memory_total;
    std::vector<GpuSample> gpus;
    std::string ollama_status = "not running";
    std::vector<ModelInfo> models;
};

// The empty endpoint identifies the local host. Implementations may be called
// concurrently for different endpoints, never concurrently for the same one.
class MonitorSource {
public:
    virtual ~MonitorSource() = default;
    virtual std::vector<std::string> hosts() = 0;
    virtual Sample sample(const std::string& endpoint) = 0;
};

using History = std::deque<std::optional<double>>;
struct GpuHistory {
    std::string id;
    History load, memory;
};
struct HostState {
    std::string endpoint;
    bool selected = false;
    bool want_selected = true;
    bool checked_once = false;
    Sample latest;
    std::optional<CpuTicks> previous_cpu;
    History cpu_history, memory_history;
    std::vector<GpuHistory> gpu_history;
    void accept(Sample sample);
};

std::optional<double> percent(std::optional<std::uint64_t> used, std::optional<std::uint64_t> total);
std::optional<double> cpu_percent(CpuTicks before, CpuTicks after);
std::string sample_text(const Sample& sample);
std::optional<Sample> parse_sample(std::string_view text);
std::string sparkline(const History& history, int width);
std::string memory_text(std::optional<std::uint64_t> used, std::optional<std::uint64_t> total);
// Windows counter instances carry both the linked adapter LUID and physical
// index. Normalize hex casing and retain the entire index, never a prefix.
std::optional<std::string> counter_gpu_id(std::string_view instance);

} // namespace ck::sysmon

namespace ck::sysmon {
std::optional<std::vector<ModelInfo>> parse_ollama(std::string_view json);
}
