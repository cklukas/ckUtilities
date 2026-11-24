// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_source.hpp"
#include "native_filesystem.hpp"
#include "ck/file_io.hpp"
#include "scratch_directory.hpp"

#include <cvision/testing/cktest.hpp>

#include <stop_token>

#ifdef __APPLE__
#include "sha256.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <membership.h>
#include <sys/acl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#include "windows_name.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace ck::files;
using namespace ck::vision;

FilesVolumeRecord fixture(FilesVolumeArchiveSupport support) {
    FilesVolumeRecord result;
    result.name = "fixture";
    result.mount_point = "/volume-archive-source-fixture-that-does-not-exist";
    result.filesystem = "fixturefs";
    result.identity = "fixture-volume";
    result.archive = {support, support == FilesVolumeArchiveSupport::SnapshotPreferred, {}};
#if defined(_WIN32)
    result.native_mount_units = u"C:\\volume-archive-source-fixture-that-does-not-exist";
#endif
    return result;
}

NativeAbsolutePath scratch_fixture_address() {
#if defined(_WIN32)
    return NativeAbsolutePath::from_windows(u"C:\\volume-source-scratch-fixture").take_value();
#else
    return NativeAbsolutePath::from_posix("/volume-source-scratch-fixture").take_value();
#endif
}
} // namespace

CK_TEST(native_volume_source_refuses_unqualified_capability_before_traversal) {
    const auto result = capture_native_volume_archive_source(fixture(FilesVolumeArchiveSupport::Unknown),
                                                             scratch_fixture_address());
    CK_CHECK(!result.value());
    CK_CHECK(result.state() == ObservationState::Unknown);
    CK_CHECK(result.unavailable_detail()->error.operation == "volume_archive.source.capability");
}

CK_TEST(native_volume_source_requires_a_bound_root_before_opening_mount) {
    const auto result = capture_native_volume_archive_source(fixture(FilesVolumeArchiveSupport::LiveReadable),
                                                             scratch_fixture_address());
    CK_CHECK(!result.value());
    CK_CHECK(result.state() == ObservationState::Changed);
    CK_CHECK(result.unavailable_detail()->error.operation == "volume_archive.source.unbound_mount");
}

CK_TEST(native_volume_source_does_not_emulate_snapshot_provider) {
    auto value = fixture(FilesVolumeArchiveSupport::SnapshotPreferred);
    const auto result = capture_native_volume_archive_source(value, scratch_fixture_address(),
                                                             FilesVolumeArchiveConsistency::Snapshot);
    CK_CHECK(!result.value());
    CK_CHECK(result.state() == ObservationState::Unsupported);
    CK_CHECK(result.unavailable_detail()->error.operation == "volume_archive.source.snapshot_provider");
}

CK_TEST(native_volume_source_honors_cancellation_before_revalidation) {
    auto value = fixture(FilesVolumeArchiveSupport::LiveReadable);
    std::stop_source stop;
    stop.request_stop();
    const auto result = capture_native_volume_archive_source(value, scratch_fixture_address(),
                                                             FilesVolumeArchiveConsistency::Live,
                                                             stop.get_token());
    CK_CHECK(!result.value());
    CK_CHECK(result.state() == ObservationState::Unknown);
    CK_CHECK(result.unavailable_detail()->error.operation == "volume_archive.source.cancelled");
}


