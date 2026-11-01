// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstddef>
#include <cstdint>
#include <span>

#include "fuzz_common.hpp"
#include "operation_journal.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    constexpr ck::files::OperationJournalLimits journal_limits{
        64, 256, 4 * 1024};
    constexpr ck::files::OperationJournalCodecLimits codec_limits{
        64 * 1024};
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(data), size);
    const auto decoded = ck::files::decode_operation_journal(bytes, journal_limits, codec_limits);
    if (!decoded.value())
        return 0;

    const auto &journal = decoded.value()->journal;
    ck::fuzz::require(static_cast<bool>(journal));
    ck::fuzz::require(journal->records().size() <= journal_limits.max_records);
    for (const auto &record : journal->records())
        ck::fuzz::require(record.item < journal_limits.max_items &&
                           record.evidence.size() <= journal_limits.max_evidence_bytes);
    if (decoded.value()->tail)
        ck::fuzz::require(!decoded.value()->tail->error.operation.empty());
    return 0;
}
