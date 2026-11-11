// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_volume_archive.hpp"
#include "object_inventory_job.hpp"

#include <cstddef>
#include <stop_token>

namespace ck::vision {

struct FilesVolumeArchiveSourceLimits final {
    std::size_t max_entries = 1'000'000;
    std::size_t max_depth = 1024;
    ck::files::ObjectInventoryLimits object{};
};

// Worker-only native source adapter. Capture traverses the held root with
// exact native names and retains that owner for the approval revalidation.
// The review model receives only the immutable archive document.
ck::files::Observation<FilesVolumeArchiveSourceCapture> capture_native_volume_archive_source(
    const FilesVolumeRecord &source, FilesVolumeArchiveConsistency consistency = FilesVolumeArchiveConsistency::Live,
    std::stop_token stop_token = {}, FilesVolumeArchiveSourceLimits limits = {});

} // namespace ck::vision
