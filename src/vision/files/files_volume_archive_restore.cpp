// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive_restore.hpp"

#include "directory_cursor.hpp"
#include "files_native_io_helpers.hpp"
#include "files_ordinary_archive.hpp"
#include "native_filesystem.hpp"
#include "opened_entry.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <utility>

namespace ck::vision {
namespace {

constexpr std::array<char, 8> kCkArvMagic{'C', 'K', 'A', 'R', 'V', '0', '1', '\0'};
constexpr std::uint32_t kCkArvVersion = 2;
constexpr std::uint64_t kCkArvHeaderSize = 56;

template <class T>
bool read_le(std::span<const std::byte> bytes, std::size_t &offset, T &value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
        return false;
    value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i)
        value |= static_cast<T>(std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8U);
    offset += sizeof(T);
    return true;
}

bool read_str(std::span<const std::byte> bytes, std::size_t &offset, std::string &out,
              std::size_t max_len) {
    std::uint64_t len = 0;
    if (!read_le(bytes, offset, len) || len > max_len || len > bytes.size() - offset)
        return false;
    out.assign(reinterpret_cast<const char *>(bytes.data() + offset), static_cast<std::size_t>(len));
    offset += static_cast<std::size_t>(len);
    return true;
}

bool read_exact_at(ck::files::OpenedEntry &entry,
                   std::uint64_t offset,
                   std::span<std::byte> dst,
                   std::stop_token stop_token) {
    std::vector<std::byte> tmp;
    if (!detail::read_entry_exact(entry, offset, dst.size(), tmp, stop_token) ||
        tmp.size() != dst.size())
        return false;
    std::memcpy(dst.data(), tmp.data(), dst.size());
    return true;
}

std::string decode_archive_entry_path_utf8(std::string_view raw, ck::files::NameEncoding enc) {
    if (enc == ck::files::NameEncoding::PosixBytes)
        return std::string(raw);
    if (raw.size() % 2 != 0)
        return {};
    std::string out;
    out.reserve(raw.size() / 2);
    for (std::size_t i = 0; i < raw.size(); i += 2) {
        const std::uint16_t u = static_cast<std::uint16_t>(
            static_cast<unsigned char>(raw[i]) |
            (static_cast<unsigned char>(raw[i + 1]) << 8U));
        std::uint32_t cp = u;
        if (u >= 0xD800U && u <= 0xDBFFU) {
            if (i + 3 >= raw.size())
                return {};
            const std::uint16_t lo = static_cast<std::uint16_t>(
                static_cast<unsigned char>(raw[i + 2]) |
                (static_cast<unsigned char>(raw[i + 3]) << 8U));
            if (lo < 0xDC00U || lo > 0xDFFFU)
                return {};
            cp = 0x10000U + ((static_cast<std::uint32_t>(u - 0xD800U) << 10U) |
                             static_cast<std::uint32_t>(lo - 0xDC00U));
            i += 2;
        } else if (u >= 0xDC00U && u <= 0xDFFFU) {
            return {}; // Unpaired low surrogate.
        }
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    }
    return out;
}

std::optional<ck::files::NativeName> native_name_from_segment(ck::files::NameEncoding enc,
                                                              std::string_view seg) {
    return detail::native_name_from_utf8(enc, seg);
}

bool split_validated_relative(std::string_view path,
                              ck::files::NameEncoding enc,
                              std::vector<ck::files::NativeName> &parts) {
    parts.clear();
    if (path == ".")
        return true;
    if (!validate_ordinary_archive_entry_path(path))
        return false;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t slash = path.find('/', pos);
        const std::string_view seg = slash == std::string_view::npos
            ? path.substr(pos)
            : path.substr(pos, slash - pos);
        auto nn = native_name_from_segment(enc, seg);
        if (!nn)
            return false;
        parts.push_back(std::move(*nn));
        if (slash == std::string_view::npos)
            break;
        pos = slash + 1;
    }
    return !parts.empty();
}

} // namespace

