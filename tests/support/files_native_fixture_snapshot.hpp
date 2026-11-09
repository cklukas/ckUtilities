// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#if defined(__APPLE__)
#include "operation_journal_file.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <sys/stat.h>
namespace ck::testing {
// Observations only: these values never grant replay, cleanup or publication authority.
struct FilesFixtureEntry final {
    std::uint64_t device = 0, inode = 0;
    std::uint32_t mode = 0;
    std::int64_t modified_seconds = 0, changed_seconds = 0;
    long modified_nanos = 0, changed_nanos = 0;
    std::string bytes;
    friend bool operator==(const FilesFixtureEntry&, const FilesFixtureEntry&) = default;
};
using FilesFixtureTree = std::map<std::string, FilesFixtureEntry>;
inline FilesFixtureTree observe_files_fixture_tree(const std::filesystem::path& root) {
    FilesFixtureTree result;
    const auto observe = [&](const std::filesystem::path& path) {
        struct stat native{};
        if (::lstat(path.c_str(), &native) != 0 || (!S_ISDIR(native.st_mode) && !S_ISREG(native.st_mode)))
            throw std::runtime_error("cannot observe controlled file fixture");
        std::string bytes;
        if (S_ISREG(native.st_mode)) {
            std::ifstream input(path, std::ios::binary);
            if (!input) throw std::runtime_error("cannot read controlled file fixture");
            bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            if (input.bad()) throw std::runtime_error("cannot complete controlled file fixture read");
        }
        result.emplace(path == root ? "." : path.lexically_relative(root).generic_string(), FilesFixtureEntry{
            static_cast<std::uint64_t>(native.st_dev), static_cast<std::uint64_t>(native.st_ino),
            static_cast<std::uint32_t>(native.st_mode), native.st_mtimespec.tv_sec, native.st_ctimespec.tv_sec,
            native.st_mtimespec.tv_nsec, native.st_ctimespec.tv_nsec, std::move(bytes)});
    };
    observe(root);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) observe(entry.path());
    return result;
}
inline ck::files::OperationJournal::Pointer read_files_fixture_journal(const std::filesystem::path& path) {
    using namespace ck::files;
    auto root = NativeAbsolutePath::from_host(path.parent_path());
    auto basename = NativeName::from_posix(path.filename().native());
    if (!root.value() || !basename) throw std::runtime_error("invalid fixture journal address");
    auto opened = open_native_operation_journal_file(*root.value(), *basename, false);
    if (!opened.value()) throw std::runtime_error("cannot inspect fixture journal");
    auto decoded = load_operation_journal(**opened.value());
    const auto closed = (*opened.value())->finish_io();
    if (!closed.value() || !*closed.value() || !decoded.value() || decoded.value()->tail)
        throw std::runtime_error("cannot finish fixture journal inspection");
    return std::move(decoded).take_value().journal;
}
} // namespace ck::testing
#endif
