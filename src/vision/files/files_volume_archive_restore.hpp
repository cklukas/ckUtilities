// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_volume_archive.hpp"
#include "native_path.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ck::vision {

struct VolumeArchiveRestoreLimits final {
    std::uint64_t max_manifest_bytes = 64ULL * 1024ULL * 1024ULL;
    std::size_t max_entries = 100000;
    std::uint64_t max_total_logical_bytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
};

struct VolumeArchiveInspectedExtent final {
    std::uint64_t source_offset = 0;
    std::uint64_t archive_offset = 0;
    std::uint64_t length = 0;
};

struct VolumeArchiveInspectedEntry final {
    FilesVolumeArchiveEntryKind kind = FilesVolumeArchiveEntryKind::Directory;
    std::string path;
    std::string link_target;
    std::uint64_t logical_size = 0;
    std::uint64_t hard_link_group = 0;
    std::vector<VolumeArchiveInspectedExtent> extents;
    bool sparse = false;
};

struct VolumeArchiveInspection final {
    bool valid = false;
    std::uint32_t format_version = 0;
    FilesVolumeArchiveConsistency consistency = FilesVolumeArchiveConsistency::Live;
    ck::files::NameEncoding path_encoding = ck::files::NameEncoding::PosixBytes;
    FilesVolumeRecord source;
    std::uint64_t manifest_size = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t data_size = 0;
    std::uint64_t archive_file_size = 0;
    std::vector<VolumeArchiveInspectedEntry> entries;
    std::vector<FilesVolumeArchiveLoss> losses;
    std::size_t directory_count = 0;
    std::size_t regular_file_count = 0;
    std::size_t sparse_file_count = 0;
    std::size_t symlink_count = 0;
    std::size_t hard_link_count = 0;
    std::size_t unsupported_count = 0;
    std::string error_message;
};

struct VolumeArchiveRestorePlan final {
    bool valid = false;
    std::optional<ck::files::NativeAbsolutePath> archive_path;
    std::optional<ck::files::NativeAbsolutePath> destination_directory;
    VolumeArchiveInspection inspection;
    std::size_t existing_conflicts = 0;
    std::string error_message;
};

struct VolumeArchiveRestoreResult final {
    bool ok = false;
    std::size_t restored_directories = 0;
    std::size_t restored_files = 0;
    std::size_t restored_sparse_files = 0;
    std::size_t restored_hard_links = 0;
    std::size_t restored_symlinks = 0;
    std::size_t skipped_unsupported = 0;
    std::size_t skipped_conflicts = 0;
    std::uint64_t verified_bytes = 0;
    bool cancelled = false;
    std::string error_message;
};

// Rejects raw device paths (/dev/..., \\.\...) and live system roots (/, C:\).
bool validate_volume_archive_restore_destination(
    const ck::files::NativeAbsolutePath &destination_directory,
    std::string *reason_out = nullptr);

// Reads and strictly validates a CKARV01 volume archive header and manifest.
VolumeArchiveInspection inspect_volume_archive_file(
    const ck::files::NativeAbsolutePath &archive_path,
    const VolumeArchiveRestoreLimits &limits = {},
    std::stop_token stop_token = {});

// Validates archive + destination directory and builds a reviewable restore plan.
VolumeArchiveRestorePlan plan_volume_archive_restore(
    const ck::files::NativeAbsolutePath &archive_path,
    const ck::files::NativeAbsolutePath &destination_directory,
    const VolumeArchiveRestoreLimits &limits = {},
    std::stop_token stop_token = {});

// Restores a reviewed CKARV01 volume archive into `plan.destination_directory`,
// preserving sparse extents, verifying restored file bytes, and refusing unsafe
// targets or overwrites unless explicitly allowed.
VolumeArchiveRestoreResult restore_volume_archive(
    const VolumeArchiveRestorePlan &plan,
    bool allow_overwrite = false,
    std::stop_token stop_token = {});

} // namespace ck::vision
