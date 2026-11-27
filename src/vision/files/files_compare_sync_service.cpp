// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_compare_sync_service.hpp"

#include "directory_cursor.hpp"
#include "files_native_io_helpers.hpp"
#include "native_filesystem.hpp"
#include "opened_entry.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <utility>

namespace ck::vision {
namespace {

struct ObservedSideEntry final {
    std::vector<ck::files::NativeName> components;
    std::optional<ck::files::NativeAbsolutePath> full_path;
    ck::files::ObjectKind kind = ck::files::ObjectKind::Other;
    std::uint64_t size = 0;
    std::optional<ck::files::FileTimestamp> modified;
    std::optional<ck::files::ObjectIdentity> identity;
};

std::string fold_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char ch : text) {
        if (ch >= 'A' && ch <= 'Z')
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        else
            out.push_back(static_cast<char>(ch));
    }
    return out;
}

bool is_hidden_name(std::string_view name) noexcept {
    return !name.empty() && name.front() == '.';
}

bool walk_side(const ck::files::AdapterScope &scope,
               const ck::files::NativeAbsolutePath &root,
               const FilesSyncOptions &options,
               std::map<std::string, ObservedSideEntry> &entries,
               bool &has_case_collisions,
               bool &truncated,
               bool &cancelled,
               std::string &error_out,
               std::stop_token stop_token) {
    ck::files::NativeFileSystem fs(scope);
    auto root_cursor = fs.open_native_directory(root);
    if (!root_cursor.value()) {
        error_out = "Unable to open directory for comparison.";
        return false;
    }

    struct Pending final {
        std::unique_ptr<ck::files::DirectoryCursor> cursor;
        ck::files::NativeAbsolutePath path;
        std::string rel_prefix;
        std::vector<ck::files::NativeName> components;
        std::size_t depth = 0;
    };

    std::set<std::string> folded_keys;
    std::deque<Pending> queue;
    queue.push_back(Pending{std::move(root_cursor).take_value(), root, {}, {}, 0});
    const std::size_t max_depth = options.recursive
        ? std::clamp<std::size_t>(options.max_depth, 1, 256)
        : 1;
    const std::size_t max_entries = std::clamp<std::size_t>(options.max_entries, 1, 200000);

    while (!queue.empty()) {
        if (stop_token.stop_requested()) {
            cancelled = true;
            return false;
        }
        Pending cur = std::move(queue.front());
        queue.pop_front();

        for (;;) {
            if (stop_token.stop_requested()) {
                cancelled = true;
                return false;
            }
            auto batch = cur.cursor->read(256, stop_token);
            if (batch.state == ck::files::DirectoryReadState::Cancelled ||
                stop_token.stop_requested()) {
                cancelled = true;
                return false;
            }
            if (batch.error)
                break;

            for (const auto &name : batch.names) {
                if (stop_token.stop_requested()) {
                    cancelled = true;
                    return false;
                }
                const std::string display = name.display_name();
                if (!options.include_hidden && is_hidden_name(display))
                    continue;
                if (entries.size() >= max_entries) {
                    truncated = true;
                    return true;
                }
                auto child_path_obs = cur.path.child_address(name);
                if (!child_path_obs.value())
                    continue;
                auto opened_obs = cur.cursor->open_child(
                    name, ck::files::EntryAccess::MetadataOnly);
                if (!opened_obs.value())
                    continue;
                auto basic_obs = (*opened_obs.value())->inspect_basic();
                if (!basic_obs.value())
                    continue;

                const std::string rel = cur.rel_prefix.empty()
                    ? display
                    : cur.rel_prefix + "/" + display;
                auto child_components = cur.components;
                child_components.push_back(name);

                if (options.case_insensitive_collision_check) {
                    if (!folded_keys.insert(fold_ascii(rel)).second)
                        has_case_collisions = true;
                }

                const auto entry_kind = basic_obs.value()->kind.value()
                    ? *basic_obs.value()->kind.value()
                    : ck::files::ObjectKind::Other;
                ObservedSideEntry obs;
                obs.components = child_components;
                obs.full_path = *child_path_obs.value();
                obs.kind = entry_kind;
                obs.size = basic_obs.value()->logical_size.value()
                    ? *basic_obs.value()->logical_size.value() : 0;
                if (basic_obs.value()->modified.value())
                    obs.modified = *basic_obs.value()->modified.value();
                if (basic_obs.value()->identity.value())
                    obs.identity = *basic_obs.value()->identity.value();
                entries.emplace(rel, std::move(obs));

                if (entry_kind == ck::files::ObjectKind::Directory &&
                    cur.depth + 1 < max_depth) {
                    auto sub = cur.cursor->open_directory_child(name);
                    if (sub.value()) {
                        queue.push_back(Pending{
                            std::move(sub).take_value(),
                            *child_path_obs.value(),
                            rel,
                            std::move(child_components),
                            cur.depth + 1,
                        });
                    }
                }
            }

            if (batch.state == ck::files::DirectoryReadState::End)
                break;
        }
    }
    return true;
}

