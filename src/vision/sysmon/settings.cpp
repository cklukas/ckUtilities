// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.hpp"
#include "cvision/core/utf8.hpp"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>

namespace ck::sysmon {
namespace {
bool endpoint_valid(std::string_view value) {
    return value.size() <= 253 && (value.empty() || value.front() != '-') &&
        value.find_first_of("*?![\\\r\n\t ,\"") == value.npos &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c != 127; }) && ckv::utf8::is_valid(value);
}
bool exhausted(std::istringstream& input) { input >> std::ws; return input.eof(); }
}
std::string settings_text(const Settings& settings) {
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "CKSYSMON_SETTINGS 1\nTHEME " << (settings.dark ? "dark" : "standard") << "\nMETRICS ";
    for (const bool visible : settings.metrics) out << (visible ? '1' : '0');
    out << '\n';
    for (const auto& [endpoint, selected] : settings.hosts)
        out << "HOST " << std::quoted(endpoint) << ' ' << (selected ? '1' : '0') << '\n';
    out << "END\n";
    return out.str();
}
std::optional<Settings> parse_settings(std::string_view text) {
    if (text.size() > 65536) return {};
    std::istringstream input{std::string(text)}; input.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(input, line) || line != "CKSYSMON_SETTINGS 1") return {};
    Settings settings;
    bool theme = false, metrics = false;
    while (std::getline(input, line)) {
        std::istringstream fields(line); fields.imbue(std::locale::classic());
        std::string kind; fields >> kind;
        if (kind == "END") {
            if (!theme || !metrics || !exhausted(fields) || input.peek() != std::char_traits<char>::eof()) return {};
            return settings;
        }
        if (kind == "THEME") {
            std::string value;
            if (theme || !(fields >> value) || !exhausted(fields) || (value != "standard" && value != "dark")) return {};
            settings.dark = value == "dark"; theme = true;
        } else if (kind == "METRICS") {
            std::string value;
            if (metrics || !(fields >> value) || !exhausted(fields) || value.size() != settings.metrics.size()) return {};
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] != '0' && value[i] != '1') return {};
                settings.metrics[i] = value[i] == '1';
            }
            metrics = true;
        } else if (kind == "HOST") {
            std::string endpoint, value;
            fields >> std::ws;
            if (fields.peek() != '"' || !(fields >> std::quoted(endpoint) >> value) || !exhausted(fields) ||
                !endpoint_valid(endpoint) || (value != "0" && value != "1") || settings.hosts.size() >= 513 ||
                !settings.hosts.emplace(endpoint, value == "1").second) return {};
        } else return {};
    }
    return {};
}
SettingsLoad SettingsStore::load() {
    revision_.reset(); writable_ = false;
    if (path_.empty()) return {{}, "Settings unavailable: no user configuration directory"};
    const auto file = files_.read_file(path_);
    if (!file) {
        if (files_.exists(path_)) return {{}, "Cannot read settings: " + path_};
        writable_ = true; return {};
    }
    revision_ = file->fingerprint; writable_ = true;
    if (auto settings = parse_settings(file->contents)) return {std::move(*settings), {}};
    return {{}, "Invalid settings; using defaults: " + path_};
}
std::string SettingsStore::save(const Settings& settings) {
    if (!writable_) return "Settings cannot be saved: " + path_;
    const auto text = settings_text(settings);
    if (!parse_settings(text)) return "Settings cannot be saved: invalid option data";
    if (!files_.create_directories(files_.parent(path_))) return "Cannot create settings directory: " + path_;
    const auto result = files_.write_file_atomic(path_, text, revision_
        ? ckv::FileWriteExpectation::matching(*revision_) : ckv::FileWriteExpectation::must_not_exist());
    if (result.status == ckv::FileWriteStatus::Conflict) return "Settings changed in another process; relaunch to reload";
    if (result.status != ckv::FileWriteStatus::Ok) return "Cannot save settings: " + path_;
    revision_ = result.fingerprint;
    return {};
}
} // namespace ck::sysmon
