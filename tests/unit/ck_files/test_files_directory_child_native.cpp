// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#if defined(_WIN32)
#include "windows_junction.hpp"
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace {
using namespace ck::files; namespace fs = std::filesystem;
template<class T> const T& required(const Observation<T>& value) {
    if (!value.value()) {
        const auto& e = *value.unavailable_detail();
        std::cerr << "directory child: " << e.error.operation << " " << e.error.code << std::endl;
        throw std::runtime_error("required directory child observation unavailable");
    }
    return *value.value();
}
NativeName name(const fs::path& path) {
#if defined(_WIN32)
    std::u16string units; for (const auto unit : path.native()) units.push_back(static_cast<char16_t>(unit));
    auto result = NativeName::from_windows(std::move(units));
#else
    auto result = NativeName::from_posix(path.native());
#endif
    if (!result) throw std::runtime_error("invalid component fixture");
    return std::move(*result);
}
std::unique_ptr<DirectoryCursor> root(const NativeFileSystem& filesystem, const fs::path& path) {
    auto result = filesystem.open_directory(path); (void)required(result); return std::move(result).take_value();
}
std::unique_ptr<DirectoryCursor> child(const DirectoryCursor& directory, const NativeName& component) {
    auto result = directory.open_directory_child(component); (void)required(result); return std::move(result).take_value();
}
void file(const fs::path& path) {
    std::ofstream out(path); out << "fixture"; out.close(); if (!out) throw std::runtime_error("cannot write directory fixture");
}
std::set<std::string> names(DirectoryCursor& cursor) {
    std::set<std::string> result;
    for (int step = 0; step < 100; ++step) {
        const auto batch = cursor.read(1);
        for (const auto& component : batch.names) result.insert(component.display_name());
        if (batch.state == DirectoryReadState::End) return result;
        if (batch.state != DirectoryReadState::More) throw std::runtime_error("directory enumeration failed");
    }
    throw std::runtime_error("directory fixture enumeration exceeded budget");
}
#if defined(_WIN32)
std::wstring literal(const fs::path& path) {
    auto preferred = path; preferred.make_preferred(); const auto raw = preferred.native();
    if (raw.starts_with(LR"(\\?\)")) return raw;
    if (raw.starts_with(LR"(\\)")) return std::wstring(LR"(\\?\UNC\)") + raw.substr(2);
    return std::wstring(LR"(\\?\)") + raw;
}
#endif
// Private tree creation/cleanup only. The production traversal never joins paths.
class DeepTree final {
public:
    static constexpr std::size_t depth = 48;
    explicit DeepTree(const fs::path& path) {
#if defined(_WIN32)
        paths_.reserve(depth + 1); paths_.push_back(path);
#else
        descriptors_.reserve(depth + 1);
        const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) throw std::runtime_error("cannot open deep fixture root");
        descriptors_.push_back(fd);
#endif
    }
    ~DeepTree() { (void)cleanup(); }
    void append() {
#if defined(_WIN32)
        auto next = paths_.back() / component();
        paths_.push_back(std::move(next));
        if (!CreateDirectoryW(literal(paths_.back()).c_str(), nullptr)) {
            paths_.pop_back(); throw std::runtime_error("cannot create deep directory fixture");
        }
#else
        const auto spelling = component().native(); const auto fd = descriptors_.back();
        if (mkdirat(fd, spelling.c_str(), 0700) != 0) throw std::runtime_error("cannot create deep fixture component");
        const int opened = openat(fd, spelling.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (opened < 0) { unlinkat(fd, spelling.c_str(), AT_REMOVEDIR); throw std::runtime_error("cannot hold deep fixture child"); }
        descriptors_.push_back(opened);
#endif
    }
    bool cleanup() noexcept {
        bool success = true;
#if defined(_WIN32)
        while (paths_.size() > 1) {
            try { if (!RemoveDirectoryW(literal(paths_.back()).c_str())) success = false; }
            catch (...) { success = false; }
            paths_.pop_back();
        }
#else
        while (descriptors_.size() > 1) {
            close(descriptors_.back()); descriptors_.pop_back();
            const auto& spelling = spelling_;
            if (unlinkat(descriptors_.back(), spelling.c_str(), AT_REMOVEDIR) != 0) success = false;
        }
        if (!descriptors_.empty()) { close(descriptors_.back()); descriptors_.clear(); }
#endif
        return success;
    }
    static fs::path component() { return fs::path(std::string(100, 'd')); }
private:
#if defined(_WIN32)
    std::vector<fs::path> paths_;
#else
    const std::string spelling_ = std::string(100, 'd');
    std::vector<int> descriptors_;
#endif
};
}