bool validate_volume_archive_restore_destination(
    const ck::files::NativeAbsolutePath &destination_directory,
    std::string *reason_out) {
    if (!destination_directory.is_valid()) {
        if (reason_out) *reason_out = "Destination directory path is invalid.";
        return false;
    }
    const std::string disp = destination_directory.display_text();
    if (disp.empty()) {
        if (reason_out) *reason_out = "Destination directory path is empty.";
        return false;
    }
    // Block raw devices and live system volume roots (including Windows extended-length paths).
    std::string_view check = disp;
    if (check.starts_with("\\\\?\\") || check.starts_with("//?/"))
        check.remove_prefix(4);
    std::string lower_check;
    lower_check.reserve(check.size());
    for (const unsigned char ch : check)
        lower_check.push_back(static_cast<char>(std::tolower(ch)));
    if (disp == "/" || disp == "\\" || disp.starts_with("/dev/") || disp == "/dev" ||
        disp.starts_with("\\\\.\\") || lower_check.starts_with("physicaldrive") ||
        lower_check.starts_with("volume{") || lower_check.starts_with("globalroot")) {
        if (reason_out)
            *reason_out = "Refused restoring volume archive onto a raw device or system root.";
        return false;
    }
    if (check.size() <= 3 && check.size() >= 2 &&
        std::isalpha(static_cast<unsigned char>(check[0])) && check[1] == ':' &&
        (check.size() == 2 || check[2] == '\\' || check[2] == '/')) {
        if (reason_out)
            *reason_out = "Refused restoring volume archive directly onto a Windows drive root.";
        return false;
    }
    return true;
}

