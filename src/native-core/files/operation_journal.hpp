// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_consent.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

struct OperationJournalId final {
    std::array<std::byte, 16> bytes{};
    friend bool operator==(const OperationJournalId&, const OperationJournalId&) = default;
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

    static Observation<Pointer> create(FileOperation operation, OperationJournalId id,
                                       OperationJournalLimits limits = {});

    FileOperation operation() const noexcept { return operation_; }
    const OperationJournalId& id() const noexcept { return id_; }
    const OperationJournalLimits& limits() const noexcept { return limits_; }
    const std::vector<OperationJournalRecord>& records() const noexcept { return records_; }

    // The first record for each item must be Planned. Every later record must
    // be the next legal state for that same item and sequences are global,
    // contiguous and strictly monotonic. Rejected records are not retained.
    Observation<bool> validate(const OperationJournalRecord& record) const;
    Observation<bool> append(OperationJournalRecord record);

    std::optional<OperationJournalState> state(std::size_t item) const noexcept;
    bool terminal(std::size_t item) const noexcept;

private:
    OperationJournal(FileOperation operation, OperationJournalId id, OperationJournalLimits limits) noexcept
        : operation_(operation), id_(id), limits_(limits) {}

    FileOperation operation_;
    OperationJournalId id_;
    OperationJournalLimits limits_;
    std::vector<OperationJournalRecord> records_;
};

bool operation_journal_transition(FileOperation operation, OperationJournalState from,
                                  OperationJournalState to) noexcept;
bool operation_journal_terminal(OperationJournalState state) noexcept;

struct OperationJournalCodecLimits final {
    std::size_t max_bytes = 64 * 1024 * 1024;
    static constexpr std::size_t hard_max_bytes = 256 * 1024 * 1024;
};

struct OperationJournalDecode final {
    OperationJournal::Pointer journal;
    // A bounded, explicitly reported tail condition does not erase earlier
    // verified records. The caller must inspect/reconcile it before resume.
    std::optional<UnavailableObservation> tail;
};

Observation<std::vector<std::byte>> encode_operation_journal(const OperationJournal& journal,
                                                             OperationJournalCodecLimits limits = {});
Observation<std::vector<std::byte>> encode_operation_journal_frame(const OperationJournal& journal,
                                                                   const OperationJournalRecord& record,
                                                                   OperationJournalCodecLimits limits = {});

struct OperationJournalSink final {
    std::function<Observation<std::size_t>(std::uint64_t, std::span<const std::byte>)> write_at;
    std::function<Observation<bool>()> flush;
};

// The sink owns the durable file. The journal is updated only after every
// byte has been written and flush succeeds. A failed write/flush leaves the
// in-memory state unchanged and must be reconciled by the caller.
Observation<bool> append_operation_journal(OperationJournal& journal, OperationJournalRecord record,
                                           std::uint64_t offset, OperationJournalSink sink,
                                           OperationJournalCodecLimits limits = {});

Observation<OperationJournalDecode> decode_operation_journal(std::span<const std::byte> bytes,
                                                             OperationJournalLimits journal_limits = {},
                                                             OperationJournalCodecLimits codec_limits = {});

} // namespace ck::files
