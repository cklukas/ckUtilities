// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>
#include <limits>
#include <type_traits>

namespace ck::sysmon {
namespace {
void append(History& history, std::optional<double> value) {
    if (history.size() == 60) history.pop_front();
    history.push_back(value);
}
std::string field(std::string text) {
    for (char& c : text) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    return text.substr(0, 256);
}
template<class T> std::optional<T> number(std::string_view text) {
    T value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return {};
    if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(value) || value < 0) return {};
    return value;
}
template<class T> std::string value_text(std::optional<T> value) {
    if (!value) return "-";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12) << *value;
    return out.str();
}
}
std::optional<double> percent(std::optional<std::uint64_t> used, std::optional<std::uint64_t> total) {
    if (!used || !total || *total == 0 || *used > *total) return {};
    return 100.0 * static_cast<double>(*used) / static_cast<double>(*total);
}
std::optional<double> cpu_percent(CpuTicks before, CpuTicks after) {
    if (after.total <= before.total || after.idle < before.idle || after.idle > after.total) return {};
    const auto total = after.total - before.total;
    const auto idle = after.idle - before.idle;
    if (idle > total) return {};
    return 100.0 * static_cast<double>(total - idle) / static_cast<double>(total);
}
std::optional<std::vector<GpuEngineTicks>> parse_v3d_activity(std::string_view text) {
    if (text.size() > 4096) return {};
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(input, line)) return {};
    std::istringstream header(line);
    std::string a, b, c, d, extra;
    if (!(header >> a >> b >> c >> d) || a != "queue" || b != "timestamp" || c != "jobs" || d != "runtime" || header >> extra) return {};
    std::vector<GpuEngineTicks> result;
    std::vector<std::string> seen;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row(line);
        if (!(row >> a >> b >> c >> d) || row >> extra) return {};
        const auto timestamp = number<std::uint64_t>(b), jobs = number<std::uint64_t>(c), runtime = number<std::uint64_t>(d);
        if (!timestamp || !jobs || !runtime || *runtime > *timestamp || seen.size() >= 32 || std::find(seen.begin(), seen.end(), a) != seen.end()) return {};
        seen.push_back(a);
        if (a == "bin" || a == "render" || a == "tfu" || a == "csd") result.push_back({a, *timestamp, *runtime});
    }
    return result.empty() ? std::nullopt : std::optional(std::move(result));
}
std::optional<std::uint64_t> parse_v3d_buffers(std::string_view text) {
    if (text.size() > 4096) return {};
    std::istringstream input{std::string(text)};
    std::string line;
    std::optional<std::uint64_t> result;
    while (std::getline(input, line)) {
        constexpr std::string_view prefix = "allocated bo size (kb):";
        if (!line.starts_with(prefix)) continue;
        std::istringstream row(line.substr(prefix.size()));
        std::string value, extra;
        if (!(row >> value) || row >> extra || result) return {};
        const auto kib = number<std::uint64_t>(value);
        if (!kib || *kib > std::numeric_limits<std::uint64_t>::max() / 1024) return {};
        result = *kib * 1024;
    }
    return result;
}
void HostState::accept(Sample sample) {
    checked_once = true;
    if (!sample.reachable) {
        selected = false;
        previous_cpu.reset();
        sample.cpu.reset(); sample.cpu_load.reset();
        sample.memory_used.reset(); sample.memory_total.reset(); sample.gpus.clear(); sample.models.clear(); sample.ollama_status = "unavailable";
    } else {
        selected = want_selected;
        if (sample.cpu && previous_cpu) sample.cpu_load = cpu_percent(*previous_cpu, *sample.cpu);
        previous_cpu = sample.cpu;
    }
    append(cpu_history, sample.cpu_load);
    append(memory_history, percent(sample.memory_used, sample.memory_total));
    // Device disappearance inserts a gap. Reappearance retains its own history,
    // independent of enumeration order; cap identity retention for hotplug churn.
    for (auto& history : gpu_history) { append(history.load, {}); append(history.memory, {}); }
    for (auto& gpu : sample.gpus) {
        if (gpu.memory_kind == GpuMemoryKind::V3dBuffers) {
            gpu.load.reset();
            const auto previous = std::find_if(latest.gpus.begin(), latest.gpus.end(), [&](const auto& g) { return g.id == gpu.id; });
            if (previous != latest.gpus.end()) {
                bool complete = !gpu.engines.empty() && previous->engines.size() == gpu.engines.size();
                double peak = 0;
                for (const auto& engine : gpu.engines) {
                    const auto old = std::find_if(previous->engines.begin(), previous->engines.end(), [&](const auto& e) { return e.name == engine.name; });
                    if (old == previous->engines.end() || engine.timestamp <= old->timestamp || engine.runtime < old->runtime || engine.runtime - old->runtime > engine.timestamp - old->timestamp) { complete = false; break; }
                    peak = std::max(peak, 100.0 * static_cast<double>(engine.runtime - old->runtime) / static_cast<double>(engine.timestamp - old->timestamp));
                }
                if (complete) gpu.load = peak;
            }
        }
        auto found = std::find_if(gpu_history.begin(), gpu_history.end(), [&](const auto& h) { return h.id == gpu.id; });
        if (found == gpu_history.end()) {
            if (gpu_history.size() >= 64) gpu_history.erase(gpu_history.begin());
            gpu_history.push_back({gpu.id, {}, {}});
            found = std::prev(gpu_history.end());
            append(found->load, {}); append(found->memory, {});
        }
        found->load.back() = gpu.load;
        found->memory.back() = percent(gpu.used, gpu.total);
    }
    latest = std::move(sample);
}
std::string sample_text(const Sample& sample) {
    std::string out = "CKSYSMON 2\nSYSTEM\t" + field(sample.system) + "\n";
    if (sample.cpu) out += "CPU\t" + std::to_string(sample.cpu->total) + "\t" + std::to_string(sample.cpu->idle) + "\n";
    out += "LOAD\t" + value_text(sample.cpu_load) + "\nRAM\t" + value_text(sample.memory_used) + "\t" + value_text(sample.memory_total) + "\n";
    for (const auto& gpu : sample.gpus) {
        out += "GPU\t" + field(gpu.id) + "\t" + field(gpu.name) + "\t" + (gpu.memory_kind == GpuMemoryKind::V3dBuffers ? "-" : value_text(gpu.load)) + "\t" + value_text(gpu.used) + "\t" + value_text(gpu.total) + "\t" + value_text(gpu.shared) + "\t" + value_text(gpu.temperature) + "\t" + value_text(gpu.watts) + "\t" + (gpu.memory_kind == GpuMemoryKind::UnifiedDriver ? "unified" : gpu.memory_kind == GpuMemoryKind::V3dBuffers ? "v3d-buffers" : "dedicated") + "\n";
        for (const auto& engine : gpu.engines)
            out += "GPU_ENGINE\t" + field(gpu.id) + "\t" + field(engine.name) + "\t" + std::to_string(engine.timestamp) + "\t" + std::to_string(engine.runtime) + "\n";
    }
    out += "OLLAMA\t" + field(sample.ollama_status) + "\n";
    for (const auto& model : sample.models) out += "MODEL\t" + field(model.name) + "\t" + value_text(model.vram) + "\n";
    out += "END\n";
    return out;
}
std::optional<Sample> parse_sample(std::string_view text) {
    if (text.size() > 32768 || !text.starts_with("CKSYSMON 2\n")) return {};
    Sample sample; sample.reachable = true;
    bool system = false, cpu = false, load = false, ram = false, end = false;
    text.remove_prefix(11);
    while (!text.empty()) {
        const auto eol = text.find('\n');
        if (eol == text.npos) return {};
        auto line = text.substr(0, eol);
        text.remove_prefix(eol + 1);
        std::vector<std::string_view> fields;
        for (;;) {
            const auto tab = line.find('\t');
            fields.push_back(line.substr(0, tab));
            if (tab == line.npos) break;
            line.remove_prefix(tab + 1);
        }
        const auto valid_text = [](std::string_view f) {
            return !f.empty() && f.size() <= 256 && std::none_of(f.begin(), f.end(), [](unsigned char c) { return c < 32 || c == 127; });
        };
        if (fields[0] == "SYSTEM" && fields.size() == 2 && !system && valid_text(fields[1])) {
            sample.system = fields[1]; system = true;
        } else if (fields[0] == "CPU" && fields.size() == 3 && !cpu) {
            const auto total = number<std::uint64_t>(fields[1]), idle = number<std::uint64_t>(fields[2]);
            if (!total || !idle || *idle > *total) return {};
            sample.cpu = CpuTicks{*total, *idle}; cpu = true;
        } else if (fields[0] == "LOAD" && fields.size() == 2 && !load) {
            sample.cpu_load = number<double>(fields[1]);
            if (fields[1] != "-" && (!sample.cpu_load || *sample.cpu_load > 100)) return {};
            load = true;
        } else if (fields[0] == "RAM" && fields.size() == 3 && !ram) {
            sample.memory_used = number<std::uint64_t>(fields[1]); sample.memory_total = number<std::uint64_t>(fields[2]);
            if ((fields[1] != "-" && !sample.memory_used) || (fields[2] != "-" && !sample.memory_total)) return {};
            if (sample.memory_used && sample.memory_total && !percent(sample.memory_used, sample.memory_total)) return {};
            ram = true;
        } else if (fields[0] == "GPU" && fields.size() == 10 && sample.gpus.size() < 32) {
            if (!valid_text(fields[1]) || !valid_text(fields[2])) return {};
            for (std::size_t i = 3; i < 9; ++i) if (fields[i] != "-" && !number<double>(fields[i])) return {};
            if (fields[9] != "dedicated" && fields[9] != "unified" && fields[9] != "v3d-buffers") return {};
            GpuSample gpu{std::string(fields[1]), std::string(fields[2]), number<double>(fields[3]),
                number<std::uint64_t>(fields[4]), number<std::uint64_t>(fields[5]), number<std::uint64_t>(fields[6]),
                number<double>(fields[7]), number<double>(fields[8])};
            gpu.memory_kind = fields[9] == "unified" ? GpuMemoryKind::UnifiedDriver : fields[9] == "v3d-buffers" ? GpuMemoryKind::V3dBuffers : GpuMemoryKind::Dedicated;
            if (gpu.memory_kind != GpuMemoryKind::Dedicated && gpu.shared) return {};
            for (std::size_t i : {4u, 5u, 6u}) if (fields[i] != "-" && !number<std::uint64_t>(fields[i])) return {};
            if (gpu.load && *gpu.load > 100) return {};
            if (gpu.used && gpu.total && !percent(gpu.used, gpu.total)) return {};
            if (std::any_of(sample.gpus.begin(), sample.gpus.end(), [&](const auto& g) { return g.id == gpu.id; })) return {};
            if (gpu.memory_kind == GpuMemoryKind::V3dBuffers && (gpu.total || gpu.load)) return {};
            sample.gpus.push_back(std::move(gpu));
        } else if (fields[0] == "GPU_ENGINE" && fields.size() == 5) {
            const auto gpu = std::find_if(sample.gpus.begin(), sample.gpus.end(), [&](const auto& g) { return g.id == fields[1]; });
            if (gpu == sample.gpus.end() || gpu->memory_kind != GpuMemoryKind::V3dBuffers || gpu->engines.size() >= 4) return {};
            if (fields[2] != "bin" && fields[2] != "render" && fields[2] != "tfu" && fields[2] != "csd") return {};
            const auto timestamp = number<std::uint64_t>(fields[3]), runtime = number<std::uint64_t>(fields[4]);
            if (!timestamp || !runtime || *runtime > *timestamp || std::any_of(gpu->engines.begin(), gpu->engines.end(), [&](const auto& e) { return e.name == fields[2]; })) return {};
            gpu->engines.push_back({std::string(fields[2]), *timestamp, *runtime});
        } else if (fields[0] == "OLLAMA" && fields.size() == 2 && valid_text(fields[1])) {
            sample.ollama_status = fields[1];
        } else if (fields[0] == "MODEL" && fields.size() == 3 && sample.models.size() < 32 && valid_text(fields[1])) {
            const auto vram = number<std::uint64_t>(fields[2]);
            if (fields[2] != "-" && !vram) return {};
            sample.models.push_back({std::string(fields[1]), vram});
        } else if (fields[0] == "OLLAMA_JSON" && fields.size() == 2) {
            auto models = parse_ollama(fields[1]);
            if (!models) return {};
            sample.models = std::move(*models); sample.ollama_status = "running";
        } else if (fields[0] == "END" && fields.size() == 1 && text.empty()) { end = true; }
        else return {};
    }
    return system && ram && end ? std::optional<Sample>(std::move(sample)) : std::nullopt;
}
std::string sparkline(const History& history, int width) {
    constexpr std::string_view levels[]{"·", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    const auto count = std::min(history.size(), static_cast<std::size_t>(std::max(0, width)));
    std::string result(static_cast<std::size_t>(std::max(0, width)) - count, ' ');
    for (std::size_t i = history.size() - count; i < history.size(); ++i)
        result += history[i] ? levels[static_cast<std::size_t>(std::clamp(*history[i], 0.0, 100.0) * 8 / 100)] : "·";
    return result;
}
std::string memory_text(std::optional<std::uint64_t> used, std::optional<std::uint64_t> total) {
    if (!used || !total) return "not reported";
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(1) << static_cast<double>(*used) / 1073741824.0
        << " / " << static_cast<double>(*total) / 1073741824.0 << " GiB";
    return out.str();
}
std::optional<std::string> counter_gpu_id(std::string_view instance) {
    std::string normalized(instance);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c); });
    const auto begin = normalized.find("luid_0x");
    if (begin == normalized.npos) return {};
    const std::string_view suffix(normalized.data() + begin, normalized.size() - begin);
    if (suffix.size() < 33 || suffix.substr(15, 3) != "_0x" || suffix.substr(26, 6) != "_phys_") return {};
    for (const std::size_t offset : {7u, 18u})
        for (std::size_t i = offset; i < offset + 8; ++i)
            if (!((suffix[i] >= '0' && suffix[i] <= '9') || (suffix[i] >= 'a' && suffix[i] <= 'f'))) return {};
    std::size_t end = 32;
    while (end < suffix.size() && suffix[end] >= '0' && suffix[end] <= '9') ++end;
    if (end == 32 || (end < suffix.size() && suffix[end] != '_')) return {};
    return std::string(suffix.substr(0, end));
}
} // namespace ck::sysmon
