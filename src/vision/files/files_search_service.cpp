// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_search_service.hpp"

#include "directory_cursor.hpp"
#include "disk_usage_core.hpp"
#include "native_filesystem.hpp"
#include "opened_entry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <regex>
#include <thread>
#include <utility>

namespace ck::vision {
namespace {

unsigned char ascii_lower(unsigned char ch) noexcept {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<unsigned char>(ch - 'A' + 'a') : ch;
}

bool ascii_equal(char lhs, char rhs, bool case_sensitive) noexcept {
    if (case_sensitive)
        return lhs == rhs;
    return ascii_lower(static_cast<unsigned char>(lhs)) ==
           ascii_lower(static_cast<unsigned char>(rhs));
}

bool glob_match_impl(std::string_view text, std::string_view pattern, bool case_sensitive) {
    std::size_t ti = 0;
    std::size_t pi = 0;
    std::size_t star_pi = std::string_view::npos;
    std::size_t star_ti = 0;
    while (ti < text.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            star_pi = pi++;
            star_ti = ti;
        } else if (pi < pattern.size() && pattern[pi] == '?') {
            ++pi;
            ++ti;
        } else if (pi < pattern.size() && pattern[pi] == '[') {
            const std::size_t close = pattern.find(']', pi + 1);
            if (close == std::string_view::npos) {
                if (ascii_equal(text[ti], '[', case_sensitive)) {
                    ++pi;
                    ++ti;
                } else if (star_pi != std::string_view::npos) {
                    pi = star_pi + 1;
                    ti = ++star_ti;
                } else {
                    return false;
                }
            } else {
                bool negate = false;
                std::size_t ci = pi + 1;
                if (ci < close && (pattern[ci] == '!' || pattern[ci] == '^')) {
                    negate = true;
                    ++ci;
                }
                bool matched = false;
                const char ch = text[ti];
                while (ci < close) {
                    if (ci + 2 < close && pattern[ci + 1] == '-') {
                        const unsigned char lo = case_sensitive
                            ? static_cast<unsigned char>(pattern[ci])
                            : ascii_lower(static_cast<unsigned char>(pattern[ci]));
                        const unsigned char hi = case_sensitive
                            ? static_cast<unsigned char>(pattern[ci + 2])
                            : ascii_lower(static_cast<unsigned char>(pattern[ci + 2]));
                        const unsigned char tc = case_sensitive
                            ? static_cast<unsigned char>(ch)
                            : ascii_lower(static_cast<unsigned char>(ch));
                        if (tc >= lo && tc <= hi)
                            matched = true;
                        ci += 3;
                    } else {
                        if (ascii_equal(ch, pattern[ci], case_sensitive))
                            matched = true;
                        ++ci;
                    }
                }
                if (negate)
                    matched = !matched;
                if (matched) {
                    pi = close + 1;
                    ++ti;
                } else if (star_pi != std::string_view::npos) {
                    pi = star_pi + 1;
                    ti = ++star_ti;
                } else {
                    return false;
                }
            }
        } else if (pi < pattern.size() && ascii_equal(text[ti], pattern[pi], case_sensitive)) {
            ++pi;
            ++ti;
        } else if (star_pi != std::string_view::npos) {
            pi = star_pi + 1;
            ti = ++star_ti;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*')
        ++pi;
    return pi == pattern.size();
}

bool contains_substring(std::string_view haystack, std::string_view needle,
                        bool case_sensitive, std::size_t *pos_out = nullptr) {
    if (needle.empty()) {
        if (pos_out)
            *pos_out = 0;
        return true;
    }
    if (needle.size() > haystack.size())
        return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool ok = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (!ascii_equal(haystack[i + j], needle[j], case_sensitive)) {
                ok = false;
                break;
            }
        }
        if (ok) {
            if (pos_out)
                *pos_out = i;
            return true;
        }
    }
    return false;
}

std::string sanitize_preview_line(std::string_view text) {
    std::string out;
    out.reserve(std::min<std::size_t>(text.size(), 120));
    for (const char ch : text) {
        if (ch == '\r' || ch == '\n')
            break;
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 0x20U || u == 0x7FU)
            out.push_back(' ');
        else
            out.push_back(ch);
        if (out.size() >= 120)
            break;
    }
    return out;
}

