// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "file_metadata.hpp"

namespace ck::files {
enum class StorageModel { Unknown, LinuxStatx, DarwinStatFlags, WindowsHandleAttributes };
struct NativeStorageFlags {
    std::uint64_t value = 0;
    std::uint64_t known_mask = 0;
};
enum class StorageFeature {
    Unknown, CompressionFlag, EncryptionFlag, SparseFlag, ImmutableFlag,
    AppendOnlyFlag, VerityFlag, DaxFlag, DatalessFlag, OfflineFlag,
    RecallOnDataAccessFlag, PinnedIntentFlag, UnpinnedIntentFlag,
    IntegrityStreamFlag, NoScrubFlag, ReparseFlag, HiddenFlag
};
enum class ReportedFlag { Clear, Set };
enum class DataAccessHint {
    NoRecallFlagReported, RecallFlagReported, ProviderIntentReported,
    ProviderTagReported, OpaqueReparseReported
};
struct WindowsCompressionDetails {
    std::uint64_t compressed_file_size = 0;
    std::uint16_t format = 0;
    std::uint8_t compression_unit_shift = 0;
    std::uint8_t chunk_shift = 0;
    std::uint8_t cluster_shift = 0;
};
struct StorageInfo {
    StorageModel model = StorageModel::Unknown;
    Observation<ObjectIdentity> owner;
    Observation<ObjectKind> kind;
    Observation<std::uint64_t> logical_size;
    Observation<std::uint64_t> allocated_size;
    Observation<NativeStorageFlags> flags;
    Observation<std::uint32_t> windows_reparse_tag;
    Observation<WindowsCompressionDetails> windows_compression;
};
// Exact reported native flags, not claims about physical compression, complete
// encryption, extent layout, clone relationships, deduplication or data locality.
// Directory compression/encryption flags can describe defaults for children.
Observation<StorageInfo> storage_from_basic(const BasicMetadata& metadata, StorageModel model);
Observation<ReportedFlag> reported_storage_flag(const StorageInfo& info, StorageFeature feature);
// NoRecallFlagReported is deliberately not "local" or permission to read data.
// Linux has no generic equivalent of the Windows recall / Darwin dataless flags.
Observation<DataAccessHint> data_access_hint(const StorageInfo& info);
// Blocks identified provider/recall cases. It never enables hydration or turns
// unidentified providers into a blanket no-hydration guarantee.
Observation<bool> provider_review_required(const BasicMetadata& metadata, StorageModel model);
} // namespace ck::files
