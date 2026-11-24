// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_source.hpp"
#include "native_filesystem.hpp"
#include "ck/file_io.hpp"
#include "scratch_directory.hpp"

#include <cvision/testing/cktest.hpp>

#include <stop_token>

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
