// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_embedded_editor.hpp"

#include "directory_cursor.hpp"
#include "files_native_io_helpers.hpp"
#include "markdown_profile.hpp"
#include "native_filesystem.hpp"
#include "opened_entry.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

namespace ck::vision {
namespace {

std::array<std::byte, 32> sha256_bytes(std::span<const std::byte> data) {
    ck::files::Sha256 h;
    h.update(data);
    return h.digest();
}

std::string normalize_to_lf(std::string_view raw, FilesNewlineStyle &detected) {
    detected = FilesNewlineStyle::Lf;
    std::size_t crlf_count = 0;
    std::size_t lf_count = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\n') {
            if (i > 0 && raw[i - 1] == '\r')
                ++crlf_count;
            else
                ++lf_count;
        }
    }
    if (crlf_count > lf_count)
        detected = FilesNewlineStyle::CrLf;

    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')
            continue;
        out.push_back(raw[i]);
    }
    return out;
}

std::string encode_with_newline_style(std::string_view lf_text, FilesNewlineStyle style) {
    if (style == FilesNewlineStyle::Lf)
        return std::string(lf_text);
    std::string out;
    out.reserve(lf_text.size() + lf_text.size() / 16);
    for (std::size_t i = 0; i < lf_text.size(); ++i) {
        if (lf_text[i] == '\n' && (i == 0 || lf_text[i - 1] != '\r'))
            out.push_back('\r');
        out.push_back(lf_text[i]);
    }
    return out;
}

bool read_native_entry_bytes(ck::files::OpenedEntry &entry,
                             std::uint64_t logical_size,
                             std::size_t max_bytes,
                             std::vector<std::byte> &out,
                             std::string &error_out,
                             std::stop_token stop_token) {
    if (logical_size > max_bytes) {
        error_out = "File exceeds editor size limit.";
        return false;
    }
    if (!detail::read_entry_exact(entry, 0, static_cast<std::size_t>(logical_size), out, stop_token)) {
        error_out = stop_token.stop_requested() ? "Read cancelled." : "Failed reading file bytes.";
        return false;
    }
    return true;
}

bool write_staged_replace(const ck::files::NativeAbsolutePath &target_path,
                          std::span<const std::byte> data,
                          bool allow_replace,
                          std::string &error_out,
                          std::stop_token stop_token) {
    return detail::write_native_file_staged(
        target_path, data, allow_replace, ".ck-edit-stage-", error_out, stop_token);
}

std::string format_hex_line(std::uint64_t offset, std::span<const std::byte> row) {
    char buf[96]{};
    int n = std::snprintf(buf, sizeof(buf), "%08llx  ", static_cast<unsigned long long>(offset));
    std::string line(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
    for (std::size_t i = 0; i < 16; ++i) {
        if (i < row.size()) {
            char hx[4]{};
            std::snprintf(hx, sizeof(hx), "%02x ", std::to_integer<unsigned int>(row[i]));
            line.append(hx);
        } else {
            line.append("   ");
        }
        if (i == 7)
            line.push_back(' ');
    }
    line.append(" |");
    for (std::size_t i = 0; i < row.size(); ++i) {
        const unsigned char ch = std::to_integer<unsigned char>(row[i]);
        line.push_back((ch >= 0x20U && ch <= 0x7EU) ? static_cast<char>(ch) : '.');
    }
    line.push_back('|');
    return line;
}

std::vector<std::string> format_json_pretty(std::string_view raw) {
    std::vector<std::string> lines;
    std::string cur;
    int indent = 0;
    bool in_str = false;
    bool escape = false;
    const auto push_line = [&] {
        lines.push_back(std::move(cur));
        cur.clear();
    };
    const auto add_indent = [&] {
        cur.append(static_cast<std::size_t>(std::max(0, indent)) * 2, ' ');
    };
    for (const char ch : raw) {
        if (in_str) {
            cur.push_back(ch);
            if (escape)
                escape = false;
            else if (ch == '\\')
                escape = true;
            else if (ch == '"')
                in_str = false;
            continue;
        }
        if (ch == '"') {
            in_str = true;
            cur.push_back(ch);
        } else if (ch == '{' || ch == '[') {
            cur.push_back(ch);
            push_line();
            ++indent;
            add_indent();
        } else if (ch == '}' || ch == ']') {
            if (!cur.empty() && cur.find_first_not_of(' ') != std::string::npos)
                push_line();
            else
                cur.clear();
            indent = std::max(0, indent - 1);
            add_indent();
            cur.push_back(ch);
        } else if (ch == ',') {
            cur.push_back(ch);
            push_line();
            add_indent();
        } else if (ch == ':') {
            cur.append(": ");
        } else if (!std::isspace(static_cast<unsigned char>(ch))) {
            cur.push_back(ch);
        }
    }
    if (!cur.empty())
        push_line();
    return lines;
}

} // namespace