std::optional<std::array<std::byte, 32>> hash_native_file(
    const ck::files::NativeAbsolutePath &root,
    const std::vector<ck::files::NativeName> &components,
    std::stop_token stop_token) {
    if (components.empty())
        return std::nullopt;
    ck::files::NativeFileSystem fs;
    ck::files::NativeAbsolutePath parent = root;
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        auto next = parent.child_address(components[i]);
        if (!next.value())
            return std::nullopt;
        parent = *next.value();
    }
    auto dir = fs.open_native_directory(parent);
    if (!dir.value())
        return std::nullopt;
    auto entry = (*dir.value())->open_child(
        components.back(), ck::files::EntryAccess::ContentRead);
    if (!entry.value())
        return std::nullopt;

    ck::files::Sha256 hasher;
    std::uint64_t offset = 0;
    for (;;) {
        if (stop_token.stop_requested())
            return std::nullopt;
        auto got = (*entry.value())->read_content(
            ck::files::ContentReadLimits{64 * 1024}, offset, stop_token);
        if (!got.value())
            return std::nullopt;
        if (got.value()->bytes.empty())
            break;
        hasher.update(got.value()->bytes);
        offset += got.value()->bytes.size();
    }
    return hasher.digest();
}

int compare_timestamps_with_quantum(const std::optional<ck::files::FileTimestamp> &lhs,
                                    const std::optional<ck::files::FileTimestamp> &rhs,
                                    std::uint64_t quantum_ns) noexcept {
    if (!lhs || !rhs)
        return 0;
    const bool lhs_greater = (lhs->seconds() > rhs->seconds()) ||
        (lhs->seconds() == rhs->seconds() && lhs->nanoseconds() > rhs->nanoseconds());
    const bool rhs_greater = (rhs->seconds() > lhs->seconds()) ||
        (rhs->seconds() == lhs->seconds() && rhs->nanoseconds() > lhs->nanoseconds());
    if (!lhs_greater && !rhs_greater)
        return 0;
    const auto &hi = lhs_greater ? *lhs : *rhs;
    const auto &lo = lhs_greater ? *rhs : *lhs;
    std::int64_t sec_diff = hi.seconds() - lo.seconds();
    std::int64_t nsec_diff = static_cast<std::int64_t>(hi.nanoseconds()) -
                             static_cast<std::int64_t>(lo.nanoseconds());
    if (nsec_diff < 0) {
        --sec_diff;
        nsec_diff += 1'000'000'000;
    }
    const std::uint64_t quantum_sec = quantum_ns / 1'000'000'000ULL;
    const std::uint64_t quantum_rem = quantum_ns % 1'000'000'000ULL;
    if (sec_diff < 0 ||
        static_cast<std::uint64_t>(sec_diff) < quantum_sec ||
        (static_cast<std::uint64_t>(sec_diff) == quantum_sec &&
         static_cast<std::uint64_t>(nsec_diff) <= quantum_rem))
        return 0;
    return lhs_greater ? 1 : -1;
}