struct ContentMatch final {
    bool matched = false;
    std::size_t line_number = 0;
    std::string preview;
};

ContentMatch search_entry_content(ck::files::OpenedEntry &entry,
                                  std::string_view needle,
                                  bool case_sensitive,
                                  bool skip_binary,
                                  std::uint64_t max_bytes,
                                  std::stop_token stop_token) {
    ContentMatch result;
    if (needle.empty()) {
        result.matched = true;
        return result;
    }
    constexpr std::size_t kChunkSize = 64 * 1024;
    std::string carry;
    std::uint64_t offset = 0;
    std::size_t current_line = 1;
    bool checked_binary = false;

    while (offset < max_bytes) {
        if (stop_token.stop_requested())
            return {};
        const std::size_t to_read = static_cast<std::size_t>(
            std::min<std::uint64_t>(kChunkSize, max_bytes - offset));
        auto read_obs = entry.read_content(
            ck::files::ContentReadLimits{to_read}, offset, stop_token);
        if (!read_obs.value())
            return {};
        const auto &bytes = read_obs.value()->bytes;
        if (bytes.empty())
            break;
        offset += bytes.size();

        std::string_view piece(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        if (skip_binary && !checked_binary) {
            checked_binary = true;
            const std::size_t probe_len = std::min<std::size_t>(bytes.size(), 8192);
            if (std::memchr(bytes.data(), 0, probe_len) != nullptr)
                return {};
        }

        std::string combined;
        combined.reserve(carry.size() + piece.size());
        combined.append(carry);
        combined.append(piece);

        std::size_t match_pos = 0;
        if (contains_substring(combined, needle, case_sensitive, &match_pos)) {
            std::size_t line = current_line;
            for (std::size_t i = 0; i < match_pos; ++i) {
                if (combined[i] == '\n')
                    ++line;
            }
            std::size_t line_start = match_pos;
            while (line_start > 0 && combined[line_start - 1] != '\n')
                --line_start;
            result.matched = true;
            result.line_number = line;
            result.preview = sanitize_preview_line(
                std::string_view(combined).substr(line_start));
            return result;
        }

        const std::size_t keep = needle.size() > 1 ? needle.size() - 1 : 0;
        const std::size_t advance = combined.size() > keep ? combined.size() - keep : 0;
        for (std::size_t i = 0; i < advance; ++i) {
            if (combined[i] == '\n')
                ++current_line;
        }
        carry.assign(combined.data() + advance, combined.size() - advance);
    }
    return {};
}

bool is_hidden_display_name(std::string_view display) noexcept {
    return !display.empty() && display.front() == '.';
}

} // namespace

bool match_search_name(std::string_view candidate, std::string_view pattern,
                       FilesSearchNameMode mode, bool case_sensitive,
                       std::string *error_out) {
    if (mode == FilesSearchNameMode::None || pattern.empty())
        return true;
    switch (mode) {
        case FilesSearchNameMode::None:
            return true;
        case FilesSearchNameMode::Substring:
            return contains_substring(candidate, pattern, case_sensitive);
        case FilesSearchNameMode::Glob:
            return glob_match_impl(candidate, pattern, case_sensitive);
        case FilesSearchNameMode::Regex: {
            try {
                auto flags = std::regex_constants::ECMAScript;
                if (!case_sensitive)
                    flags |= std::regex_constants::icase;
                std::regex re(pattern.data(), pattern.size(), flags);
                return std::regex_search(candidate.begin(), candidate.end(), re);
            } catch (const std::regex_error &ex) {
                if (error_out)
                    *error_out = ex.what();
                return false;
            }
        }
    }
    return false;
}

