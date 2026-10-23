// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#if defined(_WIN32)
#include "windows_junction.hpp"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template <class T>
const T& required(const Observation<T>& value) {
    if (!value.value()) {
        const auto& error = *value.unavailable_detail();
        std::cerr << "required child observation unavailable: " << static_cast<int>(error.state)
            << " " << error.error.operation << " " << error.error.code << std::endl;
        throw std::runtime_error("required child observation unavailable");
    }
    return *value.value();
}
void write_file(const fs::path& path, const std::string& bytes = "original") {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("cannot create child fixture");
}
NativeName name_for(const fs::path& path) {
#if defined(_WIN32)
    std::u16string units;
    for (const auto unit : path.native()) units.push_back(static_cast<char16_t>(unit));
    auto name = NativeName::from_windows(std::move(units));
#else
    auto name = NativeName::from_posix(path.native());
#endif
    if (!name) throw std::runtime_error("invalid child fixture basename");
    return *name;
}
std::unique_ptr<DirectoryCursor> directory(const NativeFileSystem& filesystem, const fs::path& path) {
    auto value = filesystem.open_directory(path);
    (void)required(value);
    return std::move(value).take_value();
}
std::unique_ptr<OpenedEntry> child(const DirectoryCursor& parent, const fs::path& name) {
    auto value = parent.open_child(name_for(name));
    (void)required(value);
    return std::move(value).take_value();
}
}

CK_TEST(native_child_open_stays_relative_to_the_original_directory_after_path_replacement) {
    const ck::testing::ScratchDirectory scratch("files-child-parent-pin");
    const auto parent_path = scratch.path() / "parent";
    const auto retired = scratch.path() / "renamed-parent";
    fs::create_directory(parent_path);
    write_file(parent_path / "same-name", "original");
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, parent_path);
    const auto original = filesystem.inspect_basic(parent_path / "same-name");
    fs::rename(parent_path, retired);
    fs::create_directory(parent_path);
    write_file(parent_path / "same-name", "replacement is longer");
    auto opened = child(*parent, "same-name");
    const auto observed = opened->inspect_basic();
    CK_CHECK(required(required(observed).logical_size) == 8);
    CK_CHECK(compare_identity(required(required(original).identity), required(required(observed).identity)) == IdentityComparison::Same);
    const auto replacement = filesystem.inspect_basic(parent_path / "same-name");
    CK_CHECK(compare_identity(required(required(replacement).identity), required(required(observed).identity)) == IdentityComparison::Different);
}

CK_TEST(native_opened_child_survives_rename_replacement_unlink_and_parent_destruction) {
    const ck::testing::ScratchDirectory scratch("files-child-object-pin");
    const auto path = scratch.path() / "child";
    write_file(path);
    std::unique_ptr<OpenedEntry> opened;
    Observation<ObjectIdentity> before;
    {
        NativeFileSystem filesystem;
        auto parent = directory(filesystem, scratch.path());
        opened = child(*parent, "child");
        before = required(opened->inspect_basic()).identity;
    }
    fs::rename(path, scratch.path() / "retired");
    write_file(path, "replacement is different");
    fs::remove(scratch.path() / "retired");
    const auto observed = opened->inspect_basic();
    CK_CHECK(required(required(observed).logical_size) == 8);
    CK_CHECK(compare_identity(required(before), required(required(observed).identity)) == IdentityComparison::Same);
}

CK_TEST(native_opened_child_metadata_is_live_and_does_not_claim_a_content_snapshot) {
    const ck::testing::ScratchDirectory scratch("files-child-live");
    write_file(scratch.path() / "child");
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, scratch.path());
    auto opened = child(*parent, "child");
    const auto before = opened->inspect_basic();
    write_file(scratch.path() / "child", "changed and longer");
    const auto after = opened->inspect_basic();
    CK_CHECK(required(required(before).logical_size) == 8);
    CK_CHECK(required(required(after).logical_size) == 18);
    CK_CHECK(compare_identity(required(required(before).identity), required(required(after).identity)) == IdentityComparison::Same);
}

CK_TEST(native_child_missing_and_foreign_name_encodings_preserve_explicit_errors) {
    const ck::testing::ScratchDirectory scratch("files-child-errors");
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, scratch.path());
    const auto missing = parent->open_child(name_for("absent"));
    CK_CHECK(missing.state() == ObservationState::Gone && !missing.value());
#if defined(_WIN32)
    CK_CHECK(missing.unavailable_detail()->error.domain == ErrorDomain::NtStatus);
    CK_CHECK(missing.unavailable_detail()->error.code == 0xC0000034LL);
    const auto foreign = *NativeName::from_posix("child");
#else
    CK_CHECK(missing.unavailable_detail()->error.domain == ErrorDomain::Posix);
    const auto foreign = *NativeName::from_windows(u"child");
#endif
    const auto refused = parent->open_child(foreign);
    CK_CHECK(refused.state() == ObservationState::Failed && !refused.value());
    CK_CHECK(refused.unavailable_detail()->error.domain == ErrorDomain::Adapter);
}

CK_TEST(native_child_directory_and_hard_links_retain_native_object_classification_and_identity) {
    const ck::testing::ScratchDirectory scratch("files-child-kinds");
    fs::create_directory(scratch.path() / "directory");
    write_file(scratch.path() / "file");
    fs::create_hard_link(scratch.path() / "file", scratch.path() / "alias");
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, scratch.path());
    auto subdirectory = child(*parent, "directory");
    CK_CHECK(required(required(subdirectory->inspect_basic()).kind) == ObjectKind::Directory);
    auto file = child(*parent, "file");
    auto alias = child(*parent, "alias");
    CK_CHECK(compare_identity(required(required(file->inspect_basic()).identity), required(required(alias->inspect_basic()).identity)) == IdentityComparison::Same);
}