bool copy_file_native(const ck::files::AdapterScope &scope,
                      const ck::files::NativeAbsolutePath &src_root,
                      const ck::files::NativeAbsolutePath &dst_root,
                      const std::vector<ck::files::NativeName> &components,
                      const std::optional<ck::files::ObjectIdentity> &expected_src_id,
                      std::string &diag,
                      std::stop_token stop_token) {
    if (components.empty()) {
        diag = "Empty relative path.";
        return false;
    }
    ck::files::NativeFileSystem fs(scope);
    ck::files::NativeAbsolutePath src_parent = src_root;
    ck::files::NativeAbsolutePath dst_parent = dst_root;
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        auto next_src = src_parent.child_address(components[i]);
        auto next_dst = dst_parent.child_address(components[i]);
        if (!next_src.value() || !next_dst.value()) {
            diag = "Invalid intermediate path component.";
            return false;
        }
        auto parent_dir = fs.open_native_directory(dst_parent);
        if (parent_dir.value())
            (void)(*parent_dir.value())->create_child_directory(components[i]);
        src_parent = *next_src.value();
        dst_parent = *next_dst.value();
    }

    auto src_dir = fs.open_native_directory(src_parent);
    if (!src_dir.value()) {
        diag = "Source parent directory unavailable.";
        return false;
    }
    auto src_file = (*src_dir.value())->open_child(
        components.back(), ck::files::EntryAccess::ContentRead);
    if (!src_file.value()) {
        diag = "Source file unavailable.";
        return false;
    }
    auto cur_basic = (*src_file.value())->inspect_basic();
    if (!cur_basic.value()) {
        diag = "Unable to inspect source metadata.";
        return false;
    }
    if (expected_src_id) {
        if (!cur_basic.value()->identity.value() ||
            ck::files::compare_identity(*cur_basic.value()->identity.value(), *expected_src_id) !=
                ck::files::IdentityComparison::Same) {
            diag = "Source identity changed before copy; skipped.";
            return false;
        }
    }

    auto dst_path_obs = dst_parent.child_address(components.back());
    if (!dst_path_obs.value()) {
        diag = "Unable to form destination path.";
        return false;
    }

    std::vector<std::byte> payload;
    const std::uint64_t sz = cur_basic.value()->logical_size.value()
        ? *cur_basic.value()->logical_size.value() : 0;
    if (sz > 512ULL * 1024ULL * 1024ULL) {
        diag = "File exceeds sync buffer limit.";
        return false;
    }
    payload.reserve(static_cast<std::size_t>(sz));
    std::uint64_t offset = 0;
    for (;;) {
        if (stop_token.stop_requested()) {
            diag = "Cancelled during copy.";
            return false;
        }
        auto got = (*src_file.value())->read_content(
            ck::files::ContentReadLimits{64 * 1024}, offset, stop_token);
        if (!got.value()) {
            diag = "Failed reading source bytes.";
            return false;
        }
        if (got.value()->bytes.empty())
            break;
        payload.insert(payload.end(), got.value()->bytes.begin(), got.value()->bytes.end());
        offset += got.value()->bytes.size();
    }
    return detail::write_native_file_staged(
        *dst_path_obs.value(), payload, true, ".ck-sync-stage-", diag, stop_token);
}

bool create_dir_native(const ck::files::NativeAbsolutePath &root,
                       const std::vector<ck::files::NativeName> &components,
                       std::string &diag) {
    if (components.empty())
        return false;
    ck::files::NativeFileSystem fs;
    ck::files::NativeAbsolutePath cur = root;
    for (std::size_t i = 0; i < components.size(); ++i) {
        auto dir = fs.open_native_directory(cur);
        if (!dir.value()) {
            diag = "Parent directory unavailable.";
            return false;
        }
        (void)(*dir.value())->create_child_directory(components[i]);
        auto next = cur.child_address(components[i]);
        if (!next.value()) {
            diag = "Invalid child directory path.";
            return false;
        }
        cur = *next.value();
    }
    return true;
}