CK_TEST(relative_directory_open_stays_with_held_parent_after_path_replacement_and_parent_destruction) {
    const ck::testing::ScratchDirectory scratch("files-dirchild-owner");
    const auto original = scratch.path() / "parent", retired = scratch.path() / "retired";
    fs::create_directories(original / "child"); file(original / "child" / "original");
    NativeFileSystem filesystem; auto directory = root(filesystem, original);
    fs::rename(original, retired); fs::create_directories(original / "child"); file(original / "child" / "replacement");
    auto opened = child(*directory, name("child"));
    CK_CHECK(compare_identity(required(opened->identity()), required(required(filesystem.inspect_basic(retired / "child")).identity)) == IdentityComparison::Same);
    directory.reset();
    fs::rename(retired / "child", retired / "held");
    fs::create_directory(retired / "child"); file(retired / "child" / "new-child");
    CK_CHECK(names(*opened) == std::set<std::string>{"original"});
}
CK_TEST(relative_directory_open_rejects_links_files_and_missing_names_without_following) {
    const ck::testing::ScratchDirectory scratch("files-dirchild-links");
    fs::create_directory(scratch.path() / "target"); file(scratch.path() / "target" / "sentinel"); file(scratch.path() / "file");
#if defined(_WIN32)
    fs::create_directory(scratch.path() / "link"); ck::testing::make_junction(scratch.path() / "link", scratch.path() / "target");
#else
    fs::create_directory_symlink("target", scratch.path() / "link");
    if (mkfifo((scratch.path() / "fifo").c_str(), 0600) != 0) throw std::runtime_error("cannot create FIFO fixture");
#endif
    NativeFileSystem filesystem; auto directory = root(filesystem, scratch.path());
    CK_CHECK(!directory->open_directory_child(name("link")).value());
    CK_CHECK(directory->open_directory_child(name("file")).state() == ObservationState::Failed);
    CK_CHECK(directory->open_directory_child(name("absent")).state() == ObservationState::Gone);
#if !defined(_WIN32)
    CK_CHECK(directory->open_directory_child(name("fifo")).state() == ObservationState::Failed);
#endif
    CK_CHECK(names(*child(*directory, name("target"))) == std::set<std::string>{"sentinel"});
}
CK_TEST(relative_directory_cursors_have_independent_enumeration_and_leave_parent_position_unchanged) {
    const ck::testing::ScratchDirectory scratch("files-dirchild-offset");
    fs::create_directory(scratch.path() / "child"); file(scratch.path() / "child" / "one"); file(scratch.path() / "child" / "two");
    NativeFileSystem filesystem; auto directory = root(filesystem, scratch.path());
    auto first = child(*directory, name("child")), second = child(*directory, name("child"));
    CK_CHECK((names(*first) == std::set<std::string>{"one", "two"}));
    CK_CHECK((names(*second) == std::set<std::string>{"one", "two"}));
    CK_CHECK(names(*directory) == std::set<std::string>{"child"});
}
CK_TEST(relative_directory_open_checks_encoding_and_keeps_unicode_components_lossless) {
    const ck::testing::ScratchDirectory scratch("files-dirchild-name");
    const auto component = fs::path(std::u8string(u8"directory-é-😀")); fs::create_directory(scratch.path() / component);
    NativeFileSystem filesystem; auto directory = root(filesystem, scratch.path()); auto opened = child(*directory, name(component));
    CK_CHECK(compare_identity(required(opened->identity()), required(required(filesystem.inspect_basic(scratch.path() / component)).identity)) == IdentityComparison::Same);
#if defined(_WIN32)
    CK_CHECK(directory->open_directory_child(*NativeName::from_posix("child")).state() == ObservationState::Failed);
    CK_CHECK(directory->open_directory_child(*NativeName::from_windows(u"child:stream")).state() == ObservationState::Failed);
#else
    CK_CHECK(directory->open_directory_child(*NativeName::from_windows(u"child")).state() == ObservationState::Failed);
#endif
}
CK_TEST(relative_directory_open_traverses_deep_tree_without_building_a_full_native_path) {
    const ck::testing::ScratchDirectory scratch("files-dirchild-deep"); DeepTree tree(scratch.path());
    NativeFileSystem filesystem; auto directory = root(filesystem, scratch.path());
    for (std::size_t level = 0; level < DeepTree::depth; ++level) {
        tree.append(); auto next = child(*directory, name(DeepTree::component())); directory = std::move(next);
    }
    CK_CHECK(names(*directory).empty());
    (void)required(directory->inspect_location()); directory.reset();
    CK_CHECK(tree.cleanup());
    std::cout << "relative directory traversal depth " << DeepTree::depth << " component bytes 100" << std::endl;
}

CK_TEST(relative_directory_open_preserves_unusual_native_components_without_display_round_trip) {
    const ck::testing::ScratchDirectory scratch("files-dirchild-raw");
#if defined(_WIN32)
    const fs::path component(std::wstring{L'r', L'a', L'w', L'-', wchar_t(0xd800)});
    if (!CreateDirectoryW(literal(scratch.path() / component).c_str(), nullptr)) throw std::runtime_error("cannot create raw directory fixture");
#elif defined(__linux__)
    const fs::path component(std::string("raw-\xff")); fs::create_directory(scratch.path() / component);
#else
    const fs::path component(std::string("raw-\\\n")); fs::create_directory(scratch.path() / component);
#endif
    NativeFileSystem filesystem; auto directory = root(filesystem, scratch.path());
    auto opened = child(*directory, name(component));
    CK_CHECK(compare_identity(required(opened->identity()), required(required(filesystem.inspect_basic(scratch.path() / component)).identity)) == IdentityComparison::Same);
    CK_CHECK(names(*opened).empty()); opened.reset(); directory.reset();
#if defined(_WIN32)
    if (!RemoveDirectoryW(literal(scratch.path() / component).c_str())) throw std::runtime_error("cannot remove raw directory fixture");
#endif
}