VolumeArchiveInspection inspect_volume_archive_file(
    const ck::files::NativeAbsolutePath &archive_path,
    const VolumeArchiveRestoreLimits &limits,
    std::stop_token stop_token) {
    VolumeArchiveInspection out;
    if (!archive_path.is_valid()) {
        out.error_message = "Invalid volume archive path.";
        return out;
    }
    auto split = detail::split_native_path(archive_path);
    if (!split) {
        out.error_message = "Unable to resolve volume archive parent directory.";
        return out;
    }
    ck::files::NativeFileSystem fs;
    auto dir = fs.open_native_directory(split->parent);
    if (!dir.value()) {
        out.error_message = "Unable to open volume archive parent directory.";
        return out;
    }
    auto opened = (*dir.value())->open_child(split->leaf, ck::files::EntryAccess::ContentRead);
    if (!opened.value()) {
        out.error_message = "Unable to open volume archive file.";
        return out;
    }
    auto basic = (*opened.value())->inspect_basic();
    if (!basic.value() || !basic.value()->kind.value() ||
        *basic.value()->kind.value() != ck::files::ObjectKind::RegularFile) {
        out.error_message = "Volume archive path is not a regular file.";
        return out;
    }
    out.archive_file_size = basic.value()->logical_size.value()
        ? *basic.value()->logical_size.value() : 0;
    if (out.archive_file_size < kCkArvHeaderSize) {
        out.error_message = "File is too small to be a CKARV01 volume archive.";
        return out;
    }

    std::array<std::byte, kCkArvHeaderSize> hdr{};
    if (!read_exact_at(**opened.value(), 0, hdr, stop_token)) {
        out.error_message = "Failed reading CKARV01 header.";
        return out;
    }
    if (std::memcmp(hdr.data(), kCkArvMagic.data(), kCkArvMagic.size()) != 0) {
        out.error_message = "Invalid volume archive magic (expected CKARV01).";
        return out;
    }
    std::size_t hoff = 8;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint64_t manifest_size = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t data_size = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t reserved = 0;
    if (!read_le(hdr, hoff, version) || !read_le(hdr, hoff, flags) ||
        !read_le(hdr, hoff, manifest_size) || !read_le(hdr, hoff, data_offset) ||
        !read_le(hdr, hoff, data_size) || !read_le(hdr, hoff, entry_count) ||
        !read_le(hdr, hoff, reserved)) {
        out.error_message = "Malformed CKARV01 header.";
        return out;
    }
    if (version != kCkArvVersion || reserved != 0) {
        out.error_message = "Unsupported CKARV01 format version or reserved header value.";
        return out;
    }
    if (manifest_size == 0 || manifest_size > limits.max_manifest_bytes ||
        data_offset != kCkArvHeaderSize + manifest_size ||
        entry_count == 0 || entry_count > limits.max_entries ||
        data_offset > out.archive_file_size ||
        data_size > out.archive_file_size - data_offset) {
        out.error_message = "CKARV01 header bounds are corrupt or exceed safety limits.";
        return out;
    }

    out.format_version = version;
    out.manifest_size = manifest_size;
    out.data_offset = data_offset;
    out.data_size = data_size;

    std::vector<std::byte> manifest(static_cast<std::size_t>(manifest_size));
    if (!read_exact_at(**opened.value(), kCkArvHeaderSize, manifest, stop_token)) {
        out.error_message = "Failed reading CKARV01 manifest.";
        return out;
    }

    std::size_t moff = 0;
    if (!read_str(manifest, moff, out.source.identity, 65536) ||
        !read_str(manifest, moff, out.source.name, 4096) ||
        !read_str(manifest, moff, out.source.mount_point, 65536) ||
        !read_str(manifest, moff, out.source.filesystem, 4096)) {
        out.error_message = "Truncated source metadata in CKARV01 manifest.";
        return out;
    }
    std::uint8_t consistency_byte = 0;
    std::uint8_t encoding_byte = 0;
    if (!read_le(manifest, moff, consistency_byte) || !read_le(manifest, moff, encoding_byte)) {
        out.error_message = "Truncated consistency/encoding fields in CKARV01 manifest.";
        return out;
    }
    if (moff + 6 > manifest.size()) {
        out.error_message = "Truncated manifest header padding.";
        return out;
    }
    moff += 6;
    out.consistency = consistency_byte == 1
        ? FilesVolumeArchiveConsistency::Snapshot
        : FilesVolumeArchiveConsistency::Live;
    out.path_encoding = static_cast<ck::files::NameEncoding>(encoding_byte);
    if (out.path_encoding != ck::files::NameEncoding::PosixBytes &&
        out.path_encoding != ck::files::NameEncoding::WindowsUtf16) {
        out.error_message = "Unsupported path encoding in CKARV01 manifest.";
        return out;
    }

    std::set<std::string> seen_paths;
    std::uint64_t total_logical = 0;
    out.entries.reserve(static_cast<std::size_t>(entry_count));

    for (std::uint64_t i = 0; i < entry_count; ++i) {
        if (stop_token.stop_requested()) {
            out.error_message = "Inspection cancelled.";
            return out;
        }
        std::uint8_t kind_byte = 0;
        if (!read_le(manifest, moff, kind_byte) || moff + 7 > manifest.size()) {
            out.error_message = "Truncated entry header in CKARV01 manifest.";
            return out;
        }
        if (kind_byte < static_cast<std::uint8_t>(FilesVolumeArchiveEntryKind::Directory) ||
            kind_byte > static_cast<std::uint8_t>(FilesVolumeArchiveEntryKind::Unsupported)) {
            out.error_message = "Unsupported entry kind in CKARV01 manifest.";
            return out;
        }
        moff += 7;
        VolumeArchiveInspectedEntry entry;
        entry.kind = static_cast<FilesVolumeArchiveEntryKind>(kind_byte);
        std::string raw_path;
        std::string raw_link;
        std::uint64_t extent_count = 0;
        if (!read_le(manifest, moff, entry.logical_size) ||
            !read_le(manifest, moff, entry.hard_link_group) ||
            !read_str(manifest, moff, raw_path, 65536) ||
            !read_str(manifest, moff, raw_link, 65536) ||
            !read_le(manifest, moff, extent_count)) {
            out.error_message = "Truncated entry payload in CKARV01 manifest.";
            return out;
        }
        if (extent_count > (manifest.size() - moff) / 24ULL) {
            out.error_message = "Extent count exceeds remaining CKARV01 manifest bytes.";
            return out;
        }

        entry.path = decode_archive_entry_path_utf8(raw_path, out.path_encoding);
        entry.link_target = decode_archive_entry_path_utf8(raw_link, out.path_encoding);
        if (entry.path.empty()) {
            out.error_message = "Empty or undecodable entry path in CKARV01 manifest.";
            return out;
        }
        if (entry.path != "." && !validate_ordinary_archive_entry_path(entry.path)) {
            out.error_message = "Hostile path rejected in CKARV01 manifest: " + entry.path;
            return out;
        }
        if (!seen_paths.insert(entry.path).second) {
            out.error_message = "Duplicate entry path in CKARV01 manifest: " + entry.path;
            return out;
        }
        if (entry.kind == FilesVolumeArchiveEntryKind::Symlink &&
            !validate_ordinary_archive_link_target(entry.path, entry.link_target)) {
            out.error_message = "Symlink target escapes restore root: " + entry.path;
            return out;
        }

        if (total_logical + entry.logical_size > limits.max_total_logical_bytes) {
            out.error_message = "Archive total logical size exceeds restore safety limit.";
            return out;
        }
        total_logical += entry.logical_size;

        std::uint64_t prev_end = 0;
        std::uint64_t covered_bytes = 0;
        for (std::uint64_t e = 0; e < extent_count; ++e) {
            VolumeArchiveInspectedExtent ext;
            if (!read_le(manifest, moff, ext.source_offset) ||
                !read_le(manifest, moff, ext.archive_offset) ||
                !read_le(manifest, moff, ext.length)) {
                out.error_message = "Truncated extent record in CKARV01 manifest.";
                return out;
            }
            if (ext.length == 0 || ext.source_offset < prev_end ||
                ext.source_offset > entry.logical_size ||
                ext.length > entry.logical_size - ext.source_offset ||
                ext.archive_offset > data_size ||
                ext.length > data_size - ext.archive_offset) {
                out.error_message = "Corrupt or out-of-bounds extent in CKARV01 manifest.";
                return out;
            }
            prev_end = ext.source_offset + ext.length;
            covered_bytes += ext.length;
            entry.extents.push_back(ext);
        }

        if (entry.kind == FilesVolumeArchiveEntryKind::RegularFile &&
            covered_bytes < entry.logical_size) {
            entry.sparse = true;
            ++out.sparse_file_count;
        }
        switch (entry.kind) {
            case FilesVolumeArchiveEntryKind::Directory: ++out.directory_count; break;
            case FilesVolumeArchiveEntryKind::RegularFile: ++out.regular_file_count; break;
            case FilesVolumeArchiveEntryKind::Symlink: ++out.symlink_count; break;
            case FilesVolumeArchiveEntryKind::HardLink: ++out.hard_link_count; break;
            case FilesVolumeArchiveEntryKind::Unsupported: ++out.unsupported_count; break;
        }
        out.entries.push_back(std::move(entry));
    }

    std::uint64_t loss_count = 0;
    if (!read_le(manifest, moff, loss_count) || loss_count > limits.max_entries) {
        out.error_message = "Truncated or invalid loss count in CKARV01 manifest.";
        return out;
    }
    for (std::uint64_t l = 0; l < loss_count; ++l) {
        FilesVolumeArchiveLoss loss;
        std::uint64_t entry_idx = 0;
        std::uint8_t lkind = 0;
        std::uint8_t hard = 0;
        if (!read_le(manifest, moff, entry_idx) ||
            !read_le(manifest, moff, lkind) ||
            !read_le(manifest, moff, hard) ||
            lkind < static_cast<std::uint8_t>(FilesVolumeArchiveLossKind::UnsupportedObject) ||
            lkind > static_cast<std::uint8_t>(FilesVolumeArchiveLossKind::Data) ||
            moff + 6 > manifest.size()) {
            out.error_message = "Truncated loss record in CKARV01 manifest.";
            return out;
        }
        moff += 6;
        loss.entry_index = static_cast<std::size_t>(entry_idx);
        loss.kind = static_cast<FilesVolumeArchiveLossKind>(lkind);
        loss.hard_failure = (hard != 0);
        if (!read_str(manifest, moff, loss.path, 65536) ||
            !read_str(manifest, moff, loss.diagnostic, 65536)) {
            out.error_message = "Truncated loss strings in CKARV01 manifest.";
            return out;
        }
        out.losses.push_back(std::move(loss));
    }

    if (moff != manifest.size()) {
        out.error_message = "Trailing unexpected bytes in CKARV01 manifest.";
        return out;
    }
    out.valid = true;
    return out;
}

