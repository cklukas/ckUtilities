// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_settings.hpp"
#include "cvision/core/utf8.hpp"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>

namespace ck::vision {
namespace {
constexpr std::size_t kMaxSettingsBytes = 1024 * 1024;
constexpr std::size_t kMaxBookmarksPerPane = 256;
constexpr std::size_t kMaxBookmarkUnits = 256 * 1024;

char hex_digit(unsigned value) { return "0123456789abcdef"[value & 15U]; }

std::string encode_bytes(std::string_view value) {
    std::string result;
    result.reserve(value.size() * 2);
    for (const unsigned char byte : value) {
        result.push_back(hex_digit(byte >> 4));
        result.push_back(hex_digit(byte));
    }
    return result;
}

std::string encode_units(const std::u16string &value) {
    std::string result;
    result.reserve(value.size() * 4);
    for (const char16_t unit : value) {
        const auto value_bits = static_cast<std::uint16_t>(unit);
        result.push_back(hex_digit(value_bits >> 12));
        result.push_back(hex_digit(value_bits >> 8));
        result.push_back(hex_digit(value_bits >> 4));
        result.push_back(hex_digit(value_bits));
    }
    return result;
}

int hex_value(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::optional<std::string> decode_bytes(std::string_view value, std::size_t max_bytes) {
    if (value.size() % 2 != 0 || value.size() / 2 > max_bytes) return {};
    std::string result(value.size() / 2, '\0');
    for (std::size_t i = 0; i < result.size(); ++i) {
        const int high = hex_value(value[i * 2]);
        const int low = hex_value(value[i * 2 + 1]);
        if (high < 0 || low < 0) return {};
        result[i] = static_cast<char>((high << 4) | low);
    }
    return result;
}

std::optional<std::u16string> decode_units(std::string_view value, std::size_t max_units) {
    if (value.size() % 4 != 0 || value.size() / 4 > max_units) return {};
    std::u16string result(value.size() / 4, u'\0');
    for (std::size_t i = 0; i < result.size(); ++i) {
        std::uint16_t unit = 0;
        for (std::size_t digit = 0; digit < 4; ++digit) {
            const int nibble = hex_value(value[i * 4 + digit]);
            if (nibble < 0) return {};
            unit = static_cast<std::uint16_t>((unit << 4) | static_cast<unsigned>(nibble));
        }
        result[i] = static_cast<char16_t>(unit);
    }
    return result;
}

bool boolean_field(std::string_view value, bool &result) {
    if (value == "0") { result = false; return true; }
    if (value == "1") { result = true; return true; }
    return false;
}

bool size_field(std::string_view value, std::size_t &result) {
    if (value.empty()) return false;
    const auto *first = value.data();
    const auto *last = first + value.size();
    unsigned long long parsed = 0;
    const auto conversion = std::from_chars(first, last, parsed);
    if (conversion.ec != std::errc{} || conversion.ptr != last || parsed > static_cast<unsigned long long>(SIZE_MAX))
        return false;
    result = static_cast<std::size_t>(parsed);
    return true;
}

bool view_fields(const std::vector<std::string> &fields, FilesPersistentSettings &settings) {
    if ((fields.size() != 7 && fields.size() != 8) || fields[0] != "VIEW") return false;
    std::size_t pane = 0, sort = 0;
    bool descending = false, folders_first = false, show_hidden = false, case_sensitive = false;
    if (!size_field(fields[1], pane) || pane >= settings.views.size() || !size_field(fields[2], sort) || sort > 3 ||
        !boolean_field(fields[3], descending) || !boolean_field(fields[4], folders_first) ||
        !boolean_field(fields[5], show_hidden) || !boolean_field(fields[6], case_sensitive))
        return false;
    const auto filter = decode_bytes(fields.size() == 8 ? fields[7] : std::string_view{}, 4096);
    if (!filter || filter->size() > 4096 || !ckv::utf8::is_valid(*filter)) return false;
    auto &view = settings.views[pane];
    view.sort = static_cast<ck::files::PaneSortKey>(sort);
    view.descending = descending;
    view.folders_first = folders_first;
    view.show_hidden = show_hidden;
    view.ascii_case_sensitive = case_sensitive;
    view.display_substring = std::move(*filter);
    return ck::files::valid_pane_order({view.sort, view.descending, view.folders_first});
}

bool bookmark_fields(const std::vector<std::string> &fields, FilesPersistentSettings &settings) {
    if (fields.size() < 4 || fields[0] != "BOOKMARK") return false;
    std::size_t pane = 0;
    if (!size_field(fields[1], pane) || pane >= settings.bookmarks.size() ||
        settings.bookmarks[pane].size() >= kMaxBookmarksPerPane)
        return false;
    if (fields[2] == "P") {
        const auto bytes = decode_bytes(fields[3], kMaxBookmarkUnits);
        if (!bytes) return false;
        auto root = ck::files::NativeAbsolutePath::from_posix(*bytes);
        if (!root.value()) return false;
        std::vector<ck::files::NativeName> components;
        components.reserve(fields.size() - 4);
        for (std::size_t i = 4; i < fields.size(); ++i) {
            const auto component = decode_bytes(fields[i], kMaxBookmarkUnits);
            if (!component) return false;
            auto name = ck::files::NativeName::from_posix(*component);
            if (!name) return false;
            components.push_back(std::move(*name));
        }
        auto location = FilesPaneLocation::from_components(std::move(root).take_value(), std::move(components), {},
                                                            FilesPaneLocation::hard_components);
        if (!location.value()) return false;
        settings.bookmarks[pane].push_back(std::move(location).take_value());
        return true;
    }
    if (fields[2] == "W") {
        const auto units = decode_units(fields[3], kMaxBookmarkUnits);
        if (!units) return false;
        auto root = ck::files::NativeAbsolutePath::from_windows(*units);
        if (!root.value()) return false;
        std::vector<ck::files::NativeName> components;
        components.reserve(fields.size() - 4);
        for (std::size_t i = 4; i < fields.size(); ++i) {
            const auto component = decode_units(fields[i], kMaxBookmarkUnits);
            if (!component) return false;
            auto name = ck::files::NativeName::from_windows(*component);
            if (!name) return false;
            components.push_back(std::move(*name));
        }
        auto location = FilesPaneLocation::from_components(std::move(root).take_value(), std::move(components), {},
                                                            FilesPaneLocation::hard_components);
        if (!location.value()) return false;
        settings.bookmarks[pane].push_back(std::move(location).take_value());
        return true;
    }
    return false;
}

std::vector<std::string> split_fields(std::string_view line) {
    std::istringstream input{std::string(line)};
    input.imbue(std::locale::classic());
    std::vector<std::string> fields;
    std::string field;
    while (input >> field) fields.push_back(std::move(field));
    return fields;
}
} // namespace

std::string files_settings_text(const FilesPersistentSettings &settings) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "CKFILES_SETTINGS 1\n";
    for (std::size_t pane = 0; pane < settings.views.size(); ++pane) {
        const auto &view = settings.views[pane];
        out << "VIEW " << pane << ' ' << static_cast<unsigned>(view.sort) << ' ' << (view.descending ? 1 : 0) << ' '
            << (view.folders_first ? 1 : 0) << ' ' << (view.show_hidden ? 1 : 0) << ' '
            << (view.ascii_case_sensitive ? 1 : 0) << ' ' << encode_bytes(view.display_substring) << '\n';
        for (const auto &bookmark : settings.bookmarks[pane]) {
            out << "BOOKMARK " << pane << ' ';
            if (const auto *bytes = bookmark.root_address().posix_bytes()) {
                out << "P " << encode_bytes(*bytes);
                for (const auto &component : bookmark.components())
                    out << ' ' << encode_bytes(*component.posix_bytes());
            } else {
                out << "W " << encode_units(*bookmark.root_address().windows_units());
                for (const auto &component : bookmark.components())
                    out << ' ' << encode_units(*component.windows_units());
            }
            out << '\n';
        }
    }
    out << "END\n";
    return out.str();
}

std::optional<FilesPersistentSettings> parse_files_settings(std::string_view text) {
    if (text.empty() || text.size() > kMaxSettingsBytes) return {};
    FilesPersistentSettings settings;
    std::array<bool, 2> seen_view{};
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(input, line) || line != "CKFILES_SETTINGS 1") return {};
    while (std::getline(input, line)) {
        if (line == "END") {
            if (!seen_view[0] || !seen_view[1] || input.peek() != std::char_traits<char>::eof()) return {};
            return settings;
        }
        const auto fields = split_fields(line);
        if (fields.empty()) return {};
        if (fields[0] == "VIEW") {
            if (fields.size() < 2) return {};
            std::size_t pane = 0;
            if (!size_field(fields[1], pane) || pane >= seen_view.size() || seen_view[pane] || !view_fields(fields, settings))
                return {};
            seen_view[pane] = true;
        } else if (fields[0] == "BOOKMARK") {
            if (!bookmark_fields(fields, settings)) return {};
        } else return {};
    }
    return {};
}

