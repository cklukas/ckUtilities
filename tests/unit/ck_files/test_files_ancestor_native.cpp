// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ancestor_walk_job.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#ifdef _WIN32
#include "windows_junction.hpp"
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <fstream>
#include <stdexcept>
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native ancestor observation unavailable");
    return std::move(value).take_value();
}
NativeName name(const char* text) {
#ifdef _WIN32
    std::u16string value; while (*text) value.push_back(static_cast<char16_t>(static_cast<unsigned char>(*text++)));
    return *NativeName::from_windows(std::move(value));
#else
    return *NativeName::from_posix(text);
#endif
}
std::shared_ptr<const DirectoryCursor> directory(const NativeFileSystem& filesystem, const fs::path& path) {
    return std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(path)));
}
auto finish(AncestorWalkJob& job) {
    for (int i = 0; i < 1000; ++i) {
        if (job.advance() == AncestorWalkState::Complete) return take(job.completed());
        if (job.state() == AncestorWalkState::Failed) throw std::runtime_error("native ancestor job failed");
    }
    throw std::runtime_error("native ancestor fixture exceeded step bound");
}
}
CK_TEST(native_parent_queries_leave_directory_enumeration_position_unchanged) {
    const ck::testing::ScratchDirectory scratch("files-parent-enumeration"); NativeFileSystem filesystem;
    const auto child = scratch.path() / "child"; fs::create_directory(child);
    for (const auto* text : {"a", "b", "c"}) fs::create_directory(child / text);
    auto cursor = take(filesystem.open_directory(child)); const auto first = cursor->read(1); CK_CHECK(first.names.size() == 1);
    const auto parent = take(cursor->open_parent_directory());
    CK_CHECK(compare_identity(take(take(parent->inspect_basic()).identity), take(directory(filesystem, scratch.path())->identity())) == IdentityComparison::Same);
    auto rest = cursor->read(10); std::vector<NativeName> names = first.names; names.insert(names.end(), rest.names.begin(), rest.names.end());
    std::sort(names.begin(), names.end(), NativeNameLess{}); CK_CHECK(names == std::vector<NativeName>({name("a"), name("b"), name("c")}));
}
CK_TEST(native_parent_references_follow_the_held_directory_after_rename_and_path_replacement) {
    const ck::testing::ScratchDirectory scratch("files-parent-move"); NativeFileSystem filesystem;
    const auto a = scratch.path() / "a", b = scratch.path() / "b"; fs::create_directory(a); fs::create_directory(b); fs::create_directory(a / "child");
    auto cursor = directory(filesystem, a); auto held = take(cursor->open_child(name("child")));
    fs::rename(a / "child", b / "moved"); fs::create_directory(a / "child");
    const auto parent = take(held->open_parent_directory());
    CK_CHECK(compare_identity(take(take(parent->inspect_basic()).identity), take(directory(filesystem, b)->identity())) == IdentityComparison::Same);
    CK_CHECK(compare_identity(take(take(parent->inspect_basic()).identity), take(cursor->identity())) == IdentityComparison::Different);
    const auto grandparent = take(parent->open_parent_directory());
    CK_CHECK(compare_identity(take(take(grandparent->inspect_basic()).identity), take(directory(filesystem, scratch.path())->identity())) == IdentityComparison::Same);
}
CK_TEST(native_ancestor_walk_qualifies_self_and_nested_source_with_exact_directory_budgets) {
    const ck::testing::ScratchDirectory scratch("files-ancestor-match"); NativeFileSystem filesystem;
    fs::create_directories(scratch.path() / "source/inner/leaf"); auto root = directory(filesystem, scratch.path());
    const auto source = take(HeldChild::observe(root, name("source"))); auto destination = directory(filesystem, scratch.path() / "source/inner/leaf");
    AncestorWalkJob walk(source, destination, {3}); const auto result = finish(walk);
    CK_CHECK(take(result->outcome()).end == AncestorWalkEnd::ObservedSource && take(result->outcome()).directory == 2 && result->directories().size() == 3);
    AncestorWalkJob limited(source, destination, {2}); CK_CHECK(finish(limited)->outcome().state() == ObservationState::Unknown);
    AncestorWalkJob self(source, directory(filesystem, scratch.path() / "source"), {1}); CK_CHECK(take(finish(self)->outcome()).directory == 0);
}
CK_TEST(native_ancestor_walk_repeated_root_is_an_observation_not_safe_copy_permission) {
    const ck::testing::ScratchDirectory scratch("files-ancestor-repeat"); NativeFileSystem filesystem;
    fs::create_directory(scratch.path() / "source"); auto root = directory(filesystem, scratch.path()); const auto source = take(HeldChild::observe(root, name("source")));
    AncestorWalkJob walk(source, root, {32}); const auto result = finish(walk); const auto end = take(result->outcome());
    CK_CHECK(end.end == AncestorWalkEnd::RepeatedDirectory && end.repeated && *end.repeated < end.directory);
}
CK_TEST(native_parent_queries_refuse_regular_files_and_final_links_without_following_them) {
    const ck::testing::ScratchDirectory scratch("files-parent-kinds"); NativeFileSystem filesystem;
    { std::ofstream output(scratch.path() / "file"); output << "file"; output.close(); if (!output) throw std::runtime_error("cannot write parent fixture"); }
    fs::create_directory(scratch.path() / "target");
#ifdef _WIN32
    fs::create_directory(scratch.path() / "link");
    struct RemoveJunction { fs::path value; ~RemoveJunction() { RemoveDirectoryW(value.c_str()); } } remove{scratch.path() / "link"};
    ck::testing::make_junction(scratch.path() / "link", scratch.path() / "target");
#else
    fs::create_directory_symlink(scratch.path() / "target", scratch.path() / "link");
#endif
    auto root = directory(filesystem, scratch.path());
    for (const auto* text : {"file", "link"}) {
        auto child = take(root->open_child(name(text))); CK_CHECK(child->open_parent_directory().state() == ObservationState::Unsupported);
    }
}
#ifdef __linux__
CK_TEST(native_parent_linux_metadata_references_do_not_require_directory_listing_permission) {
    const ck::testing::ScratchDirectory scratch("files-parent-no-list"); NativeFileSystem filesystem;
    fs::create_directories(scratch.path() / "parent/child"); auto child = directory(filesystem, scratch.path() / "parent/child");
    CK_CHECK(geteuid() != 0);
    struct Restore { fs::path value; ~Restore() { (void)chmod(value.c_str(), 0700); } } restore{scratch.path() / "parent"};
    if (chmod((scratch.path() / "parent").c_str(), 0111) != 0) throw std::runtime_error("cannot set parent fixture permissions");
    auto parent = child->open_parent_directory();
    if (chmod((scratch.path() / "parent").c_str(), 0700) != 0) throw std::runtime_error("cannot restore parent fixture permissions");
    CK_CHECK(parent.value() && take(take(parent.value()->get()->inspect_basic()).kind) == ObjectKind::Directory);
}
#endif
