// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_journal_file.hpp"

#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <stdexcept>
#include <vector>

using namespace ck::files;

namespace {
OperationJournalId id() {
    OperationJournalId value;
    value.bytes[0] = std::byte{0x42};
    return value;
}

class MemoryJournalFile final : public OperationJournalFile {
public:
    std::vector<std::byte> bytes;
    unsigned flushes = 0;
    bool fail_flush = false;

    Observation<std::uint64_t> size() override {
        return Observation<std::uint64_t>::available(bytes.size());
    }
    Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> output) override {
        if (offset > bytes.size())
            return Observation<std::size_t>::unavailable(ObservationState::Gone,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "test.file_offset"});
        const auto count = std::min(output.size(), bytes.size() - static_cast<std::size_t>(offset));
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
        return Observation<std::size_t>::available(count);
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> input) override {
        if (offset > bytes.size()) bytes.resize(static_cast<std::size_t>(offset));
        if (offset + input.size() > bytes.size()) bytes.resize(static_cast<std::size_t>(offset + input.size()));
        std::copy(input.begin(), input.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        return Observation<std::size_t>::available(input.size());
    }
    Observation<bool> flush() override {
        ++flushes;
        if (fail_flush)
            return Observation<bool>::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.file_flush"});
        return Observation<bool>::available(true);
    }
};

OperationJournal::Pointer journal() {
    auto result = OperationJournal::create(FileOperation::Copy, id(), {2, 8, 32});
    if (!result.value()) throw std::runtime_error("journal fixture creation failed");
    return std::move(result).take_value();
}
}

CK_TEST(operation_journal_file_appends_flushes_and_reloads_durable_bytes) {
    MemoryJournalFile file;
    auto value = journal();
    CK_CHECK(append_operation_journal_file(file, *value,
        {1, 0, OperationJournalState::Planned, {}}).value());
    CK_CHECK(append_operation_journal_file(file, *value,
        {2, 0, OperationJournalState::Prepared, {std::byte{7}}}).value());
    CK_CHECK(file.flushes == 2 && file.bytes.size() > 0);
    auto loaded = load_operation_journal(file, {2, 8, 32});
    CK_CHECK(loaded.value() && loaded.value()->journal && !loaded.value()->tail);
    if (loaded.value() && loaded.value()->journal)
        CK_CHECK(loaded.value()->journal->records().size() == 2 &&
                 loaded.value()->journal->records()[1].evidence.size() == 1);
}

CK_TEST(operation_journal_file_does_not_publish_after_flush_failure) {
    MemoryJournalFile file;
    file.fail_flush = true;
    auto value = journal();
    const auto result = append_operation_journal_file(file, *value,
        {1, 0, OperationJournalState::Planned, {}});
    CK_CHECK(!result.value() && value->records().empty() && file.flushes == 1);
}
