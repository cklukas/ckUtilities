// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char *operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

bool valid_limits(OperationJournalLimits limits) noexcept {
    return limits.max_items && limits.max_items <= OperationJournalLimits::hard_max_items &&
        limits.max_records && limits.max_records <= OperationJournalLimits::hard_max_records &&
        limits.max_evidence_bytes <= OperationJournalLimits::hard_max_evidence_bytes;
}

bool valid_state(OperationJournalState state) noexcept {
    return static_cast<unsigned>(state) <=
        static_cast<unsigned>(OperationJournalState::NeedsReconciliation);
}

bool valid_id(const OperationJournalId &id) noexcept {
    return std::any_of(id.bytes.begin(), id.bytes.end(), [](std::byte value) { return value != std::byte{}; });
}

constexpr std::size_t fixed_body_bytes = 1 + 16 + sizeof(std::uint64_t) + sizeof(std::uint64_t) + 1 + sizeof(std::uint32_t);

void put_u32(std::vector<std::byte> &out, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(std::byte{static_cast<unsigned char>((value >> shift) & 0xffU)});
}

void put_u64(std::vector<std::byte> &out, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        out.push_back(std::byte{static_cast<unsigned char>((value >> shift) & 0xffU)});
}

std::uint32_t get_u32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t result = 0;
    for (unsigned index = 0; index != 4; ++index)
        result |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[index])) << (index * 8);
    return result;
}

std::uint64_t get_u64(std::span<const std::byte> bytes) noexcept {
    std::uint64_t result = 0;
    for (unsigned index = 0; index != 8; ++index)
        result |= static_cast<std::uint64_t>(std::to_integer<unsigned char>(bytes[index])) << (index * 8);
    return result;
}

std::uint32_t checksum(std::span<const std::byte> bytes) noexcept {
    std::uint32_t result = 2166136261U;
    for (const auto byte : bytes) {
        result ^= std::to_integer<unsigned char>(byte);
        result *= 16777619U;
    }
    return result;
}

UnavailableObservation journal_error(AdapterError code, const char *operation,
                                     ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

template <class T>
Observation<T> journal_rejected(const UnavailableObservation &detail) {
    return Observation<T>::unavailable(detail.state, detail.error);
}

} // namespace

bool operation_journal_terminal(OperationJournalState state) noexcept {
    return state == OperationJournalState::Complete || state == OperationJournalState::Failed ||
        state == OperationJournalState::Cancelled || state == OperationJournalState::RetainedSource ||
        state == OperationJournalState::NeedsReconciliation;
}

bool operation_journal_transition(FileOperation operation, OperationJournalState from,
                                  OperationJournalState to) noexcept {
    if (!valid_state(from) || !valid_state(to) || operation_journal_terminal(from))
        return false;
    if (to == OperationJournalState::Failed || to == OperationJournalState::Cancelled ||
        to == OperationJournalState::RetainedSource || to == OperationJournalState::NeedsReconciliation)
        return true;
    switch (from) {
    case OperationJournalState::Planned:
        return to == OperationJournalState::Prepared;
    case OperationJournalState::Prepared:
        return to == OperationJournalState::Copied ||
            (operation == FileOperation::Move && to == OperationJournalState::CommitIntended);
    case OperationJournalState::Copied:
        return to == OperationJournalState::Verified;
    case OperationJournalState::Verified:
        return to == OperationJournalState::CommitIntended;
    case OperationJournalState::CommitIntended:
        return to == OperationJournalState::DestinationCommitted;
    case OperationJournalState::DestinationCommitted:
        return operation == FileOperation::Copy ? to == OperationJournalState::Complete
                                                 : to == OperationJournalState::SourceRemovalIntended;
    case OperationJournalState::SourceRemovalIntended:
        return operation == FileOperation::Move && to == OperationJournalState::SourceRetired;
    case OperationJournalState::SourceRetired:
        return operation == FileOperation::Move && to == OperationJournalState::Complete;
    case OperationJournalState::Complete:
    case OperationJournalState::Failed:
    case OperationJournalState::Cancelled:
    case OperationJournalState::RetainedSource:
    case OperationJournalState::NeedsReconciliation:
        return false;
    }
    return false;
}

Observation<OperationJournal::Pointer> OperationJournal::create(FileOperation operation, OperationJournalId id,
                                                                OperationJournalLimits limits) {
    if (operation != FileOperation::Copy && operation != FileOperation::Move)
        return rejected<Pointer>(AdapterError::PlanInvalid, "operation_journal.operation");
    if (!valid_id(id))
        return rejected<Pointer>(AdapterError::UnqualifiedIdentity, "operation_journal.id");
    if (!valid_limits(limits))
        return rejected<Pointer>(AdapterError::InvalidLimit, "operation_journal.limits");
    return Observation<Pointer>::available(Pointer(new OperationJournal(operation, id, limits)));
}