FilesSearchResult run_files_search(const FilesSearchQuery &query,
                                   std::stop_token stop_token) {
    FilesSearchResult result;
    if (!query.root || !query.root->is_valid()) {
        result.error_message = "Search root is invalid.";
        return result;
    }

    std::optional<std::regex> compiled_regex;
    if (query.name_mode == FilesSearchNameMode::Regex && !query.name_pattern.empty()) {
        try {
            auto flags = std::regex_constants::ECMAScript;
            if (!query.case_sensitive)
                flags |= std::regex_constants::icase;
            compiled_regex.emplace(query.name_pattern, flags);
        } catch (const std::regex_error &ex) {
            result.error_message = std::string("Invalid regular expression: ") + ex.what();
            return result;
        }
    }

    const std::size_t max_depth = std::clamp<std::size_t>(query.max_depth, 1, 256);
    const std::size_t max_results = std::clamp<std::size_t>(query.max_results, 1, 200000);

    ck::files::NativeFileSystem fs;
    auto root_dir = fs.open_native_directory(*query.root);
    if (!root_dir.value()) {
        result.error_message = "Unable to open search root directory.";
        return result;
    }

    struct PendingDir final {
        std::unique_ptr<ck::files::DirectoryCursor> cursor;
        ck::files::NativeAbsolutePath path;
        std::string relative_prefix;
        std::size_t depth = 0;
    };

    std::deque<PendingDir> queue;
    queue.push_back(PendingDir{std::move(root_dir).take_value(), *query.root, {}, 0});

    while (!queue.empty()) {
        if (stop_token.stop_requested()) {
            result.cancelled = true;
            break;
        }
        PendingDir current = std::move(queue.front());
        queue.pop_front();
        ++result.scanned_directories;

        for (;;) {
            if (stop_token.stop_requested()) {
                result.cancelled = true;
                break;
            }
            auto batch = current.cursor->read(256, stop_token);
            if (batch.state == ck::files::DirectoryReadState::Cancelled ||
                stop_token.stop_requested()) {
                result.cancelled = true;
                break;
            }
            if (batch.error) {
                ++result.denied_entries;
                break;
            }
            for (const auto &entry_name : batch.names) {
                if (stop_token.stop_requested()) {
                    result.cancelled = true;
                    break;
                }
                const std::string display = entry_name.display_name();
                if (!query.include_hidden && is_hidden_display_name(display))
                    continue;

                ++result.scanned_entries;
                auto child_path_obs = current.path.child_address(entry_name);
                if (!child_path_obs.value()) {
                    ++result.denied_entries;
                    continue;
                }
                const ck::files::NativeAbsolutePath child_path = *child_path_obs.value();

                auto opened_obs = current.cursor->open_child(
                    entry_name, ck::files::EntryAccess::MetadataOnly);
                if (!opened_obs.value()) {
                    ++result.denied_entries;
                    continue;
                }
                auto opened = std::move(opened_obs).take_value();
                auto basic_obs = opened->inspect_basic();
                if (!basic_obs.value()) {
                    ++result.denied_entries;
                    continue;
                }
                const auto &basic = *basic_obs.value();
                const std::string rel = current.relative_prefix.empty()
                    ? display
                    : current.relative_prefix + "/" + display;

                const auto kind = basic.kind.value() ? *basic.kind.value() : ck::files::ObjectKind::Other;
                // Queue subdirectories (never follow symlinks or reparse points).
                if (kind == ck::files::ObjectKind::Directory &&
                    current.depth + 1 < max_depth) {
                    auto sub_cursor = current.cursor->open_directory_child(entry_name);
                    if (sub_cursor.value()) {
                        queue.push_back(PendingDir{
                            std::move(sub_cursor).take_value(),
                            child_path,
                            rel,
                            current.depth + 1,
                        });
                    } else {
                        ++result.denied_entries;
                    }
                }

                // Name filter.
                bool name_matches = true;
                if (query.name_mode == FilesSearchNameMode::Regex && compiled_regex) {
                    name_matches = std::regex_search(display, *compiled_regex);
                } else if (!query.name_pattern.empty()) {
                    name_matches = match_search_name(
                        display, query.name_pattern, query.name_mode, query.case_sensitive);
                }
                if (!name_matches)
                    continue;

                const std::uint64_t logical_size =
                    basic.logical_size.value() ? *basic.logical_size.value() : 0;
                if (query.min_size && logical_size < *query.min_size)
                    continue;
                if (query.max_size && logical_size > *query.max_size)
                    continue;

                ContentMatch content_match;
                if (!query.content_query.empty()) {
                    if (kind != ck::files::ObjectKind::RegularFile)
                        continue;
                    if (logical_size > query.max_content_file_bytes)
                        continue;
                    auto content_open = current.cursor->open_child(
                        entry_name, ck::files::EntryAccess::ContentRead);
                    if (!content_open.value()) {
                        ++result.denied_entries;
                        continue;
                    }
                    content_match = search_entry_content(
                        **content_open.value(),
                        query.content_query,
                        query.case_sensitive,
                        query.skip_binary_content,
                        query.max_content_file_bytes,
                        stop_token);
                    if (!content_match.matched)
                        continue;
                }

                if (result.hits.size() >= max_results) {
                    result.truncated = true;
                    break;
                }
                FilesSearchHit hit;
                hit.full_path = child_path;
                hit.parent_directory = current.path;
                hit.entry_name = entry_name;
                hit.relative_display = rel;
                hit.kind = kind;
                hit.logical_size = logical_size;
                if (basic.modified.value())
                    hit.modified = *basic.modified.value();
                if (!query.content_query.empty() && content_match.matched) {
                    hit.content_match_line = content_match.line_number;
                    hit.content_match_preview = std::move(content_match.preview);
                }
                result.hits.push_back(std::move(hit));
            }
            if (result.cancelled || result.truncated ||
                batch.state == ck::files::DirectoryReadState::End)
                break;
        }
        if (result.cancelled || result.truncated)
            break;
    }
    return result;
}