FilesSettingsLoad FilesSettingsStore::load() {
    revision_.reset();
    writable_ = false;
    if (path_.empty()) return {{}, "Settings unavailable: no user configuration directory"};
    const auto file = files_.read_file(path_);
    if (!file) {
        if (files_.exists(path_)) return {{}, "Cannot read Files settings: " + path_};
        writable_ = true;
        return {};
    }
    revision_ = file->fingerprint;
    writable_ = true;
    if (auto settings = parse_files_settings(file->contents)) return {std::move(*settings), {}};
    return {{}, "Invalid Files settings; using defaults: " + path_};
}

std::string FilesSettingsStore::save(const FilesPersistentSettings &settings) {
    if (!writable_) return "Files settings cannot be saved: " + path_;
    const auto text = files_settings_text(settings);
    if (!parse_files_settings(text)) return "Files settings cannot be saved: invalid option data";
    if (!files_.create_directories(files_.parent(path_))) return "Cannot create Files settings directory: " + path_;
    const auto result = files_.write_file_atomic(path_, text, revision_
        ? ckv::FileWriteExpectation::matching(*revision_) : ckv::FileWriteExpectation::must_not_exist());
    if (result.status == ckv::FileWriteStatus::Conflict)
        return "Files settings changed in another process; relaunch to reload";
    if (result.status != ckv::FileWriteStatus::Ok) return "Cannot save Files settings: " + path_;
    revision_ = result.fingerprint;
    return {};
}
} // namespace ck::vision
