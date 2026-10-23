// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "data_channels.hpp"
#include "storage_info.hpp"
#include <functional>
#include <stop_token>

namespace ck::files {
enum class DataLayoutFamily { Unknown, PosixSeekDataHole, WindowsAllocatedRanges };
struct FileDataRange {
    std::uint64_t offset = 0, length = 0;
    friend bool operator==(const FileDataRange&, const FileDataRange&) = default;
};
// Native reported ranges that MAY contain nonzero bytes in the regular file's
// main data. Not physical extents, actual nonzero data, clone/dedup relationships,
// exact allocation, a stable snapshot or proof that a full-range file is dense.
struct FileDataLayout {
    DataLayoutFamily family = DataLayoutFamily::Unknown;
    Observation<ObjectIdentity> owner;
    std::uint64_t logical_size = 0;
    std::vector<FileDataRange> ranges;
};
namespace detail {
// Next native reported range intersecting [offset, size), or an evidenced end.
// Windows may round a range beyond the request; successful ranges are clipped
// to the requested interval. Empty files still issue one native query at 0/0.
using DataRangeQuery = std::function<Observation<std::optional<FileDataRange>>(std::uint64_t offset, std::uint64_t size)>;
Observation<FileDataLayout> collect_data_layout(const DataRangeQuery& query, DataLayoutFamily family,
    Observation<ObjectIdentity> owner, std::uint64_t size, InventoryLimits limits, std::stop_token stop = {});
// Live identity/kind/size checks plus any available modification/change times;
// not a comparison of every metadata field or snapshot/content-stability proof.
Observation<bool> data_layout_metadata_unchanged(const BasicMetadata& before, const BasicMetadata& after);
Observation<bool> data_layout_access_guard(const BasicMetadata& metadata, StorageModel model);
}
} // namespace ck::files