bool delete_file_native(const ck::files::AdapterScope &scope,
                        const ck::files::NativeAbsolutePath &root,
                        const std::vector<ck::files::NativeName> &components,
                        ck::files::ObjectKind /*kind*/,
                        const std::optional<ck::files::ObjectIdentity> &expected_id,
                        std::string &diag) {
    if (components.empty())
        return false;
    ck::files::NativeFileSystem fs(scope);
    ck::files::NativeAbsolutePath parent = root;
    for (std::size_t i = 0; i + 1 < components.size(); ++i) {
        auto next = parent.child_address(components[i]);
        if (!next.value()) {
            diag = "Invalid parent path.";
            return false;
        }
        parent = *next.value();
    }
    auto dir = fs.open_native_directory(parent);
    if (!dir.value()) {
        diag = "Parent directory unavailable for delete.";
        return false;
    }
    const auto live_id = detail::inspect_child_identity(**dir.value(), components.back());
    if (!live_id) {
        diag = "Target already absent.";
        return false;
    }
    if (expected_id &&
        ck::files::compare_identity(*live_id, *expected_id) != ck::files::IdentityComparison::Same) {
        diag = "Target identity changed before delete; preserved.";
        return false;
    }
    auto removed = (*dir.value())->retire_child(components.back(), *live_id);
    if (!removed.value()) {
        diag = "Unable to remove target entry.";
        return false;
    }
    return true;
}

std::vector<std::string> split_lines(std::string_view text, std::size_t max_lines, bool &truncated) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < text.size()) {
        if (lines.size() >= max_lines) {
            truncated = true;
            break;
        }
        const std::size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            std::string line(text.substr(start));
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            lines.push_back(std::move(line));
            break;
        }
        std::string line(text.substr(start, nl - start));
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
        start = nl + 1;
    }
    return lines;
}

bool read_all_native_bounded(const ck::files::NativeAbsolutePath &path,
                             std::size_t max_bytes,
                             std::vector<std::byte> &out,
                             std::uint64_t &logical_size_out,
                             bool &truncated_out,
                             std::string &error_out,
                             std::stop_token stop_token) {
    return detail::read_native_file_bounded(
        path, max_bytes, out, logical_size_out, truncated_out, error_out, stop_token);
}

bool looks_binary(std::span<const std::byte> data) noexcept {
    const std::size_t probe = std::min<std::size_t>(data.size(), 8192);
    for (std::size_t i = 0; i < probe; ++i) {
        if (data[i] == std::byte{0})
            return true;
    }
    return false;
}

} // namespace

