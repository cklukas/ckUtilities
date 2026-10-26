// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_journal.hpp"
#include "native_path.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace ck::files {

// A worker-owned durable byte file. The native adapter is responsible for
// opening an exclusively addressed, private regular file and for retaining
// the handle until the worker finishes. It must never reopen a path between
// size/read/write/flush calls.
class OperationJournalFile {
public:
    virtual ~OperationJournalFile() = default;
    virtual Observation<std::uint64_t> size() = 0;
    virtual Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> bytes) = 0;
    virtual Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> bytes) = 0;
    virtual Observation<bool> flush() = 0;
};

// Reads the bounded durable prefix and reports a corrupt/truncated tail while
// preserving every earlier verified record. A short read is never treated as
// an empty file or as successful recovery.
Observation<OperationJournalDecode> load_operation_journal(
    OperationJournalFile& file, OperationJournalLimits journal_limits = {},
    OperationJournalCodecLimits codec_limits = {});

// Appends one frame at the current durable end. The journal is changed only
// after all writes and flush succeed; callers must reconcile after failure.
Observation<bool> append_operation_journal_file(
    OperationJournalFile& file, OperationJournal& journal, OperationJournalRecord record,
    OperationJournalCodecLimits limits = {});

// Opens or creates one named durable journal below a caller-owned private
// directory. The basename is exact native data and is never presentation text.
// create_new=true refuses an existing file; false opens an existing file only.
Observation<std::unique_ptr<OperationJournalFile>> open_native_operation_journal_file(
    const NativeAbsolutePath& private_root, const NativeName& basename, bool create_new);

// Establish and verify the platform-private journal-root contract before
// workers open individual journals. This may tighten an app-owned directory's
// permissions/security descriptor, but never weakens the contract.
Observation<bool> prepare_native_operation_journal_root(const NativeAbsolutePath& private_root);

} // namespace ck::files