FilesDocumentOpenResult open_files_editor_document(
    const ck::files::NativeAbsolutePath &path,
    std::size_t max_bytes,
    std::stop_token stop_token) {
    FilesDocumentOpenResult res;
    res.snapshot.path = path;
    if (!path.is_valid()) {
        res.error_message = "Invalid document path.";
        return res;
    }
    auto split = detail::split_native_path(path);
    if (!split) {
        res.error_message = "Unable to resolve document parent directory.";
        return res;
    }
    ck::files::NativeFileSystem fs;
    auto dir = fs.open_native_directory(split->parent);
    if (!dir.value()) {
        res.error_message = "Unable to open parent directory.";
        return res;
    }
    auto entry = (*dir.value())->open_child(split->leaf, ck::files::EntryAccess::ContentRead);
    if (!entry.value()) {
        res.error_message = "Unable to open document for editing.";
        return res;
    }
    auto basic = (*entry.value())->inspect_basic();
    if (!basic.value() || !basic.value()->kind.value() ||
        *basic.value()->kind.value() != ck::files::ObjectKind::RegularFile) {
        res.error_message = "Only regular files can be opened in the embedded editor.";
        return res;
    }
    const std::uint64_t sz = basic.value()->logical_size.value()
        ? *basic.value()->logical_size.value() : 0;
    std::vector<std::byte> raw;
    if (!read_native_entry_bytes(**entry.value(), sz, max_bytes, raw, res.error_message, stop_token))
        return res;

    res.snapshot.existed_at_open = true;
    res.snapshot.logical_size = raw.size();
    if (basic.value()->identity.value())
        res.snapshot.identity = *basic.value()->identity.value();
    if (basic.value()->modified.value())
        res.snapshot.modified = *basic.value()->modified.value();
    if (basic.value()->link_count.value())
        res.snapshot.link_count = *basic.value()->link_count.value();
    res.snapshot.sha256 = sha256_bytes(raw);

    const std::size_t probe = std::min<std::size_t>(raw.size(), 8192);
    for (std::size_t i = 0; i < probe; ++i) {
        if (raw[i] == std::byte{0}) {
            res.binary_warning = true;
            break;
        }
    }

    std::string_view sv(reinterpret_cast<const char *>(raw.data()), raw.size());
    res.text = normalize_to_lf(sv, res.snapshot.newline_style);
    res.ok = true;
    return res;
}

FilesDocumentSaveResult save_files_editor_document(
    const FilesDocumentSnapshot &snapshot,
    std::string_view text,
    bool force_overwrite_conflict,
    std::stop_token stop_token) {
    FilesDocumentSaveResult res;
    res.updated_snapshot = snapshot;
    if (!snapshot.path || !snapshot.path->is_valid()) {
        res.status = FilesSaveStatus::Failed;
        res.diagnostic = "Document has no valid target path; use Save As.";
        return res;
    }
    if (stop_token.stop_requested()) {
        res.status = FilesSaveStatus::Cancelled;
        res.diagnostic = "Save cancelled.";
        return res;
    }

    // Revalidate live file against open-time snapshot to detect external edits/replacement.
    if (snapshot.existed_at_open && !force_overwrite_conflict) {
        auto live = open_files_editor_document(*snapshot.path, 64 * 1024 * 1024, stop_token);
        if (!live.ok) {
            res.status = FilesSaveStatus::ExternalConflict;
            res.diagnostic = "File was removed or became inaccessible since it was opened.";
            return res;
        }
        bool identity_changed = false;
        if (snapshot.identity && live.snapshot.identity) {
            identity_changed =
                snapshot.identity->scheme != live.snapshot.identity->scheme ||
                snapshot.identity->volume != live.snapshot.identity->volume ||
                snapshot.identity->object != live.snapshot.identity->object ||
                snapshot.identity->generation != live.snapshot.identity->generation;
        }
        if (identity_changed || live.snapshot.sha256 != snapshot.sha256) {
            res.status = FilesSaveStatus::ExternalConflict;
            res.diagnostic = identity_changed
                ? "File identity was replaced externally since it was opened."
                : "File content was modified externally since it was opened.";
            return res;
        }
    }

    const std::string encoded = encode_with_newline_style(text, snapshot.newline_style);
    std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte *>(encoded.data()), encoded.size());
    std::string err;
    if (!write_staged_replace(*snapshot.path, bytes, snapshot.existed_at_open || force_overwrite_conflict,
                              err, stop_token)) {
        res.status = stop_token.stop_requested() ? FilesSaveStatus::Cancelled : FilesSaveStatus::Failed;
        res.diagnostic = err;
        return res;
    }

    // Refresh snapshot after successful commit.
    auto refreshed = open_files_editor_document(*snapshot.path, 64 * 1024 * 1024, stop_token);
    if (refreshed.ok) {
        res.updated_snapshot = refreshed.snapshot;
        res.updated_snapshot.newline_style = snapshot.newline_style;
    } else {
        res.updated_snapshot.existed_at_open = true;
        res.updated_snapshot.logical_size = encoded.size();
        res.updated_snapshot.sha256 = sha256_bytes(bytes);
    }
    res.status = FilesSaveStatus::Saved;
    res.diagnostic = "Saved.";
    return res;
}