FilesSyncPlan build_files_sync_plan(const ck::files::NativeAbsolutePath &left_root,
                                    const ck::files::NativeAbsolutePath &right_root,
                                    const FilesSyncOptions &options,
                                    std::stop_token stop_token) {
    FilesSyncPlan plan;
    plan.left_root = left_root;
    plan.right_root = right_root;
    plan.options = options;

    if (!left_root.is_valid() || !right_root.is_valid()) {
        plan.error_message = "Both comparison directories must be valid.";
        return plan;
    }

    std::map<std::string, ObservedSideEntry> left_entries;
    std::map<std::string, ObservedSideEntry> right_entries;

    if (!walk_side(plan.adapter_scope, left_root, options, left_entries, plan.has_case_collisions,
                   plan.truncated, plan.cancelled, plan.error_message, stop_token))
        return plan;
    if (!walk_side(plan.adapter_scope, right_root, options, right_entries, plan.has_case_collisions,
                   plan.truncated, plan.cancelled, plan.error_message, stop_token))
        return plan;

    std::set<std::string> all_keys;
    for (const auto &[k, _] : left_entries)
        all_keys.insert(k);
    for (const auto &[k, _] : right_entries)
        all_keys.insert(k);

    for (const auto &key : all_keys) {
        if (stop_token.stop_requested()) {
            plan.cancelled = true;
            return plan;
        }
        const auto lit = left_entries.find(key);
        const auto rit = right_entries.find(key);
        const bool has_l = lit != left_entries.end();
        const bool has_r = rit != right_entries.end();

        FilesSyncItem item;
        item.relative_key = key;
        item.left_present = has_l;
        item.right_present = has_r;
        if (has_l) {
            item.relative_components = lit->second.components;
            item.left_kind = lit->second.kind;
            item.left_size = lit->second.size;
            item.left_modified = lit->second.modified;
            item.left_identity = lit->second.identity;
        }
        if (has_r) {
            if (item.relative_components.empty())
                item.relative_components = rit->second.components;
            item.right_kind = rit->second.kind;
            item.right_size = rit->second.size;
            item.right_modified = rit->second.modified;
            item.right_identity = rit->second.identity;
        }

        if (has_l && !has_r) {
            item.relation = FilesCompareRelation::LeftOnly;
            switch (options.direction) {
                case FilesSyncDirection::CompareOnly:
                    item.proposed_action = FilesSyncAction::None;
                    break;
                case FilesSyncDirection::OneWayLeftToRight:
                case FilesSyncDirection::TwoWayCopyOnly:
                    item.proposed_action =
                        item.left_kind == ck::files::ObjectKind::Directory
                            ? FilesSyncAction::CreateDirRight
                            : (item.left_kind == ck::files::ObjectKind::RegularFile
                                   ? FilesSyncAction::CopyLeftToRight
                                   : FilesSyncAction::ReviewConflict);
                    break;
                case FilesSyncDirection::OneWayRightToLeft:
                    item.proposed_action = options.allow_one_way_deletions
                        ? FilesSyncAction::DeleteLeft
                        : FilesSyncAction::None;
                    break;
            }
        } else if (!has_l && has_r) {
            item.relation = FilesCompareRelation::RightOnly;
            switch (options.direction) {
                case FilesSyncDirection::CompareOnly:
                    item.proposed_action = FilesSyncAction::None;
                    break;
                case FilesSyncDirection::OneWayRightToLeft:
                case FilesSyncDirection::TwoWayCopyOnly:
                    item.proposed_action =
                        item.right_kind == ck::files::ObjectKind::Directory
                            ? FilesSyncAction::CreateDirLeft
                            : (item.right_kind == ck::files::ObjectKind::RegularFile
                                   ? FilesSyncAction::CopyRightToLeft
                                   : FilesSyncAction::ReviewConflict);
                    break;
                case FilesSyncDirection::OneWayLeftToRight:
                    item.proposed_action = options.allow_one_way_deletions
                        ? FilesSyncAction::DeleteRight
                        : FilesSyncAction::None;
                    break;
            }
        } else {
            // Both sides present.
            if (item.left_kind != item.right_kind) {
                item.relation = FilesCompareRelation::TypeMismatch;
                item.proposed_action = options.direction == FilesSyncDirection::CompareOnly
                    ? FilesSyncAction::None
                    : FilesSyncAction::ReviewConflict;
                item.diagnostic = "Entry kinds differ between left and right.";
            } else if (item.left_kind == ck::files::ObjectKind::Directory) {
                item.relation = FilesCompareRelation::Identical;
                item.proposed_action = FilesSyncAction::None;
            } else if (item.left_kind == ck::files::ObjectKind::RegularFile) {
                const int cmp_time = compare_timestamps_with_quantum(
                    item.left_modified, item.right_modified, options.timestamp_quantum_ns);
                bool equal_content = false;
                bool content_checked = false;
                if (options.compare_mode == FilesCompareMode::ThoroughContent &&
                    item.left_size == item.right_size) {
                    const auto lh = hash_native_file(left_root, item.relative_components, stop_token);
                    const auto rh = hash_native_file(right_root, item.relative_components, stop_token);
                    if (lh && rh) {
                        content_checked = true;
                        equal_content = (*lh == *rh);
                    }
                }

                if (content_checked) {
                    if (equal_content) {
                        item.relation = FilesCompareRelation::Identical;
                    } else if (cmp_time > 0) {
                        item.relation = FilesCompareRelation::LeftNewer;
                    } else if (cmp_time < 0) {
                        item.relation = FilesCompareRelation::RightNewer;
                    } else {
                        item.relation = FilesCompareRelation::EqualSizeContentDiffers;
                        item.diagnostic = "Equal size and timestamp within quantum, but SHA-256 content differs.";
                    }
                } else if (item.left_size == item.right_size && cmp_time == 0) {
                    item.relation = FilesCompareRelation::Identical;
                } else if (cmp_time > 0) {
                    item.relation = FilesCompareRelation::LeftNewer;
                } else if (cmp_time < 0) {
                    item.relation = FilesCompareRelation::RightNewer;
                } else {
                    item.relation = FilesCompareRelation::Conflict;
                    item.diagnostic = "Different sizes with equal timestamp within quantum.";
                }

                if (item.relation == FilesCompareRelation::Identical) {
                    item.proposed_action = FilesSyncAction::None;
                } else if (options.direction == FilesSyncDirection::CompareOnly) {
                    item.proposed_action = FilesSyncAction::None;
                } else if (options.direction == FilesSyncDirection::OneWayLeftToRight) {
                    item.proposed_action = FilesSyncAction::CopyLeftToRight;
                } else if (options.direction == FilesSyncDirection::OneWayRightToLeft) {
                    item.proposed_action = FilesSyncAction::CopyRightToLeft;
                } else {
                    // TwoWayCopyOnly: never overwrite silently when content differs at equal time!
                    if (item.relation == FilesCompareRelation::LeftNewer)
                        item.proposed_action = FilesSyncAction::CopyLeftToRight;
                    else if (item.relation == FilesCompareRelation::RightNewer)
                        item.proposed_action = FilesSyncAction::CopyRightToLeft;
                    else
                        item.proposed_action = FilesSyncAction::ReviewConflict;
                }
            } else {
                // Symlinks/other special objects require review rather than blind mutation.
                item.relation = FilesCompareRelation::Conflict;
                item.proposed_action = options.direction == FilesSyncDirection::CompareOnly
                    ? FilesSyncAction::None
                    : FilesSyncAction::ReviewConflict;
                item.diagnostic = "Non-regular entry requires manual review.";
            }
        }

        if (item.relation == FilesCompareRelation::Identical)
            ++plan.identical_count;
        if (item.proposed_action == FilesSyncAction::ReviewConflict)
            ++plan.conflict_count;
        else if (item.proposed_action != FilesSyncAction::None)
            ++plan.action_count;

        plan.items.push_back(std::move(item));
    }
    return plan;
}

