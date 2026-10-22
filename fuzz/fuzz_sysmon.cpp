// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "monitor.hpp"
#include "settings.hpp"
#include "fuzz_common.hpp"
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const auto sample = ck::sysmon::parse_sample(input);
    if (sample) ck::fuzz::require(ck::sysmon::parse_sample(ck::sysmon::sample_text(*sample)).has_value());
    (void)ck::sysmon::parse_ollama(input);
    (void)ck::sysmon::parse_v3d_activity(input);
    (void)ck::sysmon::parse_v3d_buffers(input);
    const auto gpu = ck::sysmon::counter_gpu_id(input);
    if (gpu) ck::fuzz::require(ck::sysmon::counter_gpu_id(*gpu) == gpu);
    if (const auto settings = ck::sysmon::parse_settings(input))
        ck::fuzz::require(ck::sysmon::parse_settings(ck::sysmon::settings_text(*settings)) == settings);
    return 0;
}
