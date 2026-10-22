// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "cvision/core/filesystem.hpp"
#include <array>
#include <map>

namespace ck::sysmon {
struct Settings {
    bool dark = false;
    std::array<bool, 6> metrics{true, true, true, true, true, true};
    // Empty endpoint means local. Values express intent, not reachability.
    std::map<std::string, bool> hosts;
    friend bool operator==(const Settings&, const Settings&) = default;
};
std::string settings_text(const Settings& settings);
std::optional<Settings> parse_settings(std::string_view text);
struct SettingsLoad {
    Settings value;
    std::string diagnostic;
};
class SettingsStore {
public:
    SettingsStore(ckv::FileSystem& files, std::string path) : files_(files), path_(std::move(path)) {}
    SettingsLoad load();
    // Empty means success. Failed writes preserve the previous file.
    std::string save(const Settings& settings);
private:
    ckv::FileSystem& files_;
    std::string path_;
    std::optional<ckv::FileFingerprint> revision_;
    bool writable_ = false;
};
} // namespace ck::sysmon