FilesSearchResult filter_files_search_results(const FilesSearchResult &previous,
                                              const FilesSearchQuery &refinement,
                                              std::stop_token stop_token) {
    FilesSearchResult result;
    std::optional<std::regex> compiled_regex;
    if (refinement.name_mode == FilesSearchNameMode::Regex &&
        !refinement.name_pattern.empty()) {
        try {
            auto flags = std::regex_constants::ECMAScript;
            if (!refinement.case_sensitive)
                flags |= std::regex_constants::icase;
            compiled_regex.emplace(refinement.name_pattern, flags);
        } catch (const std::regex_error &ex) {
            result.error_message = std::string("Invalid regular expression: ") + ex.what();
            return result;
        }
    }

    ck::files::NativeFileSystem fs;
    for (const auto &hit : previous.hits) {
        if (stop_token.stop_requested()) {
            result.cancelled = true;
            break;
        }
        ++result.scanned_entries;
        if (!hit.entry_name)
            continue;
        const std::string display = hit.entry_name->display_name();
        if (!refinement.include_hidden && is_hidden_display_name(display))
            continue;
        bool name_ok = true;
        if (refinement.name_mode == FilesSearchNameMode::Regex && compiled_regex) {
            name_ok = std::regex_search(display, *compiled_regex);
        } else if (!refinement.name_pattern.empty()) {
            name_ok = match_search_name(
                display, refinement.name_pattern, refinement.name_mode,
                refinement.case_sensitive);
        }
        if (!name_ok)
            continue;
        if (refinement.min_size && hit.logical_size < *refinement.min_size)
            continue;
        if (refinement.max_size && hit.logical_size > *refinement.max_size)
            continue;

        FilesSearchHit copy = hit;
        if (!refinement.content_query.empty()) {
            if (hit.kind != ck::files::ObjectKind::RegularFile ||
                hit.logical_size > refinement.max_content_file_bytes ||
                !hit.parent_directory)
                continue;
            auto parent = fs.open_native_directory(*hit.parent_directory);
            if (!parent.value()) {
                ++result.denied_entries;
                continue;
            }
            auto opened = (*parent.value())->open_child(
                *hit.entry_name, ck::files::EntryAccess::ContentRead);
            if (!opened.value()) {
                ++result.denied_entries;
                continue;
            }
            auto match = search_entry_content(
                **opened.value(),
                refinement.content_query,
                refinement.case_sensitive,
                refinement.skip_binary_content,
                refinement.max_content_file_bytes,
                stop_token);
            if (!match.matched)
                continue;
            copy.content_match_line = match.line_number;
            copy.content_match_preview = std::move(match.preview);
        }
        if (result.hits.size() >= refinement.max_results) {
            result.truncated = true;
            break;
        }
        result.hits.push_back(std::move(copy));
    }
    return result;
}