VolumeArchiveRestorePlan plan_volume_archive_restore(
    const ck::files::NativeAbsolutePath &archive_path,
    const ck::files::NativeAbsolutePath &destination_directory,
    const VolumeArchiveRestoreLimits &limits,
    std::stop_token stop_token) {
    VolumeArchiveRestorePlan plan;
    plan.archive_path = archive_path;
    plan.destination_directory = destination_directory;

    std::string reason;
    if (!validate_volume_archive_restore_destination(destination_directory, &reason)) {
        plan.error_message = reason;
        return plan;
    }

    plan.inspection = inspect_volume_archive_file(archive_path, limits, stop_token);
    if (!plan.inspection.valid) {
        plan.error_message = plan.inspection.error_message;
        return plan;
    }

    ck::files::NativeFileSystem fs;
    auto dst_dir = fs.open_native_directory(destination_directory);
    if (!dst_dir.value()) {
        plan.error_message = "Restore destination directory does not exist or cannot be opened.";
        return plan;
    }

    // Check top-level conflicts without mutating anything.
    for (const auto &entry : plan.inspection.entries) {
        if (entry.path == ".")
            continue;
        std::vector<ck::files::NativeName> parts;
        if (!split_validated_relative(entry.path, destination_directory.encoding(), parts))
            continue;
        ck::files::NativeAbsolutePath cur = destination_directory;
        bool exists = true;
        for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
            auto next = cur.child_address(parts[i]);
            if (!next.value() || !fs.open_native_directory(*next.value()).value()) {
                exists = false;
                break;
            }
            cur = *next.value();
        }
        if (exists) {
            auto pdir = fs.open_native_directory(cur);
            if (pdir.value()) {
                auto probe = (*pdir.value())->open_child(
                    parts.back(), ck::files::EntryAccess::MetadataOnly);
                if (probe.value())
                    ++plan.existing_conflicts;
            }
        }
    }

    plan.valid = true;
    return plan;
}