FilesDocumentSaveResult save_as_files_editor_document(
    const ck::files::NativeAbsolutePath &target_path,
    std::string_view text,
    FilesNewlineStyle newline_style,
    bool allow_replace,
    std::stop_token stop_token) {
    FilesDocumentSaveResult res;
    res.updated_snapshot.path = target_path;
    res.updated_snapshot.newline_style = newline_style;
    if (!target_path.is_valid()) {
        res.status = FilesSaveStatus::Failed;
        res.diagnostic = "Invalid Save As target path.";
        return res;
    }
    const std::string encoded = encode_with_newline_style(text, newline_style);
    std::span<const std::byte> bytes(
        reinterpret_cast<const std::byte *>(encoded.data()), encoded.size());
    std::string err;
    if (!write_staged_replace(target_path, bytes, allow_replace, err, stop_token)) {
        if (!allow_replace && err.find("already exists") != std::string::npos)
            res.status = FilesSaveStatus::TargetExists;
        else
            res.status = FilesSaveStatus::Failed;
        res.diagnostic = err;
        return res;
    }
    auto refreshed = open_files_editor_document(target_path, 64 * 1024 * 1024, stop_token);
    if (refreshed.ok) {
        res.updated_snapshot = refreshed.snapshot;
        res.updated_snapshot.newline_style = newline_style;
    }
    res.status = FilesSaveStatus::Saved;
    res.diagnostic = "Saved.";
    return res;
}

std::unique_ptr<ckv::widgets::SyntaxProfileRegistry> make_files_syntax_registry() {
    auto registry = std::make_unique<ckv::widgets::SyntaxProfileRegistry>();
    (void)register_markdown_syntax_profile(*registry);
    ckv::widgets::register_standard_syntax_profiles(*registry);
    return registry;
}

