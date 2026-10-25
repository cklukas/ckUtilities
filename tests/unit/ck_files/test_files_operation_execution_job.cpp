// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_execution_job.hpp"

#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

using namespace ck::files;

namespace {
struct MemoryCopy final {
    std::vector<std::byte> source{std::byte{'e'}, std::byte{'x'}, std::byte{'e'}, std::byte{'c'}};
    std::vector<std::byte> destination;
    StreamCopyRequest request() {
        StreamCopyRequest result;
        result.limits.chunk_bytes = 2;
        result.limits.max_bytes = 64;
        result.read_source = [this](std::uint64_t offset, std::size_t limit, std::stop_token) {
            ContentRead value;
            const auto count = std::min(limit, source.size() - static_cast<std::size_t>(offset));
            value.bytes.assign(source.begin() + static_cast<std::ptrdiff_t>(offset),
                               source.begin() + static_cast<std::ptrdiff_t>(offset + count));
            value.complete = offset + count == source.size();
            return Observation<ContentRead>::available(std::move(value));
        };
        result.write_destination = [this](std::uint64_t offset, std::span<const std::byte> bytes, std::stop_token) {
            if (destination.size() < offset + bytes.size()) destination.resize(static_cast<std::size_t>(offset + bytes.size()));
            std::copy(bytes.begin(), bytes.end(), destination.begin() + static_cast<std::ptrdiff_t>(offset));
            return Observation<std::size_t>::available(bytes.size());
        };
        result.read_destination = [this](std::uint64_t offset, std::size_t limit, std::stop_token) {
            ContentRead value;
            const auto count = std::min(limit, destination.size() - static_cast<std::size_t>(offset));
            value.bytes.assign(destination.begin() + static_cast<std::ptrdiff_t>(offset),
                               destination.begin() + static_cast<std::ptrdiff_t>(offset + count));
            value.complete = offset + count == destination.size();
            return Observation<ContentRead>::available(std::move(value));
        };
        result.flush_destination = [](std::stop_token) { return Observation<bool>::available(true); };
        return result;
    }
};

Observation<OperationMetadataJob::Pointer> metadata() {
    OperationMetadataRequest request;
    request.apply_traversal_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    request.apply_security_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    request.apply_final_times = [](std::stop_token) { return Observation<bool>::available(true); };
    request.flush_destination = [](std::stop_token) { return Observation<bool>::available(true); };
    return OperationMetadataJob::create(std::move(request));
}

OperationJournal::Pointer journal() {
    OperationJournalId id;
    id.bytes[0] = std::byte{0x5a};
    auto created = OperationJournal::create(FileOperation::Copy, id, {2, 32, 128});
    if (!created.value()) return {};
    auto value = std::move(created).take_value();
    if (!value->append({1, 0, OperationJournalState::Planned, {}}).value()) return {};
    return value;
}
}

CK_TEST(operation_execution_job_does_not_construct_commit_until_copy_is_verified) {
    auto value = journal();
    MemoryCopy memory;
    unsigned commit_preparations = 0;
    unsigned publications = 0;
    OperationCopyRequest copy_request{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [&memory](std::stop_token) { return StreamCopyJob::create(memory.request()); },
        [](std::stop_token) { return metadata(); },
        [&memory](std::stop_token) { return Observation<bool>::available(memory.destination == memory.source); },
        [value](OperationJournalRecord record) { return value->append(std::move(record)); },
    };
    auto copy = OperationCopyJob::create(std::move(copy_request));
    CK_CHECK(copy.value());
    if (!copy.value()) return;
    OperationExecutionRequest request;
    request.copy = std::move(copy).take_value();
    request.prepare_commit = [&](std::stop_token) {
        ++commit_preparations;
        OperationCommitRequest commit;
        commit.operation = FileOperation::Copy;
        commit.journal = value;
        commit.revalidate_source = [](std::stop_token) { return Observation<bool>::available(true); };
        commit.revalidate_destination = [](std::stop_token) { return Observation<bool>::available(true); };
        commit.verify_source_stability = [](std::stop_token) {
            return Observation<SourceStabilityGuarantee>::available(SourceStabilityGuarantee::Snapshot);
        };
        commit.commit_destination = [&](std::stop_token) {
            ++publications;
            return Observation<bool>::available(true);
        };
        commit.retire_source = [](std::stop_token) { return Observation<bool>::available(true); };
        commit.append_journal = [value](OperationJournalRecord record) { return value->append(std::move(record)); };
        return OperationCommitJob::create(std::move(commit));
    };
    auto created = OperationExecutionJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 40 && job->state() != OperationExecutionState::Complete &&
         job->state() != OperationExecutionState::Failed; ++step) job->advance();
    CK_CHECK(job->state() == OperationExecutionState::Complete && commit_preparations == 1 && publications == 1);
    CK_CHECK(value->state(0) == OperationJournalState::Complete && memory.destination == memory.source);
}

CK_TEST(operation_execution_job_preserves_copy_reconciliation_without_commit_preparation) {
    auto value = journal();
    OperationCopyRequest copy_request{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) {
            return Observation<StreamCopyJob::Pointer>::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.copy_prepare"});
        },
        [](std::stop_token) { return metadata(); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [value](OperationJournalRecord record) { return value->append(std::move(record)); },
    };
    auto copy = OperationCopyJob::create(std::move(copy_request));
    CK_CHECK(copy.value());
    if (!copy.value()) return;
    OperationExecutionRequest request;
    request.copy = std::move(copy).take_value();
    request.prepare_commit = [](std::stop_token) {
        return Observation<OperationCommitJob::Pointer>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "test.commit_prepare"});
    };
    auto created = OperationExecutionJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 12 && job->state() != OperationExecutionState::NeedsReconciliation &&
         job->state() != OperationExecutionState::Failed; ++step) job->advance();
    CK_CHECK(job->state() == OperationExecutionState::NeedsReconciliation &&
             value->state(0) == OperationJournalState::Prepared);
}
