// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_path.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ck::vision {

enum class OrdinaryArchiveFormat : std::uint8_t {
    Unknown = 0,
    Zip = 1,
    Tar = 2,
    TarGz = 3,
    VolumeArchiveCkArv01 = 4, // Distinct format: never treated as ordinary ZIP/TAR.
};

enum class OrdinaryArchiveEntryKind : std::uint8_t {
    RegularFile = 0,
    Directory = 1,
    Symlink = 2,
    HardLink = 3,
    Unsupported = 4,
};

struct OrdinaryArchiveLimits final {
    std::size_t max_entries = 50000;
    std::uint64_t max_entry_bytes = 256ULL * 1024ULL * 1024ULL;
    std::uint64_t max_total_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t max_archive_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint32_t max_compression_ratio = 200;
    std::size_t max_nesting_depth = 4;
};

struct OrdinaryArchiveEntry final {
    std::size_t index = 0;
    std::string path;
    OrdinaryArchiveEntryKind kind = OrdinaryArchiveEntryKind::RegularFile;
    std::uint64_t uncompressed_size = 0;
    std::uint64_t compressed_size = 0;
    std::uint32_t crc32 = 0;
    std::uint16_t compression_method = 0; // 0 = stored, 8 = deflate
    std::string link_target;
    bool encrypted = false;
    bool safe_path = true;
    std::string safety_warning;
};

struct OrdinaryArchiveCatalog final {
    OrdinaryArchiveFormat format = OrdinaryArchiveFormat::Unknown;
    std::vector<OrdinaryArchiveEntry> entries;
    std::uint64_t total_uncompressed_bytes = 0;
    bool has_unsafe_entries = false;
    bool has_collisions = false;
    std::string error_message;
};

struct OrdinaryArchiveExtractResult final {
    std::size_t extracted_files = 0;
    std::size_t extracted_directories = 0;
    std::size_t skipped_conflicts = 0;
    std::size_t rejected_unsafe = 0;
    bool cancelled = false;
    std::string error_message;
};

struct OrdinaryArchiveInputItem final {
    std::string archive_path;
    std::optional<ck::files::NativeAbsolutePath> source_path;
    bool is_directory = false;
};

struct OrdinaryArchiveMutationResult final {
    bool ok = false;
    std::size_t entries_written = 0;
    std::uint64_t archive_bytes = 0;
    bool cancelled = false;
    std::string error_message;
};

// Pure security validation for archive member paths and link targets.
bool validate_ordinary_archive_entry_path(std::string_view entry_path,
                                          std::string *reason_out = nullptr);
bool validate_ordinary_archive_link_target(std::string_view entry_path,
                                           std::string_view link_target,
                                           std::string *reason_out = nullptr);

// CRC-32 (ISO 3309 / PKZIP).
std::uint32_t ordinary_archive_crc32(std::span<const std::byte> data) noexcept;

// Detects format from magic bytes and filename; explicitly separates CKARV01.
OrdinaryArchiveFormat detect_ordinary_archive_format(
    std::string_view filename_hint,
    std::span<const std::byte> magic_prefix = {}) noexcept;

// Lists entries in a ZIP, TAR, or TAR.GZ archive with full security checks.
OrdinaryArchiveCatalog list_ordinary_archive(
    const ck::files::NativeAbsolutePath &archive_path,
    const OrdinaryArchiveLimits &limits = {},
    std::stop_token stop_token = {});

// Reads one regular-file entry's bytes from the archive, verifying CRC-32 & bomb limits.
std::optional<std::vector<std::byte>> read_ordinary_archive_entry(
    const ck::files::NativeAbsolutePath &archive_path,
    std::size_t entry_index,
    std::string *error_out = nullptr,
    const OrdinaryArchiveLimits &limits = {},
    std::stop_token stop_token = {});

// Extracts all entries (or `selected_indices` if non-empty) to `destination_dir`.
// Refuses unsafe paths, escapes, collisions, and archive bombs.
OrdinaryArchiveExtractResult extract_ordinary_archive(
    const ck::files::NativeAbsolutePath &archive_path,
    const ck::files::NativeAbsolutePath &destination_dir,
    std::span<const std::size_t> selected_indices = {},
    bool allow_overwrite = false,
    const OrdinaryArchiveLimits &limits = {},
    std::stop_token stop_token = {});

// Creates a new ZIP, TAR, or TAR.GZ archive via staged write + fsync + atomic publish.
OrdinaryArchiveMutationResult create_ordinary_archive(
    const ck::files::NativeAbsolutePath &archive_path,
    OrdinaryArchiveFormat format,
    std::span<const OrdinaryArchiveInputItem> items,
    bool compress_zip_deflate = true,
    const OrdinaryArchiveLimits &limits = {},
    std::stop_token stop_token = {});

// Updates an existing archive (removing `remove_indices` and adding/replacing `add_items`)
// by writing a verified staging copy and replacing atomically, preserving the old archive
// until commit.
OrdinaryArchiveMutationResult modify_ordinary_archive(
    const ck::files::NativeAbsolutePath &archive_path,
    std::span<const OrdinaryArchiveInputItem> add_items,
    std::span<const std::size_t> remove_indices,
    const OrdinaryArchiveLimits &limits = {},
    std::stop_token stop_token = {});

} // namespace ck::vision
