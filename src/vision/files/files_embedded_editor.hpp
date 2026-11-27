// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "native_path.hpp"

#include <cvision/widgets/editor_document.hpp>
#include <cvision/widgets/syntax_profile.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ck::vision {

enum class FilesNewlineStyle : std::uint8_t {
    Lf = 0,
    CrLf = 1,
};

struct FilesDocumentSnapshot final {
    std::optional<ck::files::NativeAbsolutePath> path;
    std::optional<ck::files::ObjectIdentity> identity;
    std::uint64_t logical_size = 0;
    std::uint64_t link_count = 1;
    std::optional<ck::files::FileTimestamp> modified;
    std::array<std::byte, 32> sha256{};
    FilesNewlineStyle newline_style = FilesNewlineStyle::Lf;
    bool existed_at_open = false;
};

struct FilesDocumentOpenResult final {
    bool ok = false;
    std::string text;
    FilesDocumentSnapshot snapshot;
    bool binary_warning = false;
    std::string error_message;
};

enum class FilesSaveStatus : std::uint8_t {
    Saved = 0,
    ExternalConflict = 1,
    TargetExists = 2,
    HardLinkWarning = 3,
    Failed = 4,
    Cancelled = 5,
};

struct FilesDocumentSaveResult final {
    FilesSaveStatus status = FilesSaveStatus::Failed;
    FilesDocumentSnapshot updated_snapshot;
    std::string diagnostic;
};

// Opens a file via native handles, capturing identity, mtime, link count,
// newline style, and SHA-256 digest for conflict-safe saving.
FilesDocumentOpenResult open_files_editor_document(
    const ck::files::NativeAbsolutePath &path,
    std::size_t max_bytes = 16 * 1024 * 1024,
    std::stop_token stop_token = {});

// Revalidates open-time identity/digest and saves atomically via staged write.
// Refuses silent overwrite on external modification unless `force_overwrite_conflict` is true.
FilesDocumentSaveResult save_files_editor_document(
    const FilesDocumentSnapshot &snapshot,
    std::string_view text,
    bool force_overwrite_conflict = false,
    std::stop_token stop_token = {});

// Saves text to a new or explicitly replaced path via native staged write.
FilesDocumentSaveResult save_as_files_editor_document(
    const ck::files::NativeAbsolutePath &target_path,
    std::string_view text,
    FilesNewlineStyle newline_style = FilesNewlineStyle::Lf,
    bool allow_replace = false,
    std::stop_token stop_token = {});

// Creates a shared SyntaxProfileRegistry with ckVision standard profiles + ck-edit Markdown.
std::unique_ptr<ckv::widgets::SyntaxProfileRegistry> make_files_syntax_registry();

enum class FilesViewerMode : std::uint8_t {
    Text = 0,
    Hex = 1,
    Json = 2,
};

struct FilesViewerPage final {
    bool ok = false;
    std::uint64_t file_size = 0;
    std::uint64_t window_offset = 0;
    std::size_t window_bytes = 0;
    bool has_more = false;
    std::vector<std::string> lines;
    std::string error_message;
};

// Streams a bounded window starting at `offset` and formats it as Text, Hex, or JSON.
FilesViewerPage read_files_viewer_page(
    const ck::files::NativeAbsolutePath &path,
    FilesViewerMode mode,
    std::uint64_t offset = 0,
    std::size_t window_bytes = 64 * 1024,
    std::stop_token stop_token = {});

// Streams through a file starting at `start_offset` to find `needle` (bounded).
std::optional<std::uint64_t> search_files_viewer_stream(
    const ck::files::NativeAbsolutePath &path,
    std::string_view needle,
    std::uint64_t start_offset = 0,
    bool case_sensitive = false,
    std::uint64_t max_scan_bytes = 64ULL * 1024ULL * 1024ULL,
    std::stop_token stop_token = {});

} // namespace ck::vision