FilesSyncExecutionSummary execute_files_sync_plan(const FilesSyncPlan &plan,
                                                  std::stop_token stop_token) {
    FilesSyncExecutionSummary summary;
    if (plan.options.direction == FilesSyncDirection::CompareOnly ||
        !plan.left_root || !plan.right_root) {
        summary.skipped = plan.items.size();
        return summary;
    }

    // Create directories and copy files first, then perform any approved one-way deletions in reverse order.
    for (const auto &item : plan.items) {
        if (stop_token.stop_requested()) {
            summary.cancelled = true;
            return summary;
        }
        std::string diag;
        switch (item.proposed_action) {
            case FilesSyncAction::None:
                ++summary.skipped;
                break;
            case FilesSyncAction::ReviewConflict:
                ++summary.conflicts_preserved;
                break;
            case FilesSyncAction::CreateDirRight:
                if (create_dir_native(*plan.right_root, item.relative_components, diag))
                    ++summary.completed;
                else {
                    ++summary.failed;
                    summary.diagnostics.push_back(item.relative_key + ": " + diag);
                }
                break;
            case FilesSyncAction::CreateDirLeft:
                if (create_dir_native(*plan.left_root, item.relative_components, diag))
                    ++summary.completed;
                else {
                    ++summary.failed;
                    summary.diagnostics.push_back(item.relative_key + ": " + diag);
                }
                break;
            case FilesSyncAction::CopyLeftToRight:
                if (copy_file_native(plan.adapter_scope, *plan.left_root, *plan.right_root, item.relative_components,
                                     item.left_identity, diag, stop_token))
                    ++summary.completed;
                else {
                    ++summary.failed;
                    summary.diagnostics.push_back(item.relative_key + ": " + diag);
                }
                break;
            case FilesSyncAction::CopyRightToLeft:
                if (copy_file_native(plan.adapter_scope, *plan.right_root, *plan.left_root, item.relative_components,
                                     item.right_identity, diag, stop_token))
                    ++summary.completed;
                else {
                    ++summary.failed;
                    summary.diagnostics.push_back(item.relative_key + ": " + diag);
                }
                break;
            case FilesSyncAction::DeleteLeft:
            case FilesSyncAction::DeleteRight:
                // Handled in reverse pass below.
                break;
        }
    }

    // Never delete in TwoWayCopyOnly mode even if a caller mutated the plan.
    if (plan.options.direction == FilesSyncDirection::TwoWayCopyOnly ||
        !plan.options.allow_one_way_deletions) {
        for (const auto &item : plan.items) {
            if (item.proposed_action == FilesSyncAction::DeleteLeft ||
                item.proposed_action == FilesSyncAction::DeleteRight)
                ++summary.conflicts_preserved;
        }
        return summary;
    }

    for (auto it = plan.items.rbegin(); it != plan.items.rend(); ++it) {
        if (stop_token.stop_requested()) {
            summary.cancelled = true;
            return summary;
        }
        std::string diag;
        if (it->proposed_action == FilesSyncAction::DeleteRight) {
            if (delete_file_native(plan.adapter_scope, *plan.right_root, it->relative_components,
                                   it->right_kind, it->right_identity, diag))
                ++summary.completed;
            else {
                ++summary.failed;
                summary.diagnostics.push_back(it->relative_key + ": " + diag);
            }
        } else if (it->proposed_action == FilesSyncAction::DeleteLeft) {
            if (delete_file_native(plan.adapter_scope, *plan.left_root, it->relative_components,
                                   it->left_kind, it->left_identity, diag))
                ++summary.completed;
            else {
                ++summary.failed;
                summary.diagnostics.push_back(it->relative_key + ": " + diag);
            }
        }
    }
    return summary;
}