VolumeArchiveRestoreResult restore_volume_archive(
    const VolumeArchiveRestorePlan &plan,
    bool allow_overwrite,
    std::stop_token stop_token) {
    VolumeArchiveRestoreResult result;
    if (!plan.valid || !plan.inspection.valid ||
        !plan.archive_path || !plan.destination_directory) {
        result.error_message = plan.error_message.empty()
            ? "Invalid volume archive restore plan."
            : plan.error_message;
        return result;
    }
    std::string reason;
    if (!validate_volume_archive_restore_destination(*plan.destination_directory, &reason)) {
        result.error_message = reason;
        return result;
    }

    auto arc_split = detail::split_native_path(*plan.archive_path);
    if (!arc_split) {
        result.error_message = "Unable to split volume archive path.";
        return result;
    }
    ck::files::NativeFileSystem fs;
    auto arc_dir = fs.open_native_directory(arc_split->parent);
    if (!arc_dir.value()) {
        result.error_message = "Unable to open volume archive parent directory.";
        return result;
    }
    auto arc_file = (*arc_dir.value())->open_child(
        arc_split->leaf, ck::files::EntryAccess::ContentRead);
    if (!arc_file.value()) {
        result.error_message = "Unable to open volume archive file.";
        return result;
    }

    std::map<std::uint64_t, ck::files::NativeAbsolutePath> hard_link_targets;

    for (const auto &entry : plan.inspection.entries) {
        if (stop_token.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        if (entry.path == ".")
            continue;
        std::vector<ck::files::NativeName> parts;
        if (!split_validated_relative(entry.path, plan.destination_directory->encoding(), parts)) {
            result.error_message = "Invalid restore path: " + entry.path;
            return result;
        }

        // Ensure parent directories exist.
        ck::files::NativeAbsolutePath cur = *plan.destination_directory;
        const std::size_t dir_depth =
            entry.kind == FilesVolumeArchiveEntryKind::Directory ? parts.size() : parts.size() - 1;
        for (std::size_t i = 0; i < dir_depth; ++i) {
            auto dir = fs.open_native_directory(cur);
            if (!dir.value()) {
                result.error_message = "Failed opening parent directory during restore.";
                return result;
            }
            (void)(*dir.value())->create_child_directory(parts[i]);
            auto next = cur.child_address(parts[i]);
            if (!next.value()) {
                result.error_message = "Failed forming child directory path during restore.";
                return result;
            }
            cur = *next.value();
        }
        if (entry.kind == FilesVolumeArchiveEntryKind::Directory) {
            ++result.restored_directories;
            continue;
        }
        if (entry.kind == FilesVolumeArchiveEntryKind::Unsupported) {
            ++result.skipped_unsupported;
            continue;
        }

        auto parent_dir = fs.open_native_directory(cur);
        if (!parent_dir.value()) {
            result.error_message = "Failed opening destination parent directory.";
            return result;
        }
        auto target_path_obs = cur.child_address(parts.back());
        if (!target_path_obs.value()) {
            result.error_message = "Failed forming destination file path.";
            return result;
        }

        if (entry.kind == FilesVolumeArchiveEntryKind::HardLink) {
            auto it = hard_link_targets.find(entry.hard_link_group);
            if (it == hard_link_targets.end()) {
                ++result.skipped_unsupported;
                continue;
            }
            auto src_split = detail::split_native_path(it->second);
            if (!src_split) {
                ++result.skipped_unsupported;
                continue;
            }
            auto src_dir = fs.open_native_directory(src_split->parent);
            if (!src_dir.value()) {
                ++result.skipped_unsupported;
                continue;
            }
            auto src_id = detail::inspect_child_identity(**src_dir.value(), src_split->leaf);
            if (!src_id) {
                ++result.skipped_unsupported;
                continue;
            }
            auto linked = (*src_dir.value())->create_hard_link(
                src_split->leaf, *src_id, **parent_dir.value(), parts.back());
            if (linked.value())
                ++result.restored_hard_links;
            else
                ++result.skipped_conflicts;
            continue;
        }

        if (entry.kind == FilesVolumeArchiveEntryKind::Symlink) {
            if (plan.destination_directory->encoding() == ck::files::NameEncoding::PosixBytes) {
                ck::files::SymbolicLinkTarget sym_target{entry.link_target};
                if ((*parent_dir.value())->create_symbolic_link(sym_target, parts.back()).value())
                    ++result.restored_symlinks;
                else
                    ++result.skipped_unsupported;
            } else {
                ++result.skipped_unsupported;
            }
            continue;
        }

        // RegularFile restore: stage first, verify readback SHA-256, then publish atomically
        // so an existing target is never lost if restore fails or is cancelled mid-write.
        const auto existing_id = detail::inspect_child_identity(**parent_dir.value(), parts.back());
        if (existing_id && !allow_overwrite) {
            ++result.skipped_conflicts;
            continue;
        }

        const std::uint64_t nonce = detail::next_stage_nonce();
        const std::string suffix = "." + std::to_string(nonce);
        const std::string base_leaf = detail::truncate_utf8_boundary(parts.back().display_name(), 120);
        auto stage_name = detail::native_name_from_utf8(
            plan.destination_directory->encoding(), ".ck-restore-stage-" + base_leaf + suffix);
        if (!stage_name) {
            result.error_message = "Failed forming staging filename for restore: " + entry.path;
            return result;
        }

        auto created = (*parent_dir.value())->create_child_file(*stage_name);
        if (!created.value()) {
            result.error_message = "Failed creating staged restored file: " + entry.path;
            return result;
        }
        auto stage_basic = (*created.value())->inspect_basic();
        if (!stage_basic.value() || !stage_basic.value()->identity.value()) {
            result.error_message = "Failed inspecting staged file identity: " + entry.path;
            return result;
        }
        const ck::files::ObjectIdentity stage_id = *stage_basic.value()->identity.value();
        const auto cleanup_stage = [&] {
            if (created.value()) {
                (void)(*created.value())->finish_content_writes();
                std::move(created).take_value().reset();
            }
            (void)(*parent_dir.value())->retire_child(*stage_name, stage_id);
        };

        ck::files::Sha256 expected_hash;
        std::uint64_t hash_cursor = 0;
        std::vector<std::byte> buf(64 * 1024);
        std::array<std::byte, 4096> zeros{};

        const auto hash_zero_gap = [&](std::uint64_t count) {
            while (count > 0) {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(zeros.size(), count));
                expected_hash.update(std::span<const std::byte>(zeros.data(), chunk));
                count -= chunk;
            }
        };

        for (const auto &ext : entry.extents) {
            if (ext.source_offset > hash_cursor) {
                hash_zero_gap(ext.source_offset - hash_cursor);
                hash_cursor = ext.source_offset;
            }
            std::uint64_t remaining = ext.length;
            std::uint64_t arc_off = plan.inspection.data_offset + ext.archive_offset;
            std::uint64_t dst_off = ext.source_offset;
            while (remaining > 0) {
                if (stop_token.stop_requested()) {
                    cleanup_stage();
                    result.cancelled = true;
                    return result;
                }
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(buf.size(), remaining));
                if (!read_exact_at(**arc_file.value(), arc_off,
                                   std::span<std::byte>(buf.data(), chunk), stop_token)) {
                    cleanup_stage();
                    result.error_message = "Failed reading archive extent for: " + entry.path;
                    return result;
                }
                auto wrote = (*created.value())->write_content(
                    dst_off, std::span<const std::byte>(buf.data(), chunk),
                    ck::files::ContentWriteLimits{}, stop_token);
                if (!wrote.value() || *wrote.value() != chunk) {
                    cleanup_stage();
                    result.error_message = "Failed writing restored extent for: " + entry.path;
                    return result;
                }
                expected_hash.update(std::span<const std::byte>(buf.data(), chunk));
                arc_off += chunk;
                dst_off += chunk;
                remaining -= chunk;
                hash_cursor += chunk;
            }
        }

        // Preserve trailing sparse hole / logical size if file ends with a zero tail.
        if (entry.logical_size > hash_cursor) {
            hash_zero_gap(entry.logical_size - hash_cursor);
            std::array<std::byte, 1> tail_zero{std::byte{0}};
            auto ext_w = (*created.value())->write_content(
                entry.logical_size - 1, tail_zero, ck::files::ContentWriteLimits{}, stop_token);
            if (!ext_w.value() || *ext_w.value() != 1) {
                cleanup_stage();
                result.error_message = "Failed extending sparse logical size for: " + entry.path;
                return result;
            }
        }

        if (!(*created.value())->flush_content().value() ||
            !(*created.value())->finish_content_writes().value()) {
            cleanup_stage();
            result.error_message = "Failed flushing restored file: " + entry.path;
            return result;
        }
        std::move(created).take_value().reset();

        // Independent readback verification of staged file bytes BEFORE publishing.
        auto verify_open = (*parent_dir.value())->open_child(
            *stage_name, ck::files::EntryAccess::ContentRead);
        if (!verify_open.value()) {
            (void)(*parent_dir.value())->retire_child(*stage_name, stage_id);
            result.error_message = "Failed reopening staged file for verification: " + entry.path;
            return result;
        }
        ck::files::Sha256 actual_hash;
        std::uint64_t read_off = 0;
        for (;;) {
            auto got = (*verify_open.value())->read_content(
                ck::files::ContentReadLimits{64 * 1024}, read_off, stop_token);
            if (!got.value()) {
                std::move(verify_open).take_value().reset();
                (void)(*parent_dir.value())->retire_child(*stage_name, stage_id);
                result.error_message = "Failed reading back staged restored file: " + entry.path;
                return result;
            }
            if (got.value()->bytes.empty())
                break;
            actual_hash.update(got.value()->bytes);
            read_off += got.value()->bytes.size();
        }
        std::move(verify_open).take_value().reset();
        if (read_off != entry.logical_size || actual_hash.digest() != expected_hash.digest()) {
            (void)(*parent_dir.value())->retire_child(*stage_name, stage_id);
            result.error_message = "Restored file verification mismatch: " + entry.path;
            return result;
        }

        // Publish verified staged file atomically (preserving existing target on publish failure).
        if (!existing_id) {
            if (!(*parent_dir.value())->rename_child_noreplace(
                    *stage_name, stage_id, **parent_dir.value(), parts.back()).value()) {
                (void)(*parent_dir.value())->retire_child(*stage_name, stage_id);
                result.error_message = "Failed publishing restored file: " + entry.path;
                return result;
            }
        } else {
            auto bak_name = detail::native_name_from_utf8(
                plan.destination_directory->encoding(), ".ck-restore-bak-" + base_leaf + suffix);
            if (!bak_name ||
                !(*parent_dir.value())->rename_child_noreplace(
                    parts.back(), *existing_id, **parent_dir.value(), *bak_name).value()) {
                (void)(*parent_dir.value())->retire_child(*stage_name, stage_id);
                result.error_message = "Failed moving existing file aside during restore: " + entry.path;
                return result;
            }
            if (!(*parent_dir.value())->rename_child_noreplace(
                    *stage_name, stage_id, **parent_dir.value(), parts.back()).value()) {
                (void)(*parent_dir.value())->rename_child_noreplace(
                    *bak_name, *existing_id, **parent_dir.value(), parts.back());
                (void)(*parent_dir.value())->retire_child(*stage_name, stage_id);
                result.error_message = "Failed publishing replacement restored file (original preserved): " + entry.path;
                return result;
            }
            (void)(*parent_dir.value())->retire_child(*bak_name, *existing_id);
        }

        if (entry.hard_link_group != 0)
            hard_link_targets.emplace(entry.hard_link_group, *target_path_obs.value());
        ++result.restored_files;
        if (entry.sparse)
            ++result.restored_sparse_files;
        result.verified_bytes += entry.logical_size;
    }

    result.ok = true;
    return result;
}

} // namespace ck::vision
