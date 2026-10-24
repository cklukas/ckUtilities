// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_name.hpp"
#include "file_metadata.hpp"
#include <span>
#include <vector>
namespace ck::files {
struct DirectoryIndexContext {
    // Trusted live owner context, never reconstructed from a stored record.
    AdapterScope source_scope;
    std::array<std::byte, 16> generation{}; // owner-supplied nonzero index generation
};
struct DirectoryIndexRecordLimits {
    std::size_t record_bytes = 256 * 1024; // complete wire record
    std::size_t name_bytes = 64 * 1024;
    std::size_t diagnostic_bytes = 16 * 1024; // aggregate operation strings
    static constexpr std::size_t hard_record_bytes = 1024 * 1024;
    static constexpr std::size_t hard_name_bytes = 1024 * 1024;
    static constexpr std::size_t hard_diagnostic_bytes = 256 * 1024;
};
struct DirectoryIndexRecord {
    std::uint64_t row_id;
    NativeName name;
    Observation<BasicMetadata> metadata;
    Observation<bool> hidden;
};
// Versioned, bounded worker-side codec for a private scratch index, never a
// recovery/journal or operation-authority format. No I/O, locale conversion,
// display parsing or serialization of pointers/native owners. Available IDs
// must belong to the supplied live source namespace; decode restores that
// namespace only after checking the expected generation. Cached IDs remain
// observations and cannot replace native acquisition/revalidation.
// CRC32C detects accidental corruption; it provides no authentication. The
// storage owner must separately enforce private ownership, quotas and lifetime.
Observation<std::vector<std::byte>> encode_directory_index_record(const DirectoryIndexRecord &record,
                                                                  const DirectoryIndexContext &context,
                                                                  DirectoryIndexRecordLimits limits = {});
Observation<DirectoryIndexRecord> decode_directory_index_record(std::span<const std::byte> wire,
                                                                const DirectoryIndexContext &context,
                                                                DirectoryIndexRecordLimits limits = {});
namespace detail {
std::uint32_t directory_index_crc32c(std::span<const std::byte> bytes) noexcept;
}
} // namespace ck::files
