// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_monitor.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>

namespace ck::sysmon {
namespace {
std::vector<std::string> tokens(std::string_view line) {
    std::vector<std::string> result;
    std::string token;
    char quote = 0;
    bool escape = false;
    for (const char c : line) {
        if (escape) { token += c; escape = false; }
        else if (c == '\\') escape = true;
        else if (quote) { if (c == quote) quote = 0; else token += c; }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '#') break;
        else if (std::isspace(static_cast<unsigned char>(c)) || c == '=') {
            if (!token.empty()) { result.push_back(std::move(token)); token.clear(); }
        } else token += c;
    }
    if (!token.empty()) result.push_back(std::move(token));
    return result;
}
bool wildcard(std::string_view pattern, std::string_view name) {
    std::size_t p = 0, n = 0, star = pattern.npos, retry = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') { star = p++; retry = n; continue; }
        std::size_t atom_end = p;
        bool matches = p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n]);
        if (p < pattern.size() && pattern[p] == '[') {
            const auto closing = pattern.find(']', p + 1);
            if (closing != pattern.npos) {
                std::size_t member = p + 1;
                const bool negate = member < closing && (pattern[member] == '!' || pattern[member] == '^');
                if (negate) ++member;
                bool included = false;
                while (member < closing) {
                    if (member + 2 < closing && pattern[member + 1] == '-') {
                        included = included || (name[n] >= pattern[member] && name[n] <= pattern[member + 2]);
                        member += 3;
                    } else {
                        included = included || name[n] == pattern[member];
                        ++member;
                    }
                }
                matches = included != negate;
                atom_end = closing;
            }
        }
        if (matches) { p = atom_end + 1; ++n; }
        else if (star != pattern.npos) { p = star + 1; n = ++retry; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}
struct Discovery {
    std::filesystem::path home;
    std::set<std::filesystem::path> visited;
    std::set<std::string> aliases;
    void host(std::string_view value) {
        if (!value.empty() && value[0] != '-' && value.size() <= 253 &&
            value.find_first_of("*?![\\\r\n\t ,") == value.npos && aliases.size() < 512)
            aliases.emplace(value);
    }
    void match(const std::vector<std::string>& fields) {
        for (std::size_t i = 1; i < fields.size();) {
            std::string criterion = fields[i++];
            std::transform(criterion.begin(), criterion.end(), criterion.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool negated = criterion.starts_with('!');
            if (negated) criterion.erase(0, 1);
            if (criterion == "all" || criterion == "canonical" || criterion == "final") continue;
            if (i == fields.size()) return;
            const auto& argument = fields[i++];
            if (criterion == "host" || criterion == "originalhost") {
                if (negated) continue;
                std::string_view remaining = argument;
                while (!remaining.empty()) {
                    const auto comma = remaining.find(',');
                    host(remaining.substr(0, comma));
                    if (comma == remaining.npos) break;
                    remaining.remove_prefix(comma + 1);
                }
            } else if (criterion != "exec" && criterion != "localnetwork" && criterion != "tagged" &&
                       criterion != "command" && criterion != "user" && criterion != "localuser" &&
                       criterion != "version" && criterion != "sessiontype") return;
        }
    }
    void read(const std::filesystem::path& path, const std::filesystem::path& base, int depth) {
        if (depth > 16 || visited.size() >= 512 || aliases.size() >= 512) return;
        const auto normalized = path.lexically_normal();
        if (!visited.insert(normalized).second) return;
        std::ifstream input(normalized);
        std::string line;
        std::size_t bytes = 0;
        while (std::getline(input, line) && (bytes += line.size()) <= 1024 * 1024) {
            auto fields = tokens(line);
            if (fields.empty()) continue;
            std::transform(fields[0].begin(), fields[0].end(), fields[0].begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (fields[0] == "match") match(fields);
            for (std::size_t i = 1; i < fields.size(); ++i) {
                const auto& value = fields[i];
                if (fields[0] == "host") host(value);
                if (fields[0] != "include") continue;
                std::filesystem::path include;
                if (value.starts_with("~/")) include = home / value.substr(2);
                else include = std::filesystem::path(value).is_absolute() ? std::filesystem::path(value) : base / value;
                // Expand wildcard components, including directory components, in lexical order.
                std::vector<std::filesystem::path> candidates{include.root_path()};
                for (const auto& part : include.relative_path()) {
                    std::vector<std::filesystem::path> next;
                    for (const auto& parent : candidates) {
                        if (part.string().find_first_of("*?[") == std::string::npos) next.push_back(parent / part);
                        else {
                            std::error_code error;
                            for (std::filesystem::directory_iterator it(parent, error), end; !error && it != end; it.increment(error))
                                if ((part.string().starts_with('.') || !it->path().filename().string().starts_with('.')) &&
                                    wildcard(part.string(), it->path().filename().string()) && next.size() < 512) next.push_back(it->path());
                        }
                    }
                    std::sort(next.begin(), next.end()); candidates = std::move(next);
                }
                for (const auto& candidate : candidates) read(candidate, base, depth + 1);
            }
        }
    }
};
}
std::vector<std::string> configured_hosts(const std::filesystem::path& user_config,
                                         const std::filesystem::path& system_config,
                                         const std::filesystem::path& home) {
    Discovery discovery{home, {}, {}};
    discovery.read(user_config, home / ".ssh", 0);
    discovery.read(system_config, system_config.parent_path(), 0);
    return {discovery.aliases.begin(), discovery.aliases.end()};
}
} // namespace ck::sysmon