FilesDiffResult compute_text_diff(std::string_view left_text,
                                  std::string_view right_text,
                                  const FilesDiffLimits &limits) {
    FilesDiffResult res;
    res.binary = false;
    res.left_size = left_text.size();
    res.right_size = right_text.size();

    if (left_text == right_text) {
        res.identical = true;
        return res;
    }

    bool trunc_l = false;
    bool trunc_r = false;
    const auto left = split_lines(left_text, limits.max_text_lines, trunc_l);
    const auto right = split_lines(right_text, limits.max_text_lines, trunc_r);
    res.truncated = trunc_l || trunc_r;

    // Trim common prefix and suffix to keep DP small and fast.
    std::size_t prefix = 0;
    while (prefix < left.size() && prefix < right.size() && left[prefix] == right[prefix])
        ++prefix;
    std::size_t suffix = 0;
    while (suffix < (left.size() - prefix) && suffix < (right.size() - prefix) &&
           left[left.size() - 1 - suffix] == right[right.size() - 1 - suffix])
        ++suffix;

    const std::size_t n = left.size() - prefix - suffix;
    const std::size_t m = right.size() - prefix - suffix;

    std::vector<FilesDiffLine> raw;
    for (std::size_t i = 0; i < prefix; ++i)
        raw.push_back({FilesDiffLineKind::Context, i + 1, i + 1, left[i]});

    // Bounded LCS DP when n*m <= 4,000,000; otherwise emit block replacement safely.
    if (n * m <= 4'000'000ULL) {
        std::vector<std::uint32_t> dp((n + 1) * (m + 1), 0);
        const auto at = [&](std::size_t i, std::size_t j) -> std::uint32_t & {
            return dp[i * (m + 1) + j];
        };
        for (std::size_t i = n; i-- > 0;) {
            for (std::size_t j = m; j-- > 0;) {
                if (left[prefix + i] == right[prefix + j])
                    at(i, j) = at(i + 1, j + 1) + 1;
                else
                    at(i, j) = std::max(at(i + 1, j), at(i, j + 1));
            }
        }
        std::size_t i = 0;
        std::size_t j = 0;
        while (i < n || j < m) {
            if (i < n && j < m && left[prefix + i] == right[prefix + j]) {
                raw.push_back({FilesDiffLineKind::Context, prefix + i + 1, prefix + j + 1, left[prefix + i]});
                ++i;
                ++j;
            } else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) {
                raw.push_back({FilesDiffLineKind::Removed, prefix + i + 1, 0, left[prefix + i]});
                ++i;
            } else {
                raw.push_back({FilesDiffLineKind::Added, 0, prefix + j + 1, right[prefix + j]});
                ++j;
            }
        }
    } else {
        for (std::size_t i = 0; i < n; ++i)
            raw.push_back({FilesDiffLineKind::Removed, prefix + i + 1, 0, left[prefix + i]});
        for (std::size_t j = 0; j < m; ++j)
            raw.push_back({FilesDiffLineKind::Added, 0, prefix + j + 1, right[prefix + j]});
    }

    for (std::size_t s = 0; s < suffix; ++s) {
        const std::size_t li = left.size() - suffix + s;
        const std::size_t ri = right.size() - suffix + s;
        raw.push_back({FilesDiffLineKind::Context, li + 1, ri + 1, left[li]});
    }

    // Filter to changed lines plus `limits.context_lines` surrounding context.
    std::vector<bool> keep(raw.size(), false);
    for (std::size_t idx = 0; idx < raw.size(); ++idx) {
        if (raw[idx].kind != FilesDiffLineKind::Context) {
            const std::size_t lo = idx > limits.context_lines ? idx - limits.context_lines : 0;
            const std::size_t hi = std::min(raw.size(), idx + limits.context_lines + 1);
            for (std::size_t k = lo; k < hi; ++k)
                keep[k] = true;
        }
    }
    for (std::size_t idx = 0; idx < raw.size(); ++idx) {
        if (keep[idx])
            res.text_lines.push_back(std::move(raw[idx]));
    }
    res.identical = res.text_lines.empty() && !res.truncated;
    return res;
}