Observation<bool> OperationJournal::append(OperationJournalRecord record) {
    auto valid = validate(record);
    if (!valid.value())
        return valid;
    try {
        records_.push_back(std::move(record));
    } catch (...) {
        return rejected<bool>(AdapterError::InventoryLimit, "operation_journal.append",
                              ObservationState::Unknown);
    }
    return Observation<bool>::available(true);
}

Observation<bool> OperationJournal::validate(const OperationJournalRecord &record) const {
    if (!valid_state(record.state))
        return rejected<bool>(AdapterError::PlanInvalid, "operation_journal.state");
    if (record.item >= limits_.max_items || record.evidence.size() > limits_.max_evidence_bytes)
        return rejected<bool>(AdapterError::InventoryLimit, "operation_journal.record_limit",
                              ObservationState::Unknown);
    if (records_.size() >= limits_.max_records)
        return rejected<bool>(AdapterError::InventoryLimit, "operation_journal.records",
                              ObservationState::Unknown);
    const auto expected_sequence = static_cast<std::uint64_t>(records_.size()) + 1;
    if (record.sequence != expected_sequence)
        return rejected<bool>(AdapterError::PlanInvalid, "operation_journal.sequence");
    const auto previous = state(record.item);
    if (previous) {
        if (!operation_journal_transition(operation_, *previous, record.state))
            return rejected<bool>(AdapterError::PlanInvalid, "operation_journal.transition");
    } else if (record.state != OperationJournalState::Planned) {
        return rejected<bool>(AdapterError::PlanInvalid, "operation_journal.first_state");
    }
    return Observation<bool>::available(true);
}

Observation<std::vector<std::byte>> encode_frame_unchecked(const OperationJournal &journal,
                                                           const OperationJournalRecord &record,
                                                           OperationJournalCodecLimits limits) {
    if (!limits.max_bytes || limits.max_bytes > OperationJournalCodecLimits::hard_max_bytes)
        return journal_rejected<std::vector<std::byte>>(
            journal_error(AdapterError::InvalidLimit, "operation_journal.codec_limits"));
    try {
        if (record.evidence.size() > std::numeric_limits<std::uint32_t>::max())
            return journal_rejected<std::vector<std::byte>>(
                journal_error(AdapterError::InventoryLimit, "operation_journal.evidence", ObservationState::Unknown));
        const auto body_size = fixed_body_bytes + record.evidence.size();
        if (body_size > std::numeric_limits<std::uint32_t>::max() ||
            body_size + 9 + sizeof(std::uint32_t) > limits.max_bytes)
            return journal_rejected<std::vector<std::byte>>(
                journal_error(AdapterError::InventoryLimit, "operation_journal.frame", ObservationState::Unknown));
        std::vector<std::byte> body;
        body.reserve(body_size);
        body.push_back(journal.operation() == FileOperation::Copy ? std::byte{0} : std::byte{1});
        body.insert(body.end(), journal.id().bytes.begin(), journal.id().bytes.end());
        put_u64(body, record.sequence);
        put_u64(body, static_cast<std::uint64_t>(record.item));
        body.push_back(std::byte{static_cast<unsigned char>(record.state)});
        put_u32(body, static_cast<std::uint32_t>(record.evidence.size()));
        body.insert(body.end(), record.evidence.begin(), record.evidence.end());
        std::vector<std::byte> encoded;
        encoded.reserve(9 + body.size() + sizeof(std::uint32_t));
        encoded.insert(encoded.end(), {std::byte{'C'}, std::byte{'K'}, std::byte{'F'}, std::byte{'J'}, std::byte{1}});
        put_u32(encoded, static_cast<std::uint32_t>(body.size()));
        encoded.insert(encoded.end(), body.begin(), body.end());
        put_u32(encoded, checksum(body));
        return Observation<std::vector<std::byte>>::available(std::move(encoded));
    } catch (const std::bad_alloc &) {
        return journal_rejected<std::vector<std::byte>>(
            journal_error(AdapterError::InventoryLimit, "operation_journal.encode", ObservationState::Unknown));
    }
}