FilesViewerPage read_files_viewer_page(
    const ck::files::NativeAbsolutePath &path,
    FilesViewerMode mode,
    std::uint64_t offset,
    std::size_t window_bytes,
    std::stop_token stop_token) {
    FilesViewerPage page;
    if (!path.is_valid()) {
        page.error_message = "Invalid file path.";
        return page;
    }
    auto split = detail::split_native_path(path);
    if (!split) {
        page.error_message = "Unable to split viewer path.";
        return page;
    }
    ck::files::NativeFileSystem fs;
    auto dir = fs.open_native_directory(split->parent);
    if (!dir.value()) {
        page.error_message = "Unable to open parent directory.";
        return page;
    }
    auto opened = (*dir.value())->open_child(split->leaf, ck::files::EntryAccess::ContentRead);
    if (!opened.value()) {
        page.error_message = "Unable to open file for viewing.";
        return page;
    }
    auto basic = (*opened.value())->inspect_basic();
    if (!basic.value() || !basic.value()->kind.value() ||
        *basic.value()->kind.value() != ck::files::ObjectKind::RegularFile) {
        page.error_message = "Target is not a regular file.";
        return page;
    }
    page.file_size = basic.value()->logical_size.value()
        ? *basic.value()->logical_size.value() : 0;
    const std::size_t clamped_window = std::clamp<std::size_t>(window_bytes, 256, 256 * 1024);
    const std::uint64_t clamped_offset = std::min(offset, page.file_size);
    const std::size_t to_read = static_cast<std::size_t>(
        std::min<std::uint64_t>(clamped_window, page.file_size - clamped_offset));

    std::vector<std::byte> buf;
    if (to_read > 0 && !detail::read_entry_exact(**opened.value(), clamped_offset, to_read, buf, stop_token)) {
        page.error_message = stop_token.stop_requested() ? "Viewer read cancelled." : "Failed reading viewer window.";
        return page;
    }
    page.window_offset = clamped_offset;
    page.window_bytes = buf.size();
    page.has_more = clamped_offset + buf.size() < page.file_size;

    if (mode == FilesViewerMode::Hex) {
        for (std::size_t i = 0; i < buf.size(); i += 16) {
            const std::size_t row_len = std::min<std::size_t>(16, buf.size() - i);
            page.lines.push_back(format_hex_line(
                clamped_offset + i, std::span<const std::byte>(buf.data() + i, row_len)));
        }
        if (page.lines.empty())
            page.lines.push_back("(empty file)");
    } else if (mode == FilesViewerMode::Json) {
        std::string_view sv(reinterpret_cast<const char *>(buf.data()), buf.size());
        page.lines = format_json_pretty(sv);
        if (page.lines.empty())
            page.lines.push_back("(empty JSON)");
    } else {
        std::string_view sv(reinterpret_cast<const char *>(buf.data()), buf.size());
        std::size_t start = 0;
        while (start < sv.size()) {
            const std::size_t nl = sv.find('\n', start);
            std::string_view raw_line = nl == std::string_view::npos
                ? sv.substr(start)
                : sv.substr(start, nl - start);
            if (!raw_line.empty() && raw_line.back() == '\r')
                raw_line.remove_suffix(1);
            std::string clean;
            clean.reserve(raw_line.size());
            for (const char ch : raw_line) {
                const unsigned char u = static_cast<unsigned char>(ch);
                if (ch == '\t')
                    clean.append("    ");
                else if (u < 0x20U || u == 0x7FU)
                    clean.push_back('.');
                else
                    clean.push_back(ch);
            }
            page.lines.push_back(std::move(clean));
            if (nl == std::string_view::npos)
                break;
            start = nl + 1;
        }
        if (page.lines.empty())
            page.lines.push_back("(empty file)");
    }
    page.ok = true;
    return page;
}

std::optional<std::uint64_t> search_files_viewer_stream(
    const ck::files::NativeAbsolutePath &path,
    std::string_view needle,
    std::uint64_t start_offset,
    bool case_sensitive,
    std::uint64_t max_scan_bytes,
    std::stop_token stop_token) {
    if (needle.empty() || !path.is_valid())
        return std::nullopt;
    auto split = detail::split_native_path(path);
    if (!split)
        return std::nullopt;
    ck::files::NativeFileSystem fs;
    auto dir = fs.open_native_directory(split->parent);
    if (!dir.value())
        return std::nullopt;
    auto opened = (*dir.value())->open_child(split->leaf, ck::files::EntryAccess::ContentRead);
    if (!opened.value())
        return std::nullopt;

    constexpr std::size_t kChunk = 64 * 1024;
    std::string carry;
    std::uint64_t offset = start_offset;
    std::uint64_t scanned = 0;

    const auto eq_ch = [case_sensitive](char a, char b) noexcept {
        if (case_sensitive)
            return a == b;
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };

    while (scanned < max_scan_bytes) {
        if (stop_token.stop_requested())
            return std::nullopt;
        const std::size_t to_read = static_cast<std::size_t>(
            std::min<std::uint64_t>(kChunk, max_scan_bytes - scanned));
        auto got = (*opened.value())->read_content(
            ck::files::ContentReadLimits{to_read}, offset, stop_token);
        if (!got.value() || got.value()->bytes.empty())
            break;
        const auto &bytes = got.value()->bytes;
        const std::uint64_t window_start = offset - carry.size();
        std::string combined;
        combined.reserve(carry.size() + bytes.size());
        combined.append(carry);
        combined.append(reinterpret_cast<const char *>(bytes.data()), bytes.size());

        for (std::size_t i = 0; i + needle.size() <= combined.size(); ++i) {
            bool ok = true;
            for (std::size_t j = 0; j < needle.size(); ++j) {
                if (!eq_ch(combined[i + j], needle[j])) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                return window_start + i;
        }

        const std::size_t keep = needle.size() > 1 ? needle.size() - 1 : 0;
        if (combined.size() > keep)
            carry.assign(combined.data() + (combined.size() - keep), keep);
        else
            carry = std::move(combined);
        offset += bytes.size();
        scanned += bytes.size();
    }
    return std::nullopt;
}

} // namespace ck::vision