CK_TEST(native_child_opens_preserve_exact_unusual_native_name_units) {
    const ck::testing::ScratchDirectory scratch("files-child-names");
    auto spelling = fs::path("native-").native();
#if defined(_WIN32)
    spelling.push_back(static_cast<wchar_t>(0xD800));
#elif defined(__linux__)
    spelling.push_back(static_cast<char>(0xFF));
#else
    spelling += "\xC3\xA4";
#endif
    const fs::path name(spelling);
    write_file(scratch.path() / name);
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, scratch.path());
    auto opened = child(*parent, name);
    CK_CHECK(required(required(opened->inspect_basic()).logical_size) == 8);
}

#if !defined(_WIN32)
CK_TEST(native_posix_child_observation_pins_final_and_dangling_symlinks_and_does_not_read_fifos) {
    const ck::testing::ScratchDirectory scratch("files-child-links");
    write_file(scratch.path() / "target");
    fs::create_symlink("target", scratch.path() / "link");
    fs::create_symlink("absent", scratch.path() / "dangling");
    if (mkfifo((scratch.path() / "fifo").c_str(), 0600) != 0) throw std::runtime_error("cannot create child FIFO");
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, scratch.path());
    for (const auto* name : {"link", "dangling"}) {
        auto opened = child(*parent, name);
        CK_CHECK(required(required(opened->inspect_basic()).kind) == ObjectKind::SymbolicLink);
    }
    auto fifo = child(*parent, "fifo");
    CK_CHECK(required(required(fifo->inspect_basic()).kind) == ObjectKind::Fifo);
}

CK_TEST(native_posix_child_mode_zero_reports_the_platform_access_contract) {
    const ck::testing::ScratchDirectory scratch("files-child-mode-zero");
    const auto path = scratch.path() / "child";
    write_file(path);
    struct Restore {
        fs::path path;
        ~Restore() { chmod(path.c_str(), 0600); }
    } restore{path};
    if (chmod(path.c_str(), 0000) != 0) throw std::runtime_error("cannot set mode-zero child");
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, scratch.path());
    const auto opened = parent->open_child(name_for("child"));
#if defined(__linux__)
    const auto observed = required(opened)->inspect_basic();
    CK_CHECK(required(required(observed).posix_permissions) == 0);
    CK_CHECK(required(required(observed).logical_size) == 8);
#else
    CK_CHECK(opened.state() == ObservationState::Denied && !opened.value());
    CK_CHECK(opened.unavailable_detail()->error.domain == ErrorDomain::Posix);
    CK_CHECK(opened.unavailable_detail()->error.code == EACCES);
    // Denial must not be bypassed or silently replaced with an unpinned query.
    CK_CHECK(required(required(filesystem.inspect_basic(path)).posix_permissions) == 0);
#endif
}
#else
CK_TEST(native_windows_child_opens_reserved_and_trailing_names_literally_and_refuses_stream_syntax) {
    const ck::testing::ScratchDirectory scratch("files-child-literal");
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, scratch.path());
    auto preferred = scratch.path();
    preferred.make_preferred();
    for (const auto* name : {L"NUL", L"trailing. ", L"COM1.txt"}) {
        const fs::path path(std::wstring(L"\\\\?\\") + preferred.native() + L"\\" + name);
        write_file(path);
        auto opened = child(*parent, name);
        const auto absolute = filesystem.inspect_basic(path);
        CK_CHECK(compare_identity(required(required(absolute).identity), required(required(opened->inspect_basic()).identity)) == IdentityComparison::Same);
        // Remove via the same literal namespace; ordinary Win32 cleanup would
        // reinterpret these basenames and could leave the fixture behind.
        opened.reset();
        if (!DeleteFileW(path.c_str())) throw std::runtime_error("cannot remove literal child fixture");
    }
    write_file(scratch.path() / "file", "main data");
    write_file(scratch.path() / "file:stream", "additional channel");
    for (const auto* stream : {u"file:stream", u"file::$DATA"}) {
        const auto refused = parent->open_child(*NativeName::from_windows(stream));
        CK_CHECK(refused.state() == ObservationState::Failed && !refused.value());
        CK_CHECK(refused.unavailable_detail()->error.domain == ErrorDomain::Adapter);
        const auto absolute = filesystem.inspect_basic(scratch.path() / fs::path(stream));
        CK_CHECK(absolute.state() == ObservationState::Failed && !absolute.value());
        CK_CHECK(absolute.unavailable_detail()->error.domain == ErrorDomain::Adapter);
    }
}

CK_TEST(native_windows_child_opens_a_junction_itself_without_following_its_target) {
    const ck::testing::ScratchDirectory scratch("files-child-junction");
    const auto link = scratch.path() / "junction";
    fs::create_directory(scratch.path() / "target");
    fs::create_directory(link);
    ck::testing::make_junction(link, scratch.path() / "target");
    struct Remove {
        fs::path path;
        ~Remove() { RemoveDirectoryW(path.c_str()); }
    } remove{link};
    NativeFileSystem filesystem;
    auto parent = directory(filesystem, scratch.path());
    auto opened = child(*parent, "junction");
    CK_CHECK(required(required(opened->inspect_basic()).kind) == ObjectKind::Junction);
    const auto target = filesystem.inspect_basic(scratch.path() / "target");
    CK_CHECK(compare_identity(required(required(target).identity), required(required(opened->inspect_basic()).identity)) == IdentityComparison::Different);
}
#endif
