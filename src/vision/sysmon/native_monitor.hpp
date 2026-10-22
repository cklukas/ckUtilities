// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "monitor.hpp"
#include "cvision/core/clock.hpp"
#include <memory>
#include <filesystem>

namespace ck::sysmon {
// Include paths are resolved relative to the configuration root, as in ssh_config.
std::vector<std::string> configured_hosts(const std::filesystem::path& user_config,
                                         const std::filesystem::path& system_config,
                                         const std::filesystem::path& home);
class NativeMonitorSource final : public MonitorSource {
public:
    explicit NativeMonitorSource(const ckv::Clock& clock);
    ~NativeMonitorSource() override;
    std::vector<std::string> hosts() override;
    Sample sample(const std::string& endpoint) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
Sample native_sample();
void add_ollama(Sample& sample);
void linux_drm_gpus(Sample& sample, const std::filesystem::path& cards, const std::filesystem::path& debug);
} // namespace ck::sysmon
