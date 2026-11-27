// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "native_name.hpp"
#include "native_path.hpp"
#include "observation.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ck::vision {

enum class FilesCompareMode : std::uint8_t {
    QuickSizeAndTime = 0,
    ThoroughContent = 1,
};

enum class FilesSyncDirection : std::uint8_t {
    CompareOnly = 0,
    OneWayLeftToRight = 1,
    OneWayRightToLeft = 2,
    TwoWayCopyOnly = 3,
};

enum class FilesCompareRelation : std::uint8_t {
    Identical = 0,
    LeftOnly = 1,
    RightOnly = 2,
    LeftNewer = 3,
    RightNewer = 4,
    EqualSizeContentDiffers = 5,
    TypeMismatch = 6,
    Conflict = 7,
};

enum class FilesSyncAction : std::uint8_t {
    None = 0,
    CopyLeftToRight = 1,
    CopyRightToLeft = 2,
    CreateDirRight = 3,
    CreateDirLeft = 4,
    DeleteRight = 5,
    DeleteLeft = 6,
    ReviewConflict = 7,
};

struct FilesSyncOptions final {
    FilesCompareMode compare_mode = FilesCompareMode::QuickSizeAndTime;
    FilesSyncDirection direction = FilesSyncDirection::CompareOnly;
    bool recursive = true;
    bool include_hidden = true;
    // Two-way mode never infers deletions without an explicit durable baseline.
    bool allow_one_way_deletions = false;
    bool case_insensitive_collision_check = true;
    // Timestamp tolerance in nanoseconds (e.g., 2,000,000,000 ns for FAT/clock skew).
    std::uint64_t timestamp_quantum_ns = 0;
    std::size_t max_depth = 64;
    std::size_t max_entries = 50000;
};

struct FilesSyncItem final {
    std::string relative_key;
    std::vector<ck::files::NativeName> relative_components;
    bool left_present = false;
    bool right_present = false;
    ck::files::ObjectKind left_kind = ck::files::ObjectKind::Other;
    ck::files::ObjectKind right_kind = ck::files::ObjectKind::Other;
    std::uint64_t left_size = 0;
    std::uint64_t right_size = 0;
    std::optional<ck::files::FileTimestamp> left_modified;
    std::optional<ck::files::FileTimestamp> right_modified;
    std::optional<ck::files::ObjectIdentity> left_identity;
    std::optional<ck::files::ObjectIdentity> right_identity;
    FilesCompareRelation relation = FilesCompareRelation::Identical;
    FilesSyncAction proposed_action = FilesSyncAction::None;
    std::string diagnostic;
};

struct FilesSyncPlan final {
    ck::files::AdapterScope adapter_scope;
    std::optional<ck::files::NativeAbsolutePath> left_root;
    std::optional<ck::files::NativeAbsolutePath> right_root;
    FilesSyncOptions options;
    std::vector<FilesSyncItem> items;
    std::size_t identical_count = 0;
    std::size_t action_count = 0;
    std::size_t conflict_count = 0;
    bool has_case_collisions = false;
    bool truncated = false;
    bool cancelled = false;
    std::string error_message;
};

struct FilesSyncExecutionSummary final {
    std::size_t completed = 0;
    std::size_t skipped = 0;
    std::size_t conflicts_preserved = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    std::vector<std::string> diagnostics;
};

// Compares two directories natively without following symlinks and produces
// a reviewed synchronization plan.
FilesSyncPlan build_files_sync_plan(const ck::files::NativeAbsolutePath &left_root,
                                    const ck::files::NativeAbsolutePath &right_root,
                                    const FilesSyncOptions &options,
                                    std::stop_token stop_token = {});

// Executes only the explicitly planned non-conflict actions in `plan`,
// revalidating source/destination identities before each write.
FilesSyncExecutionSummary execute_files_sync_plan(const FilesSyncPlan &plan,
                                                  std::stop_token stop_token = {});

enum class FilesDiffLineKind : std::uint8_t {
    Context = 0,
    Removed = 1,
    Added = 2,
};

struct FilesDiffLine final {
    FilesDiffLineKind kind = FilesDiffLineKind::Context;
    std::size_t left_line = 0;
    std::size_t right_line = 0;
    std::string text;
};

struct FilesBinaryDiffEntry final {
    std::uint64_t offset = 0;
    std::optional<std::uint8_t> left_byte;
    std::optional<std::uint8_t> right_byte;
};

struct FilesDiffLimits final {
    std::size_t max_file_bytes = 4 * 1024 * 1024;
    std::size_t max_text_lines = 8000;
    std::size_t max_binary_diffs = 512;
    std::size_t context_lines = 3;
};

struct FilesDiffResult final {
    bool identical = false;
    bool binary = false;
    bool truncated = false;
    std::uint64_t left_size = 0;
    std::uint64_t right_size = 0;
    std::vector<FilesDiffLine> text_lines;
    std::vector<FilesBinaryDiffEntry> binary_diffs;
    std::string error_message;
};

FilesDiffResult compute_text_diff(std::string_view left_text,
                                  std::string_view right_text,
                                  const FilesDiffLimits &limits = {});

FilesDiffResult compute_binary_diff(std::span<const std::byte> left_bytes,
                                    std::span<const std::byte> right_bytes,
                                    const FilesDiffLimits &limits = {});

FilesDiffResult diff_native_files(const ck::files::NativeAbsolutePath &left_path,
                                  const ck::files::NativeAbsolutePath &right_path,
                                  const FilesDiffLimits &limits = {},
                                  std::stop_token stop_token = {});

} // namespace ck::vision
