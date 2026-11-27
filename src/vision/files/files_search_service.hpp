// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "native_name.hpp"
#include "native_path.hpp"
#include "observation.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ck::vision {

enum class FilesSearchNameMode : std::uint8_t {
    None = 0,
    Substring = 1,
    Glob = 2,
    Regex = 3,
};

struct FilesSearchQuery final {
    std::optional<ck::files::NativeAbsolutePath> root;
    std::string name_pattern;
    FilesSearchNameMode name_mode = FilesSearchNameMode::Glob;
    std::string content_query;
    bool case_sensitive = false;
    bool include_hidden = true;
    bool skip_binary_content = true;
    std::optional<std::uint64_t> min_size;
    std::optional<std::uint64_t> max_size;
    std::size_t max_depth = 64;
    std::size_t max_results = 20000;
    std::uint64_t max_content_file_bytes = 8ULL * 1024ULL * 1024ULL;
};

struct FilesSearchHit final {
    std::optional<ck::files::NativeAbsolutePath> full_path;
    std::optional<ck::files::NativeAbsolutePath> parent_directory;
    std::optional<ck::files::NativeName> entry_name;
    std::string relative_display;
    ck::files::ObjectKind kind = ck::files::ObjectKind::Other;
    std::uint64_t logical_size = 0;
    std::optional<ck::files::FileTimestamp> modified;
    std::optional<std::size_t> content_match_line;
    std::string content_match_preview;
};

struct FilesSearchResult final {
    std::vector<FilesSearchHit> hits;
    std::uint64_t scanned_entries = 0;
    std::uint64_t scanned_directories = 0;
    std::uint64_t denied_entries = 0;
    bool truncated = false;
    bool cancelled = false;
    std::string error_message;
};

// Pure helper for matching names in Glob/Substring/Regex modes.
bool match_search_name(std::string_view candidate, std::string_view pattern,
                       FilesSearchNameMode mode, bool case_sensitive,
                       std::string *error_out = nullptr);

// Synchronous bounded handle-relative recursive search (no symlink traversal).
FilesSearchResult run_files_search(const FilesSearchQuery &query,
                                   std::stop_token stop_token = {});

// Search-within-results: refines an existing result set by name/size/content
// without re-walking the filesystem tree.
FilesSearchResult filter_files_search_results(const FilesSearchResult &previous,
                                              const FilesSearchQuery &refinement,
                                              std::stop_token stop_token = {});

class FilesSearchService final {
  public:
    explicit FilesSearchService(std::function<void()> wake = {});
    ~FilesSearchService();
    FilesSearchService(const FilesSearchService &) = delete;
    FilesSearchService &operator=(const FilesSearchService &) = delete;

    std::uint64_t start(FilesSearchQuery query);
    std::uint64_t start_refine(FilesSearchResult previous, FilesSearchQuery refinement);
    void cancel() noexcept;
    bool working() const;
    std::optional<FilesSearchResult> take();
    void request_stop() noexcept;
    void join();

  private:
    struct State;
    std::unique_ptr<State> state_;
};

struct FilesDirectoryTreeNode final {
    std::optional<ck::files::NativeAbsolutePath> path;
    std::string display_name;
    std::size_t depth = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t allocated_bytes = 0;
    std::size_t file_count = 0;
    std::size_t directory_count = 0;
};

struct FilesDirectoryTreeSnapshot final {
    std::optional<ck::files::NativeAbsolutePath> root;
    std::vector<FilesDirectoryTreeNode> nodes;
    bool truncated = false;
    bool cancelled = false;
    std::string error_message;
};

// Builds a bounded navigable directory tree with sizes by reusing ck_du_core
// in-process (never shelling out to ck-du).
FilesDirectoryTreeSnapshot build_files_directory_tree(
    const ck::files::NativeAbsolutePath &root,
    std::size_t max_nodes = 4096,
    std::size_t max_depth = 16,
    std::stop_token stop_token = {});

class FilesDirectoryTreeService final {
  public:
    explicit FilesDirectoryTreeService(std::function<void()> wake = {});
    ~FilesDirectoryTreeService();
    FilesDirectoryTreeService(const FilesDirectoryTreeService &) = delete;
    FilesDirectoryTreeService &operator=(const FilesDirectoryTreeService &) = delete;

    std::uint64_t start(ck::files::NativeAbsolutePath root,
                        std::size_t max_nodes = 4096,
                        std::size_t max_depth = 16);
    void cancel() noexcept;
    bool working() const;
    std::optional<FilesDirectoryTreeSnapshot> take();
    void request_stop() noexcept;
    void join();

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ck::vision
