// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "object_inventory_job.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <iostream>
#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#include "windows_junction.hpp"
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
constexpr std::uint64_t size = 8 * 1024 * 1024, first = 64 * 1024, second = 6 * 1024 * 1024;
template<class T> T take(Observation<T> value) {
    if (!value.value()) {
        const auto& e = *value.unavailable_detail(); std::cerr << "layout unavailable " << static_cast<int>(e.state) << ' ' << e.error.operation << ' ' << e.error.code << '\n';
        throw std::runtime_error("required native layout observation unavailable");
    }
    return std::move(value).take_value();
}
NativeName name(const char* text) {
#if defined(_WIN32)
    std::u16string units; for (const auto* p = text; *p; ++p) units.push_back(static_cast<char16_t>(*p)); return *NativeName::from_windows(std::move(units));
#else
    return *NativeName::from_posix(text);
#endif
}
void sparse(const fs::path& path) {
#if defined(_WIN32)
    struct Owner { HANDLE value; ~Owner() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } fd{
        CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW, 0, nullptr)};
    if (fd.value == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot create sparse layout fixture");
    DWORD returned = 0; LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(size);
    if (!DeviceIoControl(fd.value, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) || !SetFilePointerEx(fd.value, position, nullptr, FILE_BEGIN) || !SetEndOfFile(fd.value))
        throw std::runtime_error("cannot mark sparse layout fixture");
    for (const auto offset : {first, second}) {
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(fd.value, position, nullptr, FILE_BEGIN) || !WriteFile(fd.value, "x", 1, &returned, nullptr) || returned != 1)
            throw std::runtime_error("cannot write sparse layout fixture");
    }
    if (!FlushFileBuffers(fd.value)) throw std::runtime_error("cannot flush sparse layout fixture");
