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

// Worker-only native source adapter. It revalidates the enumerated volume,
// opens the mount without following its final component, stays on the root
// mount, and returns a complete immutable archive document or an explicit
// unavailable observation. It never asks for elevation or unmounts a volume.
// Windows remains fail-closed until the archive has a native UTF-16 path
// codec; display UTF-8 is never used to reconstruct a source path.
ck::files::Observation<FilesVolumeArchiveDocument> build_native_volume_archive_document(
    const FilesVolumeRecord &source, FilesVolumeArchiveConsistency consistency = FilesVolumeArchiveConsistency::Live,
    std::stop_token stop_token = {}, FilesVolumeArchiveSourceLimits limits = {});

} // namespace ck::vision