struct FilesSearchService::State final {
    struct Request final {
        std::uint64_t id = 0;
        FilesSearchQuery query;
        std::optional<FilesSearchResult> refine_source;
    };

    std::function<void()> wake;
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    bool busy = false;
    std::uint64_t next_id = 1;
    std::optional<Request> pending;
    std::optional<FilesSearchResult> completed;
    std::stop_source active_stop;
    std::thread worker;

    explicit State(std::function<void()> cb)
        : wake(std::move(cb)), worker([this] { run(); }) {}

    ~State() {
        request_stop();
        join();
    }

    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        active_stop.request_stop();
        cv.notify_all();
    }

    void join() {
        if (worker.joinable())
            worker.join();
    }

    void run() {
        for (;;) {
            std::optional<Request> job;
            std::stop_token token;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping)
                    return;
                job = std::move(*pending);
                pending.reset();
                busy = true;
                active_stop = std::stop_source{};
                token = active_stop.get_token();
            }
            FilesSearchResult res;
            if (job->refine_source)
                res = filter_files_search_results(*job->refine_source, job->query, token);
            else
                res = run_files_search(job->query, token);
            std::function<void()> notify;
            {
                std::lock_guard lock(mutex);
                busy = false;
                if (!stopping) {
                    completed = std::move(res);
                    notify = wake;
                }
            }
            if (notify) {
                try {
                    notify();
                } catch (...) {
                }
            }
        }
    }
};

FilesSearchService::FilesSearchService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}

FilesSearchService::~FilesSearchService() = default;

std::uint64_t FilesSearchService::start(FilesSearchQuery query) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return 0;
    state_->active_stop.request_stop();
    const std::uint64_t id = state_->next_id++;
    state_->pending = State::Request{id, std::move(query), std::nullopt};
    state_->cv.notify_one();
    return id;
}

std::uint64_t FilesSearchService::start_refine(FilesSearchResult previous,
                                               FilesSearchQuery refinement) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return 0;
    state_->active_stop.request_stop();
    const std::uint64_t id = state_->next_id++;
    state_->pending = State::Request{id, std::move(refinement), std::move(previous)};
    state_->cv.notify_one();
    return id;
}

void FilesSearchService::cancel() noexcept {
    std::lock_guard lock(state_->mutex);
    state_->pending.reset();
    state_->active_stop.request_stop();
}

bool FilesSearchService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

std::optional<FilesSearchResult> FilesSearchService::take() {
    std::lock_guard lock(state_->mutex);
    auto out = std::move(state_->completed);
    state_->completed.reset();
    return out;
}

void FilesSearchService::request_stop() noexcept { state_->request_stop(); }
void FilesSearchService::join() { state_->join(); }

namespace {

void flatten_du_tree(const ck::du::DirectoryNode &node,
                     std::size_t depth,
                     std::size_t max_nodes,
                     std::size_t max_depth,
                     FilesDirectoryTreeSnapshot &out) {
    if (out.nodes.size() >= max_nodes) {
        out.truncated = true;
        return;
    }
    auto native_obs = ck::files::NativeAbsolutePath::from_host(node.path);
    if (native_obs.value()) {
        FilesDirectoryTreeNode item;
        item.path = *native_obs.value();
        item.display_name = node.path.filename().string();
        if (item.display_name.empty())
            item.display_name = node.path.string();
        item.depth = depth;
        item.logical_bytes = static_cast<std::uint64_t>(node.stats.logicalSize);
        item.allocated_bytes = static_cast<std::uint64_t>(node.stats.totalSize);
        item.file_count = node.stats.fileCount;
        item.directory_count = node.stats.directoryCount;
        out.nodes.push_back(std::move(item));
    }
    if (depth >= max_depth)
        return;
    for (const auto &child : node.children) {
        if (!child)
            continue;
        flatten_du_tree(*child, depth + 1, max_nodes, max_depth, out);
        if (out.truncated)
            return;
    }
}

} // namespace