Observation<std::vector<std::byte>> encode_operation_journal_frame(const OperationJournal &journal,
                                                                   const OperationJournalRecord &record,
                                                                   OperationJournalCodecLimits limits) {
    auto valid = journal.validate(record);
    if (!valid.value())
        return journal_rejected<std::vector<std::byte>>(*valid.unavailable_detail());
    return encode_frame_unchecked(journal, record, limits);
}

Observation<std::vector<std::byte>> encode_operation_journal(const OperationJournal &journal,
                                                              OperationJournalCodecLimits limits) {
    if (journal.records().empty())
        return journal_rejected<std::vector<std::byte>>(
            journal_error(AdapterError::PlanInvalid, "operation_journal.empty"));
    try {
        std::vector<std::byte> encoded;
        encoded.reserve(std::min<std::size_t>(limits.max_bytes, 4096));
        for (const auto &record : journal.records()) {
            auto frame = encode_frame_unchecked(journal, record, limits);
            if (!frame.value())
                return journal_rejected<std::vector<std::byte>>(*frame.unavailable_detail());
            if (encoded.size() > limits.max_bytes - frame.value()->size())
                return journal_rejected<std::vector<std::byte>>(
                    journal_error(AdapterError::InventoryLimit, "operation_journal.encoded", ObservationState::Unknown));
            encoded.insert(encoded.end(), frame.value()->begin(), frame.value()->end());
        }
        return Observation<std::vector<std::byte>>::available(std::move(encoded));
    } catch (const std::bad_alloc &) {
        return journal_rejected<std::vector<std::byte>>(
            journal_error(AdapterError::InventoryLimit, "operation_journal.encode", ObservationState::Unknown));
    }
}

Observation<bool> append_operation_journal(OperationJournal &journal, OperationJournalRecord record,
                                           std::uint64_t offset, OperationJournalSink sink,
                                           OperationJournalCodecLimits limits) {
    if (!sink.write_at || !sink.flush)
        return rejected<bool>(AdapterError::InvalidAccess, "operation_journal.sink");
    std::uint64_t expected_offset = 0;
    if (!journal.records().empty()) {
        auto existing = encode_operation_journal(journal, limits);
        if (!existing.value())
            return journal_rejected<bool>(*existing.unavailable_detail());
        expected_offset = existing.value()->size();
    }
    if (offset != expected_offset)
        return rejected<bool>(AdapterError::InvalidAccess, "operation_journal.offset");
    auto frame = encode_operation_journal_frame(journal, record, limits);
    if (!frame.value())
        return journal_rejected<bool>(*frame.unavailable_detail());
    std::size_t written = 0;
    while (written != frame.value()->size()) {
        if (offset > std::numeric_limits<std::uint64_t>::max() - written)
            return rejected<bool>(AdapterError::InvalidLimit, "operation_journal.offset");
        auto result = sink.write_at(offset + written,
                                    std::span<const std::byte>(frame.value()->data() + written,
                                                                 frame.value()->size() - written));
        if (!result.value())
            return journal_rejected<bool>(*result.unavailable_detail());
        if (!*result.value() || *result.value() > frame.value()->size() - written)
            return rejected<bool>(AdapterError::InvalidNativeResult, "operation_journal.write_count");
        written += *result.value();
    }
    auto flushed = sink.flush();
    if (!flushed.value())
        return journal_rejected<bool>(*flushed.unavailable_detail());
    if (!*flushed.value())
        return rejected<bool>(AdapterError::InvalidNativeResult, "operation_journal.flush_result");
    return journal.append(std::move(record));
}

