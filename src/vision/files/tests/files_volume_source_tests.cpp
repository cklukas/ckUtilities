// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_source.hpp"

#include <cvision/testing/cktest.hpp>

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
} // namespace

CK_TEST(native_volume_source_refuses_unqualified_capability_before_traversal) {
    const auto result = build_native_volume_archive_document(fixture(FilesVolumeArchiveSupport::Unknown));
    CK_CHECK(!result.value());
    CK_CHECK(result.state() == ObservationState::Unknown);
    CK_CHECK(result.unavailable_detail()->error.operation == "volume_archive.source.capability");
}

CK_TEST(native_volume_source_revalidates_identity_before_opening_mount) {
    const auto result = build_native_volume_archive_document(fixture(FilesVolumeArchiveSupport::LiveReadable));
    CK_CHECK(!result.value());
    CK_CHECK(result.state() == ObservationState::Changed);
    CK_CHECK(result.unavailable_detail()->error.operation == "volume_archive.source.revalidation");
}

CK_TEST(native_volume_source_does_not_emulate_snapshot_provider) {
    auto value = fixture(FilesVolumeArchiveSupport::SnapshotPreferred);
    const auto result = build_native_volume_archive_document(value, FilesVolumeArchiveConsistency::Snapshot);
    CK_CHECK(!result.value());
    CK_CHECK(result.state() == ObservationState::Unsupported);
    CK_CHECK(result.unavailable_detail()->error.operation == "volume_archive.source.snapshot_provider");
}