FilesDirectoryTreeSnapshot build_files_directory_tree(
    const ck::files::NativeAbsolutePath &root,
    std::size_t max_nodes,
    std::size_t max_depth,
    std::stop_token stop_token) {
    FilesDirectoryTreeSnapshot snap;
    snap.root = root;
    if (!root.is_valid()) {
        snap.error_message = "Directory tree root is invalid.";
        return snap;
    }
    auto host_obs = root.host_path();
    if (!host_obs.value()) {
        snap.error_message = "Unable to represent directory tree root for disk usage scan.";
        return snap;
    }
    ck::du::BuildDirectoryTreeOptions options;
    options.symlinkPolicy = ck::du::BuildDirectoryTreeOptions::SymlinkPolicy::Never;
    options.reportErrors = false;
    options.cancelRequested = [&stop_token] { return stop_token.stop_requested(); };

    auto built = ck::du::buildDirectoryTree(*host_obs.value(), options);
    snap.cancelled = built.cancelled || stop_token.stop_requested();
    if (!built.root) {
        if (!snap.cancelled)
            snap.error_message = "Directory tree scan produced no root.";
        return snap;
    }
    const std::function<void(ck::du::DirectoryNode &)> sort_desc =
        [&](ck::du::DirectoryNode &node) {
            std::sort(node.children.begin(), node.children.end(),
                      [](const auto &lhs, const auto &rhs) {
                          if (!lhs || !rhs)
                              return static_cast<bool>(lhs);
                          return lhs->stats.totalSize > rhs->stats.totalSize;
                      });
            for (auto &child : node.children) {
                if (child)
                    sort_desc(*child);
            }
        };
    sort_desc(*built.root);
    flatten_du_tree(*built.root, 0, std::max<std::size_t>(1, max_nodes),
                    std::max<std::size_t>(1, max_depth), snap);
    return snap;
}

struct FilesDirectoryTreeService::State final {
    struct Request final {
        std::uint64_t id = 0;
        ck::files::NativeAbsolutePath root;
        std::size_t max_nodes = 4096;
        std::size_t max_depth = 16;
    };

    std::function<void()> wake;
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    bool busy = false;
    std::uint64_t next_id = 1;
    std::optional<Request> pending;
    std::optional<FilesDirectoryTreeSnapshot> completed;
    std::stop_source active_stop;
    std::thread worker;

    explicit State(std::function<void()> cb)
        : wake(std::move(cb)), worker([this] { run(); }) {}

    ~State() {
        request_stop();
        join();
    }

    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        active_stop.request_stop();
        cv.notify_all();
    }

    void join() {
        if (worker.joinable())
            worker.join();
    }

    void run() {
        for (;;) {
            std::optional<Request> job;
            std::stop_token token;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping)
                    return;
                job = std::move(*pending);
                pending.reset();
                busy = true;
                active_stop = std::stop_source{};
                token = active_stop.get_token();
            }
            auto snap = build_files_directory_tree(
                job->root, job->max_nodes, job->max_depth, token);
            std::function<void()> notify;
            {
                std::lock_guard lock(mutex);
                busy = false;
                if (!stopping) {
                    completed = std::move(snap);
                    notify = wake;
                }
            }
            if (notify) {
                try {
                    notify();
                } catch (...) {
                }
            }
        }
    }
};

FilesDirectoryTreeService::FilesDirectoryTreeService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}

FilesDirectoryTreeService::~FilesDirectoryTreeService() = default;

std::uint64_t FilesDirectoryTreeService::start(ck::files::NativeAbsolutePath root,
                                               std::size_t max_nodes,
                                               std::size_t max_depth) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return 0;
    state_->active_stop.request_stop();
    const std::uint64_t id = state_->next_id++;
    state_->pending = State::Request{id, std::move(root), max_nodes, max_depth};
    state_->cv.notify_one();
    return id;
}

void FilesDirectoryTreeService::cancel() noexcept {
    std::lock_guard lock(state_->mutex);
    state_->pending.reset();
    state_->active_stop.request_stop();
}

bool FilesDirectoryTreeService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

std::optional<FilesDirectoryTreeSnapshot> FilesDirectoryTreeService::take() {
    std::lock_guard lock(state_->mutex);
    auto out = std::move(state_->completed);
    state_->completed.reset();
    return out;
}

void FilesDirectoryTreeService::request_stop() noexcept { state_->request_stop(); }
void FilesDirectoryTreeService::join() { state_->join(); }

} // namespace ck::vision