Observation<OperationJournalDecode> decode_operation_journal(std::span<const std::byte> bytes,
                                                              OperationJournalLimits journal_limits,
                                                              OperationJournalCodecLimits codec_limits) {
    if (!codec_limits.max_bytes || codec_limits.max_bytes > OperationJournalCodecLimits::hard_max_bytes)
        return journal_rejected<OperationJournalDecode>(
            journal_error(AdapterError::InvalidLimit, "operation_journal.codec_limits"));
    if (bytes.empty() || bytes.size() > codec_limits.max_bytes)
        return journal_rejected<OperationJournalDecode>(
            journal_error(bytes.empty() ? AdapterError::PlanInvalid : AdapterError::InventoryLimit,
                          "operation_journal.input", bytes.empty() ? ObservationState::Failed : ObservationState::Unknown));
    if (!valid_limits(journal_limits))
        return journal_rejected<OperationJournalDecode>(
            journal_error(AdapterError::InvalidLimit, "operation_journal.journal_limits"));
    std::size_t offset = 0;
    OperationJournal::Pointer journal;
    std::optional<UnavailableObservation> tail;
    const auto report_tail = [&](UnavailableObservation detail) { tail = std::move(detail); };
    while (offset < bytes.size()) {
        const auto remaining = bytes.size() - offset;
        if (remaining < 9) {
            report_tail(journal_error(AdapterError::InvalidNativeResult, "operation_journal.truncated_header",
                                      ObservationState::Unknown));
            break;
        }
        const auto frame = bytes.subspan(offset);
        const bool magic = frame[0] == std::byte{'C'} && frame[1] == std::byte{'K'} &&
            frame[2] == std::byte{'F'} && frame[3] == std::byte{'J'};
        if (!magic || frame[4] != std::byte{1}) {
            const auto detail = journal_error(AdapterError::InvalidNativeResult,
                                              "operation_journal.frame_header");
            if (!journal)
                return journal_rejected<OperationJournalDecode>(detail);
            report_tail(detail);
            break;
        }
        const auto body_size = static_cast<std::size_t>(get_u32(frame.subspan(5, 4)));
        if (body_size < fixed_body_bytes || body_size > fixed_body_bytes + journal_limits.max_evidence_bytes) {
            const auto detail = journal_error(AdapterError::InventoryLimit, "operation_journal.frame_size",
                                              ObservationState::Unknown);
            if (!journal)
                return journal_rejected<OperationJournalDecode>(detail);
            report_tail(detail);
            break;
        }
        const auto frame_size = std::size_t{9} + body_size + sizeof(std::uint32_t);
        if (remaining < frame_size) {
            report_tail(journal_error(AdapterError::InvalidNativeResult, "operation_journal.truncated_record",
                                      ObservationState::Unknown));
            break;
        }
        const auto body = frame.subspan(9, body_size);
        if (get_u32(frame.subspan(9 + body_size, 4)) != checksum(body)) {
            const auto detail = journal_error(AdapterError::InvalidNativeResult, "operation_journal.checksum");
            if (!journal)
                return journal_rejected<OperationJournalDecode>(detail);
            report_tail(detail);
            break;
        }
        const auto operation = body[0] == std::byte{0} ? FileOperation::Copy :
            body[0] == std::byte{1} ? FileOperation::Move : static_cast<FileOperation>(255);
        OperationJournalId id;
        std::copy_n(body.begin() + 1, id.bytes.size(), id.bytes.begin());
        const auto sequence = get_u64(body.subspan(17, 8));
        const auto item_value = get_u64(body.subspan(25, 8));
        const auto state = static_cast<OperationJournalState>(std::to_integer<unsigned char>(body[33]));
        const auto evidence_size = static_cast<std::size_t>(get_u32(body.subspan(34, 4)));
        if (operation == static_cast<FileOperation>(255) || !valid_id(id) || !valid_state(state) ||
            evidence_size != body_size - fixed_body_bytes || item_value > std::numeric_limits<std::size_t>::max()) {
            const auto detail = journal_error(AdapterError::InvalidNativeResult, "operation_journal.record");
            if (!journal)
                return journal_rejected<OperationJournalDecode>(detail);
            report_tail(detail);
            break;
        }
        if (!journal) {
            auto created = OperationJournal::create(operation, id, journal_limits);
            if (!created.value())
                return journal_rejected<OperationJournalDecode>(*created.unavailable_detail());
            journal = std::move(created).take_value();
        } else if (journal->operation() != operation || journal->id() != id) {
            report_tail(journal_error(AdapterError::InvalidNativeResult, "operation_journal.identity"));
            break;
        }
        OperationJournalRecord record;
        record.sequence = sequence;
        record.item = static_cast<std::size_t>(item_value);
        record.state = state;
        record.evidence.assign(body.begin() + 38, body.end());
        auto appended = journal->append(std::move(record));
        if (!appended.value()) {
            const auto detail = *appended.unavailable_detail();
            if (journal->records().size() == 0)
                return journal_rejected<OperationJournalDecode>(detail);
            report_tail(detail);
            break;
        }
        offset += frame_size;
    }
    if (!journal)
        return journal_rejected<OperationJournalDecode>(
            journal_error(AdapterError::PlanInvalid, "operation_journal.no_records"));
    return Observation<OperationJournalDecode>::available({std::move(journal), std::move(tail)});
}

std::optional<OperationJournalState> OperationJournal::state(std::size_t item) const noexcept {
    for (auto it = records_.rbegin(); it != records_.rend(); ++it)
        if (it->item == item)
            return it->state;
    return std::nullopt;
}

bool OperationJournal::terminal(std::size_t item) const noexcept {
    const auto current = state(item);
    return current && operation_journal_terminal(*current);
}

} // namespace ck::files