FilesDiffResult compute_binary_diff(std::span<const std::byte> left_bytes,
                                    std::span<const std::byte> right_bytes,
                                    const FilesDiffLimits &limits) {
    FilesDiffResult res;
    res.binary = true;
    res.left_size = left_bytes.size();
    res.right_size = right_bytes.size();

    const std::size_t max_len = std::max(left_bytes.size(), right_bytes.size());
    for (std::size_t i = 0; i < max_len; ++i) {
        const bool has_l = i < left_bytes.size();
        const bool has_r = i < right_bytes.size();
        if (has_l && has_r && left_bytes[i] == right_bytes[i])
            continue;
        if (res.binary_diffs.size() >= limits.max_binary_diffs) {
            res.truncated = true;
            break;
        }
        FilesBinaryDiffEntry entry;
        entry.offset = i;
        if (has_l)
            entry.left_byte = std::to_integer<std::uint8_t>(left_bytes[i]);
        if (has_r)
            entry.right_byte = std::to_integer<std::uint8_t>(right_bytes[i]);
        res.binary_diffs.push_back(entry);
    }
    res.identical = res.binary_diffs.empty() && !res.truncated;
    return res;
}

FilesDiffResult diff_native_files(const ck::files::NativeAbsolutePath &left_path,
                                  const ck::files::NativeAbsolutePath &right_path,
                                  const FilesDiffLimits &limits,
                                  std::stop_token stop_token) {
    FilesDiffResult res;
    std::vector<std::byte> left_bytes;
    std::vector<std::byte> right_bytes;
    std::uint64_t left_size = 0;
    std::uint64_t right_size = 0;
    bool trunc = false;
    std::string err;

    if (!read_all_native_bounded(left_path, limits.max_file_bytes, left_bytes,
                                 left_size, trunc, err, stop_token)) {
        res.error_message = "Left file: " + err;
        return res;
    }
    if (!read_all_native_bounded(right_path, limits.max_file_bytes, right_bytes,
                                 right_size, trunc, err, stop_token)) {
        res.error_message = "Right file: " + err;
        return res;
    }

    if (looks_binary(left_bytes) || looks_binary(right_bytes)) {
        res = compute_binary_diff(left_bytes, right_bytes, limits);
    } else {
        std::string_view l_sv(reinterpret_cast<const char *>(left_bytes.data()), left_bytes.size());
        std::string_view r_sv(reinterpret_cast<const char *>(right_bytes.data()), right_bytes.size());
        res = compute_text_diff(l_sv, r_sv, limits);
    }
    res.left_size = left_size;
    res.right_size = right_size;
    res.truncated = res.truncated || trunc;
    if (left_size != right_size)
        res.identical = false;
    return res;
}

} // namespace ck::vision
