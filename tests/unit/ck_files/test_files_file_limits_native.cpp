// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "object_inventory_job.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <stdexcept>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> result) {
    if (!result.value()) throw std::runtime_error("native file-limit observation unavailable");
    return std::move(result).take_value();
}
NativeName file_name() {
#ifdef _WIN32
    return *NativeName::from_windows(u"file");
#else
    return *NativeName::from_posix("file");
#endif
}
void write(const fs::path& path) {
    std::ofstream file(path); file << "fixture"; file.close();
    if (!file) throw std::runtime_error("file-limit fixture write failed");
}
#ifndef _WIN32
struct Descriptor {
    int value;
    explicit Descriptor(const fs::path& path, int flags) : value(open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW)) {
        if (value < 0) throw std::runtime_error("independent pathconf descriptor failed");
    }
    ~Descriptor() { close(value); }
};
void compare_limit(int fd, int name, const Observation<std::uint64_t>& result) {
    errno = 0; const auto value = fpathconf(fd, name); const auto code = errno;
    if (value > 0) CK_CHECK(result.value() && *result.value() == static_cast<std::uint64_t>(value));
    else if (value == -1) CK_CHECK(!result.value() && result.unavailable_detail()->error.domain == ErrorDomain::Posix && result.unavailable_detail()->error.code == code);
    else CK_CHECK(!result.value());
}
#endif
}
CK_TEST(native_file_limits_directory_reports_match_independent_queries_without_enumerating) {
    const ck::testing::ScratchDirectory scratch("files-native-file-limits");
    fs::create_directory(scratch.path()/"a"); fs::create_directory(scratch.path()/"b");
    NativeFileSystem filesystem; auto cursor = take(filesystem.open_directory(scratch.path()));
    const auto first = cursor->read(1); CK_CHECK(first.names.size() == 1);
    const auto info = take(cursor->inspect_location());
#ifdef _WIN32
    CK_CHECK(info.posix_file_size_bits.state() == ObservationState::Unsupported && info.posix_symlink_max.state() == ObservationState::Unsupported);
    CK_CHECK(assess_file_size_bits(1, info).state() == ObservationState::Unsupported);
#else
    Descriptor fd(scratch.path(), O_RDONLY | O_DIRECTORY);
    compare_limit(fd.value, _PC_FILESIZEBITS, info.posix_file_size_bits);
    compare_limit(fd.value, _PC_SYMLINK_MAX, info.posix_symlink_max);
    if (info.posix_file_size_bits.value()) CK_CHECK(assess_file_size_bits(0, info).value());
#endif
    const auto rest = cursor->read(10);
    CK_CHECK(rest.state == DirectoryReadState::End && rest.names.size() == 1 && first.names[0] != rest.names[0]);
    CK_CHECK(std::distance(fs::directory_iterator(scratch.path()), fs::directory_iterator{}) == 2);
}
CK_TEST(native_file_limits_link_query_pins_the_exact_inode_after_path_replacement_and_unlink) {
    const ck::testing::ScratchDirectory scratch("files-native-link-limit-held");
    const auto original = scratch.path()/"file", moved = scratch.path()/"moved"; write(original);
    NativeFileSystem filesystem; auto parent = take(filesystem.open_directory(scratch.path()));
    auto entry = take(parent->open_child(file_name())); const auto before = take(entry->inspect_basic());
    CK_CHECK(before.identity.value());
#ifndef _WIN32
    Descriptor independent(original, O_RDONLY);
#endif
    fs::rename(original, moved); write(original); fs::remove(moved);
    const auto report = entry->inspect_link_limits();
#ifdef _WIN32
    CK_CHECK(report.state() == ObservationState::Unsupported);
#else
    CK_CHECK(report.value()); const auto& limits = *report.value();
    CK_CHECK(limits.kind == ObjectKind::RegularFile && limits.family == FileLinkLimitFamily::PosixFpathconf);
    CK_CHECK(compare_identity(limits.owner, *before.identity.value()) == IdentityComparison::Same);
    CK_CHECK(compare_identity(limits.owner, *take(filesystem.inspect_basic(original)).identity.value()) == IdentityComparison::Different);
    CK_CHECK(*limits.before_link_count.value() == 0 && *limits.after_link_count.value() == 0);
    compare_limit(independent.value, _PC_LINK_MAX, limits.maximum_links);
#endif
    CK_CHECK(fs::file_size(original) == 7);
}
CK_TEST(native_file_limits_are_collected_in_the_object_inventory_with_owner_context) {
    const ck::testing::ScratchDirectory scratch("files-native-link-limit-inventory"); write(scratch.path()/"file");
    NativeFileSystem filesystem; auto parent = take(filesystem.open_directory(scratch.path()));
    ObjectInventoryJob job(take(parent->open_child(file_name())));
    for (int i = 0; i < 1000 && job.state() != ObjectInventoryJobState::Complete && job.state() != ObjectInventoryJobState::Failed; ++i) (void)job.advance();
    const auto result = take(job.completed());
#ifdef _WIN32
    CK_CHECK(result->data().link_limits.state() == ObservationState::Unsupported);
#else
    const auto& report = take(result->data().link_limits);
    CK_CHECK(report.kind == ObjectKind::RegularFile && *report.before_link_count.value() == 1);
    CK_CHECK(take(result->compare_owner(Observation<ObjectIdentity>::available(report.owner))) == IdentityComparison::Same);
#endif
}
#ifndef _WIN32
CK_TEST(native_file_limits_fifo_query_is_metadata_only_and_does_not_open_a_reader) {
    const ck::testing::ScratchDirectory scratch("files-native-link-limit-fifo");
    const auto path = scratch.path()/"fifo"; CK_CHECK(mkfifo(path.c_str(), 0600) == 0);
    NativeFileSystem filesystem; auto parent = take(filesystem.open_directory(scratch.path()));
    auto entry = take(parent->open_child(*NativeName::from_posix("fifo")));
    const auto report = take(entry->inspect_link_limits()); CK_CHECK(report.kind == ObjectKind::Fifo);
    CK_CHECK(fs::is_fifo(path));
}
#endif
