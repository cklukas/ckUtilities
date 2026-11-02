// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_filesystem.hpp"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace ck::files {

// A directory entry observed through an owning native cursor. The path is a
// presentation/navigation value only; enumeration and metadata acquisition
// have already happened through the held native parent and child handles.
struct NativeWalkEntry {
    std::filesystem::path path;
    NativeAbsolutePath address;
    std::optional<NativeName> name;
    BasicMetadata metadata;
    int depth = 0;
    bool root = false;
};

enum class NativeWalkAction { Continue, SkipSubtree, Stop };

struct NativeWalkResult {
    bool completed = true;
    bool cancelled = false;
};

inline std::error_code native_walk_error(const UnavailableObservation& detail) {
    if (detail.error.code > 0 &&
        (detail.error.domain == ErrorDomain::Win32 || detail.error.domain == ErrorDomain::Posix))
        return {static_cast<int>(detail.error.code),
                detail.error.domain == ErrorDomain::Win32 ? std::system_category() : std::generic_category()};
    return std::make_error_code(std::errc::io_error);
}

// Bounded, handle-owned native directory walk. This is intentionally a
// header-only algorithm so ck-find and ck-du can share the exact same Windows
// filesystem boundary without introducing a second path-based adapter.
template <class EntryCallback, class ErrorCallback, class CancelCallback, class ExitCallback>
NativeWalkResult walk_native_directories(const std::filesystem::path& root,
                                         bool recursive,
                                         bool follow_final_links,
                                         EntryCallback&& on_entry,
                                         ErrorCallback&& on_error,
                                         CancelCallback&& cancelled,
                                         ExitCallback&& on_directory_exit) {
    NativeFileSystem filesystem;
    auto root_address = NativeAbsolutePath::from_host(root);
    if (!root_address.value()) {
        on_error(root, native_walk_error(*root_address.unavailable_detail()));
        return {true, false};
    }
    auto root_metadata = filesystem.inspect_basic(root);
    if (!root_metadata.value()) {
        on_error(root, native_walk_error(*root_metadata.unavailable_detail()));
        return {true, false};
    }

    NativeWalkEntry root_entry{root, std::move(root_address).take_value(), std::nullopt,
                               std::move(root_metadata).take_value(), 0, true};
    const auto root_action = on_entry(root_entry);
    if (root_action == NativeWalkAction::Stop)
        return {false, false};
    if (root_action == NativeWalkAction::SkipSubtree)
        return {true, false};

    using Frame = std::tuple<std::unique_ptr<DirectoryCursor>, NativeAbsolutePath,
                             std::filesystem::path, int>;
    std::vector<Frame> stack;
    std::vector<ObjectIdentity> visited_directories;
    if (root_entry.metadata.identity.value())
        visited_directories.push_back(*root_entry.metadata.identity.value());

    auto open_root = filesystem.open_directory(root,
        follow_final_links ? DirectoryLinkPolicy::FollowFinalLink : DirectoryLinkPolicy::RejectFinalLink);
    if (!open_root.value()) {
        on_error(root, native_walk_error(*open_root.unavailable_detail()));
        return {true, false};
    }
    stack.emplace_back(std::move(open_root).take_value(), std::move(root_entry.address), root, 0);

    while (!stack.empty()) {
        if (cancelled())
            return {false, true};
        auto& frame = stack.back();
        auto& cursor = std::get<0>(frame);
        auto read = cursor->read(1);
        if (read.state == DirectoryReadState::Cancelled)
            return {false, true};
        if (read.state == DirectoryReadState::Error) {
            if (read.error)
                on_error(std::get<2>(frame), native_walk_error(*read.error));
            on_directory_exit(std::get<2>(frame), std::get<3>(frame));
            stack.pop_back();
            continue;
        }
        if (read.state == DirectoryReadState::End) {
            on_directory_exit(std::get<2>(frame), std::get<3>(frame));
            stack.pop_back();
            continue;
        }
        if (read.names.empty())
            continue;

        NativeName name = std::move(read.names.front());
        auto child_address = std::get<1>(frame).child_address(name);
        if (!child_address.value()) {
            on_error(std::get<2>(frame), native_walk_error(*child_address.unavailable_detail()));
            continue;
        }
        const auto child_path = child_address.value()->host_path();
        if (!child_path.value()) {
            on_error(std::get<2>(frame), native_walk_error(*child_path.unavailable_detail()));
            continue;
        }
        auto opened = cursor->open_child(name, EntryAccess::MetadataOnly);
        if (!opened.value()) {
            on_error(*child_path.value(), native_walk_error(*opened.unavailable_detail()));
            continue;
        }
        auto child = std::move(opened).take_value();
        auto metadata = child->inspect_basic();
        if (!metadata.value()) {
            on_error(*child_path.value(), native_walk_error(*metadata.unavailable_detail()));
            continue;
        }

        std::unique_ptr<DirectoryCursor> followed_directory;
        if (follow_final_links && metadata.value() && metadata.value()->kind.value()) {
            const auto kind = *metadata.value()->kind.value();
            if (kind == ObjectKind::Directory || kind == ObjectKind::SymbolicLink ||
                kind == ObjectKind::Junction || kind == ObjectKind::ReparsePoint) {
                auto followed = filesystem.open_directory(*child_path.value(), DirectoryLinkPolicy::FollowFinalLink);
                if (followed.value()) {
                    followed_directory = std::move(followed).take_value();
                    auto target_metadata = followed_directory->inspect_basic();
                    if (target_metadata.value())
                        metadata = std::move(target_metadata);
                }
            }
        }

        NativeWalkEntry entry{*child_path.value(), std::move(child_address).take_value(),
                              std::optional<NativeName>(std::move(name)),
                              std::move(metadata).take_value(), std::get<3>(frame) + 1, false};
        const auto action = on_entry(entry);
        if (action == NativeWalkAction::Stop)
            return {false, false};
        if (action == NativeWalkAction::SkipSubtree || !recursive || !entry.metadata.kind.value() ||
            *entry.metadata.kind.value() != ObjectKind::Directory)
            continue;

        Observation<std::unique_ptr<DirectoryCursor>> child_directory;
        if (followed_directory) {
            child_directory = Observation<std::unique_ptr<DirectoryCursor>>::available(std::move(followed_directory));
        } else if (follow_final_links) {
            child_directory = filesystem.open_directory(entry.path, DirectoryLinkPolicy::FollowFinalLink);
        } else {
            child_directory = cursor->open_directory_child(*entry.name);
        }
        if (!child_directory.value()) {
            on_error(entry.path, native_walk_error(*child_directory.unavailable_detail()));
            continue;
        }
        auto directory = std::move(child_directory).take_value();
        const auto identity = directory->identity();
        if (identity.value() && std::any_of(visited_directories.begin(), visited_directories.end(),
                                            [&](const ObjectIdentity& prior) {
                                                return compare_identity(prior, *identity.value()) == IdentityComparison::Same;
                                            }))
            continue;
        if (identity.value())
            visited_directories.push_back(*identity.value());
        stack.emplace_back(std::move(directory), std::move(entry.address), entry.path, entry.depth);
    }
    return {true, false};
}

} // namespace ck::files
