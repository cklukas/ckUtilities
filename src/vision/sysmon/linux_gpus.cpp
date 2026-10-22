// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_monitor.hpp"
#include <charconv>
#include <fstream>

namespace ck::sysmon {
namespace {
std::string snapshot(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    char buffer[4097];
    input.read(buffer, sizeof buffer);
    if (input.bad() || input.gcount() > 4096) return {};
    return std::string(buffer, static_cast<std::size_t>(input.gcount()));
}
std::optional<std::uint64_t> counter(const std::filesystem::path& path) {
    const auto text = snapshot(path);
    const auto end = text.find_first_of(" \r\n\t");
    const auto token = std::string_view(text).substr(0, end);
    std::uint64_t value{};
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (token.empty() || result.ec != std::errc{} || result.ptr != token.data() + token.size()) return {};
    if (end != text.npos && text.find_first_not_of(" \r\n\t", end) != text.npos) return {};
    return value;
}
}
void linux_drm_gpus(Sample& sample, const std::filesystem::path& cards, const std::filesystem::path& debug) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(cards, error), end; !error && it != end; it.increment(error)) {
        const auto name = it->path().filename().string();
        if (!name.starts_with("card") || name.size() == 4 || name.find_first_not_of("0123456789", 4) != name.npos || sample.gpus.size() >= 32) continue;
        const auto device = it->path() / "device";
        GpuSample gpu;
        gpu.id = std::filesystem::weakly_canonical(device, error).generic_string();
        if (error) { error.clear(); continue; }
        const auto driver = std::filesystem::read_symlink(device / "driver", error).filename();
        error.clear();
        if (driver == "v3d") {
            gpu.name = "V3D " + name;
            gpu.memory_kind = GpuMemoryKind::V3dBuffers;
            if (auto activity = parse_v3d_activity(snapshot(device / "gpu_stats"))) gpu.engines = std::move(*activity);
            gpu.used = parse_v3d_buffers(snapshot(debug / name.substr(4) / "bo_stats"));
        } else {
            auto vendor = snapshot(device / "vendor");
            vendor.erase(vendor.find_last_not_of(" \r\n\t") + 1);
            if (vendor.empty() || vendor == "0x10de") continue;
            gpu.name = (vendor == "0x1002" ? "AMD " : vendor == "0x8086" ? "Intel " : "GPU ") + name;
            if (const auto busy = counter(device / "gpu_busy_percent"); busy && *busy <= 100) gpu.load = static_cast<double>(*busy);
            gpu.used = counter(device / "mem_info_vram_used"); gpu.total = counter(device / "mem_info_vram_total"); gpu.shared = counter(device / "mem_info_gtt_used");
        }
        sample.gpus.push_back(std::move(gpu));
    }
}
} // namespace ck::sysmon
