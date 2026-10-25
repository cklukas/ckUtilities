// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal_file.hpp"

#include <algorithm>
#include <limits>

namespace ck::files {
namespace {
template <class T>
Observation<T> error(AdapterError code, const char* operation,
                     ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

Observation<bool> read_exact(OperationJournalFile& file, std::uint64_t offset,
                             std::span<std::byte> bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        auto result = file.read_at(offset + done, bytes.subspan(done));
        if (!result.value()) {
            const auto& detail = *result.unavailable_detail();
            return Observation<bool>::unavailable(detail.state, detail.error);
        }
        const auto count = *result.value();
        if (count == 0 || count > bytes.size() - done)
            return error<bool>(AdapterError::InvalidNativeResult, "journal_file.read_progress");
        done += count;
    }
    return Observation<bool>::available(true);
}

bool valid_journal_limits(OperationJournalLimits limits) noexcept {
    return limits.max_items && limits.max_items <= OperationJournalLimits::hard_max_items &&
        limits.max_records && limits.max_records <= OperationJournalLimits::hard_max_records &&
        limits.max_evidence_bytes <= OperationJournalLimits::hard_max_evidence_bytes;
}

bool valid_codec_limits(OperationJournalCodecLimits limits) noexcept {
    return limits.max_bytes && limits.max_bytes <= OperationJournalCodecLimits::hard_max_bytes;
}
}

Observation<OperationJournalDecode> load_operation_journal(
    OperationJournalFile& file, OperationJournalLimits journal_limits,
    OperationJournalCodecLimits codec_limits) {
    if (!valid_journal_limits(journal_limits) || !valid_codec_limits(codec_limits))
        return error<OperationJournalDecode>(AdapterError::InvalidLimit, "journal_file.load_limits");
    auto size = file.size();
    if (!size.value()) {
        const auto& detail = *size.unavailable_detail();
        return Observation<OperationJournalDecode>::unavailable(detail.state, detail.error);
    }
    if (*size.value() > codec_limits.max_bytes || *size.value() > std::numeric_limits<std::size_t>::max())
        return error<OperationJournalDecode>(AdapterError::InventoryLimit, "journal_file.load_size",
                                             ObservationState::Unknown);
    std::vector<std::byte> bytes(static_cast<std::size_t>(*size.value()));
    if (!bytes.empty()) {
        auto read = read_exact(file, 0, bytes);
        if (!read.value()) {
            const auto& detail = *read.unavailable_detail();
            return Observation<OperationJournalDecode>::unavailable(detail.state, detail.error);
        }
    }
    return decode_operation_journal(bytes, journal_limits, codec_limits);
}

Observation<bool> append_operation_journal_file(
    OperationJournalFile& file, OperationJournal& journal, OperationJournalRecord record,
    OperationJournalCodecLimits limits) {
    auto size = file.size();
    if (!size.value()) {
        const auto& detail = *size.unavailable_detail();
        return Observation<bool>::unavailable(detail.state, detail.error);
    }
    return append_operation_journal(
        journal, std::move(record), *size.value(),
        OperationJournalSink{
            [&file](std::uint64_t offset, std::span<const std::byte> bytes) {
                return file.write_at(offset, bytes);
            },
            [&file] { return file.flush(); }
        }, limits);
}

} // namespace ck::files
