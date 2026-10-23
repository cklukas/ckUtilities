// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_listing.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <set>
#include <stdexcept>
#if defined(_WIN32)
#include "windows_junction.hpp"
#else
#include <sys/stat.h>
#endif
namespace {
using namespace ck::files; namespace fs = std::filesystem;
template<class T> const T& required(const Observation<T>& value) {
    if (!value.value()) throw std::runtime_error("native listing observation unavailable");
    return *value.value();
}
void write(const fs::path& path) { std::ofstream out(path); out << "fixture"; out.close(); if (!out) throw std::runtime_error("cannot write listing fixture"); }
std::unique_ptr<DirectoryListing> listing(const NativeFileSystem& filesystem, const fs::path& path, DirectoryListingLimits limits) {
    auto result = DirectoryListing::open(filesystem, path, limits); (void)required(result); return std::move(result).take_value();
}
std::vector<ListedEntry> all(DirectoryListing& source, std::size_t rows, std::size_t bytes) {
    std::vector<ListedEntry> result;
    for (int step = 0; step < 1000; ++step) {
        auto batch = source.read(); CK_CHECK(batch.entries.size() <= rows);
        std::size_t used = 0;
        for (auto& entry : batch.entries) { used += entry.name.posix_bytes() ? entry.name.posix_bytes()->size() : 2 * entry.name.windows_units()->size(); result.push_back(std::move(entry)); }
        CK_CHECK(used <= bytes);
        if (batch.state == DirectoryReadState::End) return result;
        if (batch.state != DirectoryReadState::More) throw std::runtime_error("native listing failed");
    }
    throw std::runtime_error("native listing fixture budget exceeded");
}
}
CK_TEST(native_listing_streams_bounded_batches_and_does_not_follow_final_entry_links) {
    const ck::testing::ScratchDirectory scratch("files-listing-native");
    for (int index = 0; index < 37; ++index) write(scratch.path() / ("file-" + std::to_string(index)));
    fs::create_directory(scratch.path() / "directory"); write(scratch.path() / "directory" / "not-a-top-level-entry");
#if defined(_WIN32)
    fs::create_directory(scratch.path() / "link"); ck::testing::make_junction(scratch.path() / "link", scratch.path() / "directory");
    constexpr auto link_kind = ObjectKind::Junction;
#else
    fs::create_directory_symlink("directory", scratch.path() / "link"); constexpr auto link_kind = ObjectKind::SymbolicLink;
#endif
    NativeFileSystem filesystem; auto source = listing(filesystem, scratch.path(), {3, 40, 40});
    const auto entries = all(*source, 3, 40); CK_CHECK(entries.size() == 39);
    std::set<std::string> seen;
    for (const auto& entry : entries) {
        CK_CHECK(seen.insert(entry.name.display_name()).second);
        const auto& metadata = required(entry.metadata);
        const auto kind = required(metadata.kind);
        if (entry.name.display_name() == "link") CK_CHECK(kind == link_kind);
        else if (entry.name.display_name() == "directory") CK_CHECK(kind == ObjectKind::Directory);
        else CK_CHECK(kind == ObjectKind::RegularFile && required(metadata.logical_size) == 7);
    }
}
CK_TEST(native_listing_keeps_held_directory_after_original_path_is_replaced) {
    const ck::testing::ScratchDirectory scratch("files-listing-owner"); const auto path = scratch.path() / "original", retired = scratch.path() / "retired";
    fs::create_directory(path); write(path / "original-entry"); NativeFileSystem filesystem;
    auto source = listing(filesystem, path, {4, 128, 128}); const auto identity = source->identity();
    fs::rename(path, retired); fs::create_directory(path); write(path / "replacement-entry");
    const auto entries = all(*source, 4, 128); CK_CHECK(entries.size() == 1 && entries[0].name.display_name() == "original-entry");
    CK_CHECK(compare_identity(required(identity), required(required(filesystem.inspect_basic(retired)).identity)) == IdentityComparison::Same);
}
CK_TEST(native_listing_cancellation_and_resume_preserve_all_entries_and_leave_cwd_unchanged) {
    const ck::testing::ScratchDirectory scratch("files-listing-resume");
    for (int index = 0; index < 257; ++index) write(scratch.path() / ("entry-" + std::to_string(index)));
    const auto before = fs::current_path(); NativeFileSystem filesystem; auto source = listing(filesystem, scratch.path(), {7, 64, 64});
    std::stop_source stop; stop.request_stop(); const auto paused = source->read(stop.get_token());
    CK_CHECK(paused.state == DirectoryReadState::Cancelled && paused.entries.empty());
    const auto entries = all(*source, 7, 64); std::set<std::string> seen;
    for (const auto& entry : entries) { CK_CHECK(seen.insert(entry.name.display_name()).second); (void)required(entry.metadata); }
    CK_CHECK(entries.size() == 257 && source->read().state == DirectoryReadState::End && fs::current_path() == before);
}