#else
    struct Owner { int value; ~Owner() { if (value >= 0) close(value); } } fd{::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
    if (fd.value < 0 || ftruncate(fd.value, static_cast<off_t>(size)) != 0 ||
        pwrite(fd.value, "x", 1, static_cast<off_t>(first)) != 1 || pwrite(fd.value, "x", 1, static_cast<off_t>(second)) != 1)
        throw std::runtime_error("cannot create sparse layout fixture");
#if defined(__APPLE__)
    // APFS may allocate a ftruncate extension. Punch only this known-zero,
    // aligned interval in our exclusively created fixture, outside both writes.
    fpunchhole_t punch{}; punch.fp_offset = 128 * 1024; punch.fp_length = 5 * 1024 * 1024 - punch.fp_offset;
    if (fcntl(fd.value, F_PUNCHHOLE, &punch) != 0) throw std::runtime_error("cannot punch APFS sparse fixture hole");
#endif
    if (fsync(fd.value) != 0) throw std::runtime_error("cannot flush sparse layout fixture");
#endif
}
std::unique_ptr<OpenedEntry> open(const NativeFileSystem& filesystem, const fs::path& parent, EntryAccess access = EntryAccess::MetadataAndLayout) {
    return take(take(filesystem.open_directory(parent))->open_child(name("file"), access));
}
bool covers(const FileDataLayout& layout, std::uint64_t offset) {
    for (const auto& range : layout.ranges) if (range.offset <= offset && offset - range.offset < range.length) return true;
    return false;
}
}
CK_TEST(native_layout_reports_ranges_covering_actual_sparse_file_data_without_claiming_exact_allocation) {
    const ck::testing::ScratchDirectory scratch("files-layout"); sparse(scratch.path() / "file"); NativeFileSystem filesystem;
    auto entry = open(filesystem, scratch.path()); const auto metadata = take(entry->inspect_basic());
    CK_CHECK(take(metadata.logical_size) == size && take(metadata.allocated_size) < size);
    const auto layout = take(entry->inspect_data_layout());
    CK_CHECK(layout.logical_size == size && covers(layout, first) && covers(layout, second));
    CK_CHECK(compare_identity(take(metadata.identity), take(layout.owner)) == IdentityComparison::Same);
    std::uint64_t end = 0, bytes = 0;
    for (const auto& range : layout.ranges) { CK_CHECK(range.length > 0 && range.offset >= end && range.length <= size - range.offset); end = range.offset + range.length; bytes += range.length; }
    CK_CHECK(bytes <= size);
    std::cout << "LAYOUT_QUALIFIED family=" << static_cast<int>(layout.family) << " ranges=" << layout.ranges.size() << " reported_bytes=" << bytes << " allocated_bytes=" << take(metadata.allocated_size) << '\n';
}
CK_TEST(native_layout_access_is_opt_in_and_cancelled_queries_and_limits_never_return_prefixes) {
    const ck::testing::ScratchDirectory scratch("files-layout"); sparse(scratch.path() / "file"); NativeFileSystem filesystem;
    auto metadata = open(filesystem, scratch.path(), EntryAccess::MetadataOnly);
    CK_CHECK(metadata->inspect_data_layout().state() == ObservationState::Denied);
    auto entry = open(filesystem, scratch.path()); std::stop_source stop; stop.request_stop();
    CK_CHECK(entry->inspect_data_layout({}, stop.get_token()).state() == ObservationState::Unknown);
    CK_CHECK(entry->inspect_data_layout({1, 1}).state() == ObservationState::Unknown);
    CK_CHECK(entry->inspect_data_layout({0, 1}).state() == ObservationState::Failed);
    CK_CHECK(entry->inspect_data_layout().value());
}
CK_TEST(native_layout_keeps_the_opened_object_when_its_original_name_is_replaced) {
    const ck::testing::ScratchDirectory scratch("files-layout"); sparse(scratch.path() / "file"); NativeFileSystem filesystem;
    auto entry = open(filesystem, scratch.path()); const auto original = take(entry->inspect_basic());
    fs::rename(scratch.path() / "file", scratch.path() / "retired"); std::ofstream replacement(scratch.path() / "file"); replacement << "new"; replacement.close();
    const auto layout = take(entry->inspect_data_layout());
    CK_CHECK(layout.logical_size == size && covers(layout, second));
    CK_CHECK(compare_identity(take(original.identity), take(layout.owner)) == IdentityComparison::Same);
    CK_CHECK(compare_identity(take(layout.owner), take(take(filesystem.inspect_basic(scratch.path() / "file")).identity)) == IdentityComparison::Different);
}
CK_TEST(native_layout_collects_into_a_pinned_inventory_with_current_owner_checks) {
    const ck::testing::ScratchDirectory scratch("files-layout"); sparse(scratch.path() / "file"); NativeFileSystem filesystem;
    ObjectInventoryJob job(open(filesystem, scratch.path()));
    for (int i = 0; i < 100 && job.state() != ObjectInventoryJobState::Complete && job.state() != ObjectInventoryJobState::Failed; ++i) (void)job.advance();
    const auto result = take(job.completed()); CK_CHECK(result->data().layout.value() && covers(*result->data().layout.value(), first));
    CK_CHECK(take(result->compare_owner(result->data().layout.value()->owner)) == IdentityComparison::Same);
}
CK_TEST(native_layout_regular_file_access_refuses_final_links_and_directories) {
    const ck::testing::ScratchDirectory scratch("files-layout"); NativeFileSystem filesystem;
    fs::create_directory(scratch.path() / "directory");
#if defined(_WIN32)
    fs::create_directory(scratch.path() / "link"); ck::testing::make_junction(scratch.path() / "link", scratch.path() / "directory");
#else
    fs::create_symlink("absent", scratch.path() / "link");
#endif
    auto parent = take(filesystem.open_directory(scratch.path()));
    CK_CHECK(parent->open_child(name("link"), EntryAccess::MetadataAndLayout).state() == ObservationState::Unsupported);
    CK_CHECK(parent->open_child(name("directory"), EntryAccess::MetadataAndLayout).state() == ObservationState::Unsupported);
#if !defined(_WIN32)
    if (mkfifo((scratch.path() / "fifo").c_str(), 0600) != 0) throw std::runtime_error("cannot create FIFO layout fixture");
    CK_CHECK(parent->open_child(name("fifo"), EntryAccess::MetadataAndLayout).state() == ObservationState::Unsupported);
#endif
}
CK_TEST(native_layout_empty_file_requires_a_successful_native_end_query) {
    const ck::testing::ScratchDirectory scratch("files-layout"); std::ofstream empty(scratch.path() / "file"); empty.close(); NativeFileSystem filesystem;
    const auto layout = take(open(filesystem, scratch.path())->inspect_data_layout());
    CK_CHECK(layout.logical_size == 0 && layout.ranges.empty());
}
#if defined(__linux__)
CK_TEST(native_layout_linux_read_denial_is_not_bypassed_by_a_metadata_only_handle) {
    const ck::testing::ScratchDirectory scratch("files-layout"); sparse(scratch.path() / "file"); NativeFileSystem filesystem;
    if (geteuid() == 0) throw std::runtime_error("layout permission qualification requires an unprivileged user");
    struct Restore { fs::path path; ~Restore() { chmod(path.c_str(), 0600); } } restore{scratch.path() / "file"};
    if (chmod(restore.path.c_str(), 0) != 0) throw std::runtime_error("cannot set layout permission fixture");
    auto parent = take(filesystem.open_directory(scratch.path()));
    CK_CHECK(parent->open_child(name("file"), EntryAccess::MetadataOnly).value());
    const auto denied = parent->open_child(name("file"), EntryAccess::MetadataAndLayout);
    CK_CHECK(denied.state() == ObservationState::Denied && !denied.value() && denied.unavailable_detail()->error.operation == "openat.layout_fd");
}
#elif defined(_WIN32)
CK_TEST(native_layout_windows_offline_review_blocks_queries_and_access_upgrade) {
    const ck::testing::ScratchDirectory scratch("files-layout"); const auto path = scratch.path() / "file";
    std::ofstream out(path); out << "local"; out.close(); NativeFileSystem filesystem;
    auto entry = open(filesystem, scratch.path()); const auto flags = GetFileAttributesW(path.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("cannot query offline layout fixture");
    struct Restore { fs::path path; DWORD flags; ~Restore() { SetFileAttributesW(path.c_str(), flags); } } restore{path, flags};
    if (!SetFileAttributesW(path.c_str(), flags | FILE_ATTRIBUTE_OFFLINE)) throw std::runtime_error("cannot set offline layout fixture");
    const auto refused = entry->inspect_data_layout(); CK_CHECK(refused.state() == ObservationState::Unknown);
    CK_CHECK(refused.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::ProviderReviewRequired));
    const auto upgrade = take(filesystem.open_directory(scratch.path()))->open_child(name("file"), EntryAccess::MetadataAndLayout);
    CK_CHECK(upgrade.state() == ObservationState::Unknown && !upgrade.value());
}
#endif
