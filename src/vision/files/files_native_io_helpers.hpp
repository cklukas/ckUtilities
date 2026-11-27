// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_cursor.hpp"
#include "native_filesystem.hpp"
#include "native_name.hpp"
#include "native_path.hpp"
#include "opened_entry.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ck::vision::detail {

inline std::uint64_t next_stage_nonce() noexcept {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

inline std::string truncate_utf8_boundary(std::string_view sv, std::size_t max_bytes) {
    if (sv.size() <= max_bytes)
        return std::string(sv);
    std::size_t len = max_bytes;
    while (len > 0 && (static_cast<unsigned char>(sv[len]) & 0xC0U) == 0x80U)
        --len;
    return std::string(sv.substr(0, len));
}

inline std::optional<std::u16string> utf8_to_u16(std::string_view utf8) {
    std::u16string units;
    units.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c0 = static_cast<unsigned char>(utf8[i]);
        std::uint32_t cp = 0;
        if (c0 < 0x80U) {
            cp = c0;
            ++i;
        } else if ((c0 & 0xE0U) == 0xC0U && i + 1 < utf8.size()) {
            const unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
            if ((c1 & 0xC0U) != 0x80U)
                return std::nullopt;
            cp = ((c0 & 0x1FU) << 6U) | (c1 & 0x3FU);
            if (cp < 0x80U)
                return std::nullopt; // Overlong encoding.
            i += 2;
        } else if ((c0 & 0xF0U) == 0xE0U && i + 2 < utf8.size()) {
            const unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(utf8[i + 2]);
            if ((c1 & 0xC0U) != 0x80U || (c2 & 0xC0U) != 0x80U)
                return std::nullopt;
            cp = ((c0 & 0x0FU) << 12U) | ((c1 & 0x3FU) << 6U) | (c2 & 0x3FU);
            if (cp < 0x800U || (cp >= 0xD800U && cp <= 0xDFFFU))
                return std::nullopt; // Overlong or UTF-16 surrogate code point.
            i += 3;
        } else if ((c0 & 0xF8U) == 0xF0U && i + 3 < utf8.size()) {
            const unsigned char c1 = static_cast<unsigned char>(utf8[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(utf8[i + 2]);
            const unsigned char c3 = static_cast<unsigned char>(utf8[i + 3]);
            if ((c1 & 0xC0U) != 0x80U || (c2 & 0xC0U) != 0x80U || (c3 & 0xC0U) != 0x80U)
                return std::nullopt;
            cp = ((c0 & 0x07U) << 18U) | ((c1 & 0x3FU) << 12U) |
                 ((c2 & 0x3FU) << 6U) | (c3 & 0x3FU);
            if (cp < 0x10000U || cp > 0x10FFFFU)
                return std::nullopt; // Overlong or out-of-range code point.
            i += 4;
        } else {
            return std::nullopt;
        }
        if (cp <= 0xFFFFU) {
            units.push_back(static_cast<char16_t>(cp));
        } else {
            cp -= 0x10000U;
            units.push_back(static_cast<char16_t>(0xD800U + (cp >> 10U)));
            units.push_back(static_cast<char16_t>(0xDC00U + (cp & 0x3FFU)));
        }
    }
    return units;
}

inline std::optional<std::string> host_path_utf8(const ck::files::NativeAbsolutePath &path) {
    auto hp = path.host_path();
    if (!hp.value())
        return std::nullopt;
    const auto u8 = hp.value()->u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

inline std::optional<ck::files::NativeName> native_name_from_utf8(
    ck::files::NameEncoding encoding, std::string_view utf8) {
    if (encoding == ck::files::NameEncoding::PosixBytes)
        return ck::files::NativeName::from_posix(std::string(utf8));
    auto units = utf8_to_u16(utf8);
    if (!units)
        return std::nullopt;
    return ck::files::NativeName::from_windows(std::move(*units));
}

struct SplitNativePath final {
    ck::files::NativeAbsolutePath parent;
    ck::files::NativeName leaf;
};

inline std::optional<SplitNativePath> split_native_path(const ck::files::NativeAbsolutePath &path) {
    if (!path.is_valid())
        return std::nullopt;
    auto parent_obs = path.parent_address();
    if (!parent_obs.value())
        return std::nullopt;
    auto rel_obs = path.relative_components_from(*parent_obs.value());
    if (!rel_obs.value() || rel_obs.value()->size() != 1)
        return std::nullopt;
    return SplitNativePath{*parent_obs.value(), std::move((*rel_obs.value())[0])};
}

inline std::optional<ck::files::ObjectIdentity> inspect_child_identity(
    const ck::files::DirectoryCursor &dir, const ck::files::NativeName &name) {
    auto opened = dir.open_child(name, ck::files::EntryAccess::MetadataOnly);
    if (!opened.value())
        return std::nullopt;
    auto basic = (*opened.value())->inspect_basic();
    if (!basic.value() || !basic.value()->identity.value())
        return std::nullopt;
    return *basic.value()->identity.value();
}

inline bool read_entry_exact(ck::files::OpenedEntry &entry,
                             std::uint64_t offset,
                             std::size_t length,
                             std::vector<std::byte> &out,
                             std::stop_token stop_token) {
    out.clear();
    out.reserve(length);
    std::uint64_t cur = offset;
    std::size_t remaining = length;
    while (remaining > 0) {
        if (stop_token.stop_requested())
            return false;
        const std::size_t chunk = std::min<std::size_t>(64 * 1024, remaining);
        auto got = entry.read_content(ck::files::ContentReadLimits{chunk}, cur, stop_token);
        if (!got.value() || got.value()->bytes.empty())
            return false;
        const std::size_t take = std::min<std::size_t>(remaining, got.value()->bytes.size());
        out.insert(out.end(), got.value()->bytes.begin(),
                   got.value()->bytes.begin() + static_cast<std::ptrdiff_t>(take));
        cur += take;
        remaining -= take;
    }
    return true;
}

inline bool read_native_file_bounded(const ck::files::NativeAbsolutePath &path,
                                     std::uint64_t max_bytes,
                                     std::vector<std::byte> &out,
                                     std::uint64_t &logical_size_out,
                                     bool &truncated_out,
                                     std::string &error_out,
                                     std::stop_token stop_token) {
    auto split = split_native_path(path);
    if (!split) {
        error_out = "Unable to split native file path.";
        return false;
    }
    ck::files::NativeFileSystem fs;
    auto dir = fs.open_native_directory(split->parent);
    if (!dir.value()) {
        error_out = "Unable to open parent directory.";
        return false;
    }
    auto entry = (*dir.value())->open_child(split->leaf, ck::files::EntryAccess::ContentRead);
    if (!entry.value()) {
        error_out = "Unable to open file.";
        return false;
    }
    auto basic = (*entry.value())->inspect_basic();
    if (!basic.value() || !basic.value()->kind.value() ||
        *basic.value()->kind.value() != ck::files::ObjectKind::RegularFile) {
        error_out = "Path is not a regular file.";
        return false;
    }
    logical_size_out = basic.value()->logical_size.value()
        ? *basic.value()->logical_size.value() : 0;
    const std::uint64_t limit_len = std::min<std::uint64_t>(logical_size_out, max_bytes);
    truncated_out = logical_size_out > max_bytes;
    out.clear();
    out.reserve(static_cast<std::size_t>(limit_len));
    std::uint64_t offset = 0;
    while (offset < limit_len) {
        if (stop_token.stop_requested()) {
            error_out = "Read cancelled.";
            return false;
        }
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(64 * 1024, limit_len - offset));
        auto got = (*entry.value())->read_content(
            ck::files::ContentReadLimits{chunk}, offset, stop_token);
        if (!got.value()) {
            error_out = "Failed reading file bytes.";
            return false;
        }
        if (got.value()->bytes.empty())
            break;
        const std::size_t take = static_cast<std::size_t>(
            std::min<std::uint64_t>(limit_len - offset, got.value()->bytes.size()));
        out.insert(out.end(), got.value()->bytes.begin(),
                   got.value()->bytes.begin() + static_cast<std::ptrdiff_t>(take));
        offset += take;
    }
    return true;
}

inline bool write_native_file_staged(const ck::files::NativeAbsolutePath &target_path,
                                     std::span<const std::byte> data,
                                     bool allow_replace,
                                     std::string_view stage_prefix,
                                     std::string &error_out,
                                     std::stop_token stop_token) {
    auto split = split_native_path(target_path);
    if (!split) {
        error_out = "Invalid target path.";
        return false;
    }
    ck::files::NativeFileSystem fs;
    auto dir = fs.open_native_directory(split->parent);
    if (!dir.value()) {
        error_out = "Unable to open target directory.";
        return false;
    }

    // Check if target already exists when !allow_replace.
    const auto existing_id = inspect_child_identity(**dir.value(), split->leaf);
    if (existing_id && !allow_replace) {
        error_out = "Target file already exists.";
        return false;
    }

    const std::uint64_t nonce = next_stage_nonce();
    const std::string suffix = "." + std::to_string(nonce);
    const std::string base_leaf = truncate_utf8_boundary(split->leaf.display_name(), 120);
    std::string stage_str = std::string(stage_prefix) + base_leaf + suffix;
    auto stage_name = native_name_from_utf8(split->parent.encoding(), stage_str);
    if (!stage_name) {
        error_out = "Unable to form staging filename.";
        return false;
    }

    auto staged = (*dir.value())->create_child_file(*stage_name);
    if (!staged.value()) {
        error_out = "Unable to create staging file.";
        return false;
    }

    auto stage_basic = (*staged.value())->inspect_basic();
    if (!stage_basic.value() || !stage_basic.value()->identity.value()) {
        error_out = "Unable to inspect staging file identity.";
        return false;
    }
    const ck::files::ObjectIdentity stage_id = *stage_basic.value()->identity.value();

    std::uint64_t offset = 0;
    while (offset < data.size()) {
        if (stop_token.stop_requested()) {
            (void)(*staged.value())->finish_content_writes();
            std::move(staged).take_value().reset();
            (void)(*dir.value())->retire_child(*stage_name, stage_id);
            error_out = "Write cancelled.";
            return false;
        }
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(64 * 1024, data.size() - offset));
        auto wrote = (*staged.value())->write_content(
            offset, data.subspan(static_cast<std::size_t>(offset), chunk),
            ck::files::ContentWriteLimits{}, stop_token);
        if (!wrote.value() || *wrote.value() != chunk) {
            (void)(*staged.value())->finish_content_writes();
            std::move(staged).take_value().reset();
            (void)(*dir.value())->retire_child(*stage_name, stage_id);
            error_out = "Failed writing staging bytes.";
            return false;
        }
        offset += chunk;
    }

    if (!(*staged.value())->flush_content().value() ||
        !(*staged.value())->finish_content_writes().value()) {
        std::move(staged).take_value().reset();
        (void)(*dir.value())->retire_child(*stage_name, stage_id);
        error_out = "Failed flushing staging file.";
        return false;
    }
    std::move(staged).take_value().reset();

    if (!existing_id) {
        auto published = (*dir.value())->rename_child_noreplace(
            *stage_name, stage_id, **dir.value(), split->leaf);
        if (!published.value()) {
            (void)(*dir.value())->retire_child(*stage_name, stage_id);
            error_out = "Target file already exists or could not be published.";
            return false;
        }
        return true;
    }

    // Safe replacement: rename existing to backup, rename staging to target, then retire backup.
    // On publish failure, restore backup so original file is never lost.
    std::string bak_str = std::string(stage_prefix) + "bak-" + base_leaf + suffix;
    auto bak_name = native_name_from_utf8(split->parent.encoding(), bak_str);
    if (!bak_name) {
        (void)(*dir.value())->retire_child(*stage_name, stage_id);
        error_out = "Unable to form backup filename.";
        return false;
    }

    auto backed = (*dir.value())->rename_child_noreplace(
        split->leaf, *existing_id, **dir.value(), *bak_name);
    if (!backed.value()) {
        (void)(*dir.value())->retire_child(*stage_name, stage_id);
        error_out = "Failed moving existing target aside for replacement.";
        return false;
    }
    auto published = (*dir.value())->rename_child_noreplace(
        *stage_name, stage_id, **dir.value(), split->leaf);
    if (!published.value()) {
        (void)(*dir.value())->rename_child_noreplace(
            *bak_name, *existing_id, **dir.value(), split->leaf);
        (void)(*dir.value())->retire_child(*stage_name, stage_id);
        error_out = "Failed publishing replacement file (original preserved).";
        return false;
    }
    (void)(*dir.value())->retire_child(*bak_name, *existing_id);
    return true;
}

} // namespace ck::vision::detail