CK_TEST(native_volume_source_retains_the_root_used_to_capture_file_bytes) {
    ck::testing::ScratchDirectory scratch("volume-source-root", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    std::filesystem::permissions(scratch.path(), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
#endif
    const auto parent = scratch.path() / std::string(140, 'a') / std::string(140, 'b');
    const auto source_path = parent / "source";
    const auto renamed = parent / "renamed";
    CK_CHECK(ck::io::create_directories(source_path));
    CK_CHECK(ck::io::write_file(source_path / "payload.txt", "captured bytes"));
    const auto route = NativeAbsolutePath::from_host(source_path);
    CK_CHECK(route.value());
    if (!route.value())
        return;
    NativeFileSystem filesystem;
    auto opened = filesystem.open_native_directory(*route.value());
    CK_CHECK(opened.value());
    if (!opened.value())
        return;
    const auto metadata = (*opened.value())->inspect_basic();
    CK_CHECK(metadata.value() && metadata.value()->identity.value() && metadata.value()->mount_id.value());
    if (!metadata.value() || !metadata.value()->identity.value() || !metadata.value()->mount_id.value())
        return;
    auto selected = fixture(FilesVolumeArchiveSupport::LiveReadable);
    selected.archive.root = {*metadata.value()->identity.value(), *metadata.value()->mount_id.value()};
    selected.mount_point = route.value()->display_text();
#if defined(_WIN32)
    selected.native_mount_units = *route.value()->windows_units();
#else
    selected.mount_point = *route.value()->posix_bytes();
#endif
    std::move(opened).take_value().reset();

    const auto scratch_root = NativeAbsolutePath::from_host(scratch.path());
    CK_CHECK(scratch_root.value());
    if (!scratch_root.value())
        return;
    auto result = capture_native_volume_archive_source(selected, *scratch_root.value());
    if (!result.value())
        throw std::runtime_error(result.unavailable_detail()->error.operation);
    auto captured = std::move(result).take_value();
    CK_CHECK(captured.document.entries.size() == 2);
    if (captured.document.entries.size() == 2) {
        CK_CHECK(captured.document.entries[1].logical_size == 14);
        const auto &extents = captured.document.entries[1].extents;
        CK_CHECK(extents.size() == 1 && extents.front().bytes.empty() && extents.front().streamed_length == 14);
    }
    CK_CHECK(captured.payload.scratch);
    if (captured.payload.scratch) {
        const auto stored = captured.payload.scratch->size();
        CK_CHECK(stored.value() && *stored.value() == 14);
    }
    const auto live = captured.revalidate({});
    CK_CHECK(live.value() && *live.value());

#if defined(_WIN32)
    const auto from = windows_extended_path(source_path.native());
    const auto to = windows_extended_path(renamed.native());
    CK_CHECK(from && to && MoveFileExW(from->c_str(), to->c_str(), 0));
#else
    std::filesystem::rename(source_path, renamed);
#endif
    CK_CHECK(ck::io::create_directories(source_path));
    const auto stale = captured.revalidate({});
    CK_CHECK(!stale.value() && stale.state() == ObservationState::Changed);
    if (stale.unavailable_detail())
        CK_CHECK(stale.unavailable_detail()->error.operation == "volume_archive.source.mount_changed");
}

#ifdef __APPLE__
CK_TEST(native_apfs_archive_source_preserves_sparse_content_and_reports_metadata_losses) {
    ck::testing::ScratchDirectory scratch("volume-source-apfs", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    CK_CHECK(::chmod(scratch.path().c_str(), 0700) == 0);
    struct statfs fs{};
    if (::statfs(scratch.path().c_str(), &fs) != 0)
        throw std::runtime_error("cannot verify APFS archive fixture");
    if (std::string_view(fs.f_fstypename) != "apfs" || ::geteuid() == 0) {
        std::fputs("APFS archive coverage requires an ordinary user and an APFS fixture\n", stderr);
        return;
    }
    const auto source_path = scratch.path() / "volume";
    const auto file_path = source_path / "sparse";
    const auto alias_path = source_path / "alias";
    const auto symlink_path = source_path / "pointer";
    CK_CHECK(::mkdir(source_path.c_str(), 0700) == 0);
    constexpr std::size_t logical_size = 8 * 1024 * 1024;
    const int fd = ::open(file_path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot open APFS sparse fixture");
    CK_CHECK(::pwrite(fd, "HEAD", 4, 0) == 4);
    CK_CHECK(::pwrite(fd, "TAIL", 4, logical_size - 4) == 4);
    // APFS may allocate the whole gap on an extending write. Create a
    // physical hole explicitly and verify that the provider reports it.
    constexpr off_t edge = 64 * 1024;
    fpunchhole_t hole{.fp_offset = edge, .fp_length = logical_size - 2 * edge};
    CK_CHECK(::fcntl(fd, F_PUNCHHOLE, &hole) == 0);
    CK_CHECK(::fsetxattr(fd, "org.cktools.fixture", "attr", 4, 0, 0) == 0);
    CK_CHECK(::fsetxattr(fd, "com.apple.ResourceFork", "fork", 4, 0, 0) == 0);
    acl_t acl = ::acl_init(1);
    acl_entry_t entry{};
    acl_permset_t permissions{};
    uuid_t owner{};
    const bool acl_written = acl && ::mbr_uid_to_uuid(::getuid(), owner) == 0 &&
        ::acl_create_entry(&acl, &entry) == 0 && ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
        ::acl_set_qualifier(entry, owner) == 0 && ::acl_get_permset(entry, &permissions) == 0 &&
        ::acl_add_perm(permissions, ACL_READ_DATA) == 0 && ::acl_set_fd_np(fd, acl, ACL_TYPE_EXTENDED) == 0;
    if (acl)
        ::acl_free(acl);
    CK_CHECK(acl_written);
    CK_CHECK(::fsync(fd) == 0);
    const auto hole_start = ::lseek(fd, 0, SEEK_HOLE);
    const auto tail_start = hole_start > 0 ? ::lseek(fd, hole_start, SEEK_DATA) : -1;
    const auto end = tail_start > 0 ? ::lseek(fd, tail_start, SEEK_HOLE) : -1;
    struct stat sparse{};
    CK_CHECK(::fstat(fd, &sparse) == 0 && sparse.st_blocks > 0 &&
             sparse.st_blocks < static_cast<blkcnt_t>(logical_size / 1024));
    CK_CHECK(::close(fd) == 0);
    if (hole_start < 4 || tail_start - hole_start < static_cast<off_t>(logical_size / 2) ||
        tail_start > static_cast<off_t>(logical_size - 4) || end != static_cast<off_t>(logical_size))
        throw std::runtime_error("APFS fixture did not expose the expected sparse hole");
    CK_CHECK(::link(file_path.c_str(), alias_path.c_str()) == 0);
    CK_CHECK(::symlink("sparse", symlink_path.c_str()) == 0);

    const auto route = NativeAbsolutePath::from_host(source_path).take_value();
    const auto scratch_root = NativeAbsolutePath::from_host(scratch.path()).take_value();
    auto directory = NativeFileSystem{}.open_native_directory(route).take_value();
    const auto metadata = directory->inspect_basic().take_value();
    if (!metadata.identity.value() || !metadata.mount_id.value())
        throw std::runtime_error("unqualified APFS source identity");
    auto selected = fixture(FilesVolumeArchiveSupport::LiveReadable);
    selected.mount_point = *route.posix_bytes();
    selected.filesystem = "apfs";
    selected.archive.root = {*metadata.identity.value(), *metadata.mount_id.value()};
    auto result = capture_native_volume_archive_source(selected, scratch_root);
    if (!result.value())
        throw std::runtime_error(result.unavailable_detail()->error.operation);
    auto capture = std::move(result).take_value();
    const auto &entries = capture.document.entries;
    CK_CHECK(entries.size() == 4);
    const auto data = std::ranges::find(entries, FilesVolumeArchiveEntryKind::RegularFile, &FilesVolumeArchiveEntry::kind);
    const auto alias = std::ranges::find(entries, FilesVolumeArchiveEntryKind::HardLink, &FilesVolumeArchiveEntry::kind);
    const auto link = std::ranges::find(entries, FilesVolumeArchiveEntryKind::Symlink, &FilesVolumeArchiveEntry::kind);
    if (data == entries.end() || alias == entries.end() || link == entries.end() || !capture.payload.scratch)
        throw std::runtime_error("missing APFS archive data or link entry");
    CK_CHECK(data->hard_link_group != 0 && alias->hard_link_group == data->hard_link_group);
    CK_CHECK(link->path == "pointer" && link->link_target == "sparse");
    CK_CHECK(data->logical_size == logical_size && data->extents.size() == 2);
    if (data->extents.size() == 2) {
        CK_CHECK(data->extents[0].offset == 0 &&
                 data->extents[0].streamed_length == static_cast<std::uint64_t>(hole_start));
        CK_CHECK(data->extents[1].offset == static_cast<std::uint64_t>(tail_start) &&
                 data->extents[1].streamed_length == logical_size - static_cast<std::uint64_t>(tail_start));
    }
    std::vector<std::byte> restored(logical_size);
    std::uint64_t stored_bytes = 0;
    Sha256 stored_digest;
    for (const auto &extent : data->extents) {
        CK_CHECK(extent.bytes.empty());
        auto output = std::span(restored).subspan(static_cast<std::size_t>(extent.offset),
                                                  static_cast<std::size_t>(extent.streamed_length));
        const auto count = capture.payload.scratch->read_at(stored_bytes, output);
        CK_CHECK(count.value() && *count.value() == output.size());
        stored_digest.update(output);
        stored_bytes += output.size();
    }
    CK_CHECK(stored_bytes < restored.size() / 2);
    CK_CHECK(stored_digest.digest() == capture.payload.digest);
    std::vector<std::byte> expected(logical_size);
    const std::array head{std::byte{'H'}, std::byte{'E'}, std::byte{'A'}, std::byte{'D'}};
    const std::array tail{std::byte{'T'}, std::byte{'A'}, std::byte{'I'}, std::byte{'L'}};
    std::ranges::copy(head, expected.begin());
    std::ranges::copy(tail, expected.end() - tail.size());
    CK_CHECK(restored == expected);
    const auto loss_reported = [&](FilesVolumeArchiveLossKind kind, std::string_view diagnostic) {
        return std::ranges::any_of(capture.document.losses, [&](const FilesVolumeArchiveLoss &loss) {
            return loss.path == data->path && loss.kind == kind && !loss.hard_failure &&
                loss.diagnostic.find(diagnostic) != std::string::npos;
        });
    };
    CK_CHECK(loss_reported(FilesVolumeArchiveLossKind::AdditionalChannel, "additional data channels"));
    CK_CHECK(loss_reported(FilesVolumeArchiveLossKind::Metadata, "extended attributes"));
    CK_CHECK(loss_reported(FilesVolumeArchiveLossKind::Security, "ACL"));
    const auto review = FilesVolumeArchiveReview::create(capture.document, scratch_root, "apfs.ckarchive",
                                                          FilesVolumeArchiveDestinationState::Ready, true);
    CK_CHECK(review.value() && (*review.value())->summary().hard_losses == 0);
    CK_CHECK(review.value() && !(*review.value())->approve(FilesVolumeArchiveReviewDecision::Proceed).value());
    CK_CHECK(capture.revalidate({}).value());
    std::fprintf(stderr, "APFS ordinary-user capture stored %llu of %zu bytes\n",
                 static_cast<unsigned long long>(stored_bytes), logical_size);

    const auto denied_path = source_path / "read-denied";
    CK_CHECK(ck::io::write_file(denied_path, "unreadable payload"));
    CK_CHECK(::chmod(denied_path.c_str(), 0) == 0);
    const auto denied = capture_native_volume_archive_source(selected, scratch_root);
    CK_CHECK(::chmod(denied_path.c_str(), 0600) == 0);
    CK_CHECK(!denied.value() && denied.state() == ObservationState::Denied);
}
#endif
