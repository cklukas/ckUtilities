// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_consent.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ck::files {

// A journal state is a durable fact about one planned item. It is not an
// execution permission: every destructive adapter must still revalidate the
// plan, receipt and native identities at the point of use.
enum class OperationJournalState : std::uint8_t {
    Planned,
    Prepared,
    Copied,
    Verified,
    CommitIntended,
    DestinationCommitted,
    SourceRemovalIntended,
    SourceRetired,
    Complete,
    Failed,
    Cancelled,
    RetainedSource,
    NeedsReconciliation
};

struct OperationJournalLimits final {
    std::size_t max_items = 1'000'000;
    std::size_t max_records = 8'000'000;
    std::size_t max_evidence_bytes = 4 * 1024 * 1024;
    static constexpr std::size_t hard_max_items = 8'000'000;
    static constexpr std::size_t hard_max_records = 64'000'000;
    static constexpr std::size_t hard_max_evidence_bytes = 64 * 1024 * 1024;
};

struct OperationJournalRecord final {
    std::uint64_t sequence = 0;
    std::size_t item = 0;
    OperationJournalState state = OperationJournalState::Planned;
    // Opaque, bounded adapter evidence. The core preserves it but never
    // interprets paths, handles, digests or platform records as authority.
    std::vector<std::byte> evidence;
};

class OperationJournal final {
public:
    using Pointer = std::shared_ptr<OperationJournal>;

    static Observation<Pointer> create(FileOperation operation, OperationJournalLimits limits = {});

    FileOperation operation() const noexcept { return operation_; }
    const OperationJournalLimits& limits() const noexcept { return limits_; }
    const std::vector<OperationJournalRecord>& records() const noexcept { return records_; }

    // The first record for each item must be Planned. Every later record must
    // be the next legal state for that same item and sequences are global,
    // contiguous and strictly monotonic. Rejected records are not retained.
    Observation<bool> append(OperationJournalRecord record);

    std::optional<OperationJournalState> state(std::size_t item) const noexcept;
    bool terminal(std::size_t item) const noexcept;

private:
    OperationJournal(FileOperation operation, OperationJournalLimits limits) noexcept
        : operation_(operation), limits_(limits) {}

    FileOperation operation_;
    OperationJournalLimits limits_;
    std::vector<OperationJournalRecord> records_;
};

bool operation_journal_transition(FileOperation operation, OperationJournalState from,
                                  OperationJournalState to) noexcept;
bool operation_journal_terminal(OperationJournalState state) noexcept;

} // namespace ck::files
