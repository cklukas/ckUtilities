// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_entry_probe.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#if defined(__linux__) && defined(CK_FILES_REQUIRE_CASEFOLD)
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native probe fixture observation unavailable");
    return std::move(value).take_value();
}
NativeName name(const char* text) {
#ifdef _WIN32
    std::u16string units; while (*text) units.push_back(static_cast<char16_t>(static_cast<unsigned char>(*text++)));
    return *NativeName::from_windows(std::move(units));
#else
    return *NativeName::from_posix(text);
#endif
}
void write(const fs::path& path, const char* text) {
    std::ofstream output(path, std::ios::binary); output << text; output.close();
    if (!output) throw std::runtime_error("cannot write native probe fixture");
}
std::shared_ptr<const DirectoryCursor> cursor(const NativeFileSystem& filesystem, const fs::path& path) {
    return std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(path)));
}
}
CK_TEST(native_entry_probe_retains_existing_entry_and_reports_missing_without_reservation) {
    const ck::testing::ScratchDirectory scratch("files-entry-probe-native"); NativeFileSystem filesystem;
    write(scratch.path() / "exists", "abc"); auto parent = cursor(filesystem, scratch.path());
    auto existing = take(DirectoryEntryProbe::inspect(parent, name("exists")));
    CK_CHECK(existing->lookup().value() && existing->lookup().value()->state() == ChildLookupState::Found);
    CK_CHECK(take(take(existing->existing_metadata()).logical_size) == 3);
    const auto bound = take(existing->length()); CK_CHECK(!bound.exceeds_reported_maximum());
    auto missing = take(DirectoryEntryProbe::inspect(parent, name("new")));
    CK_CHECK(missing->lookup().value() && missing->lookup().value()->state() == ChildLookupState::Missing && missing->lookup().value()->missing_witness());
    CK_CHECK(missing->existing_metadata().state() == ObservationState::Unsupported);
    write(scratch.path() / "new", "created later"); auto fresh = take(missing->refresh());
    CK_CHECK(fresh->lookup().value() && fresh->lookup().value()->state() == ChildLookupState::Found);
    CK_CHECK(missing->lookup().value()->state() == ChildLookupState::Missing);
}
CK_TEST(native_entry_probe_refresh_uses_the_held_parent_after_path_replacement) {
    const ck::testing::ScratchDirectory scratch("files-entry-probe-parent"); NativeFileSystem filesystem;
    const auto original = scratch.path() / "directory", retired = scratch.path() / "retired";
    fs::create_directory(original); auto parent = cursor(filesystem, original);
    auto before = take(DirectoryEntryProbe::inspect(parent, name("a"))); parent.reset();
    fs::rename(original, retired); fs::create_directory(original);
    write(retired / "a", "old parent"); write(original / "a", "replacement");
    auto after = take(before->refresh());
    const auto observed = take(take(after->existing_metadata()).identity);
    CK_CHECK(compare_identity(observed, take(take(filesystem.inspect_basic(retired / "a")).identity)) == IdentityComparison::Same);
    CK_CHECK(compare_identity(observed, take(take(filesystem.inspect_basic(original / "a")).identity)) == IdentityComparison::Different);
}
CK_TEST(native_entry_probe_inspects_dangling_final_links_without_following_the_target) {
    const ck::testing::ScratchDirectory scratch("files-entry-probe-link"); NativeFileSystem filesystem;
    fs::create_symlink("absent", scratch.path() / "link");
    auto probe = take(DirectoryEntryProbe::inspect(cursor(filesystem, scratch.path()), name("link")));
    CK_CHECK(probe->lookup().value() && probe->lookup().value()->state() == ChildLookupState::Found);
    CK_CHECK(take(take(probe->existing_metadata()).kind) == ObjectKind::SymbolicLink);
}
CK_TEST(native_entry_probe_preserves_encoding_mismatch_errors_as_unavailable_observations) {
    const ck::testing::ScratchDirectory scratch("files-entry-probe-encoding"); NativeFileSystem filesystem;
#ifdef _WIN32
    auto foreign = *NativeName::from_posix("foreign");
#else
    auto foreign = *NativeName::from_windows(u"foreign");
#endif
    auto probe = take(DirectoryEntryProbe::inspect(cursor(filesystem, scratch.path()), std::move(foreign)));
    CK_CHECK(!probe->lookup().value() && !probe->existing_metadata().value() && !probe->length().value());
}
#if defined(__linux__) && defined(CK_FILES_REQUIRE_CASEFOLD)
CK_TEST(native_entry_probe_uses_real_casefold_lookup_on_the_required_private_volume) {
    const ck::testing::ScratchDirectory scratch("files-entry-probe-casefold");
    const int descriptor = open(scratch.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) throw std::runtime_error("cannot open casefold probe fixture");
    int flags = 0; const int queried = ioctl(descriptor, FS_IOC_GETFLAGS, &flags);
    flags |= FS_CASEFOLD_FL;
    const int changed = queried == 0 ? ioctl(descriptor, FS_IOC_SETFLAGS, &flags) : -1; close(descriptor);
    if (changed != 0) throw std::runtime_error("required casefold probe activation failed");
    write(scratch.path() / "MixedCase", "payload"); NativeFileSystem filesystem;
    auto probe = take(DirectoryEntryProbe::inspect(cursor(filesystem, scratch.path()), name("mixedcase")));
    CK_CHECK(probe->lookup().value() && probe->lookup().value()->state() == ChildLookupState::Found);
    CK_CHECK(take(reported_linux_directory_flag(take(probe->location()), LinuxDirectoryFeature::Casefold)) == ReportedFlag::Set);
    CK_CHECK(compare_identity(take(take(probe->existing_metadata()).identity),
        take(take(filesystem.inspect_basic(scratch.path() / "MixedCase")).identity)) == IdentityComparison::Same);
    std::cout << "ENTRY_PROBE_CASEFOLD_QUALIFIED" << std::endl;
}
#endif
