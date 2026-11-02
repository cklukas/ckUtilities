// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_batch_job.hpp"
#include "native_operation_copy_job.hpp"

#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

using namespace ck::files;

namespace {
struct MemoryCopy final {
    std::vector<std::byte> source{std::byte{'b'}, std::byte{'a'}, std::byte{'t'}, std::byte{'c'}, std::byte{'h'}};
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

OperationJournal::Pointer journal(std::byte id_byte) {
    OperationJournalId id;
    id.bytes[0] = id_byte;
    auto created = OperationJournal::create(FileOperation::Copy, id, {2, 32, 128});
    if (!created.value()) return {};
    auto value = std::move(created).take_value();
    if (!value->append({1, 0, OperationJournalState::Planned, {}}).value()) return {};
    return value;
}

Observation<OperationMetadataJob::Pointer> metadata() {
    OperationMetadataRequest request;
    request.apply_traversal_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    request.apply_security_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    request.apply_final_times = [](std::stop_token) { return Observation<bool>::available(true); };
    request.flush_destination = [](std::stop_token) { return Observation<bool>::available(true); };
    return OperationMetadataJob::create(std::move(request));
}

OperationExecutionJob::Pointer complete_job(MemoryCopy& memory, OperationJournal::Pointer value,
                                            unsigned& commits) {
    OperationCopyRequest copy_request{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [&memory](std::stop_token) { return StreamCopyJob::create(memory.request()); },
        [](std::stop_token) { return metadata(); },
        [&memory](std::stop_token) { return Observation<bool>::available(memory.destination == memory.source); },
        [value](OperationJournalRecord record) { return value->append(std::move(record)); },
        {},
    };
    auto copy = OperationCopyJob::create(std::move(copy_request));
    if (!copy.value()) return {};
    OperationExecutionRequest request;
    request.copy = std::move(copy).take_value();
    request.prepare_commit = [&commits, value](std::stop_token) {
        ++commits;
        OperationCommitRequest commit;
        commit.operation = FileOperation::Copy;
        commit.journal = value;
        commit.revalidate_source = [](std::stop_token) { return Observation<bool>::available(true); };
        commit.revalidate_destination = [](std::stop_token) { return Observation<bool>::available(true); };
        commit.verify_source_stability = [](std::stop_token) {
            return Observation<SourceStabilityGuarantee>::available(SourceStabilityGuarantee::Snapshot);
        };
        commit.commit_destination = [](std::stop_token) { return Observation<bool>::available(true); };
        commit.retire_source = [](std::stop_token) { return Observation<bool>::available(true); };
        commit.append_journal = [value](OperationJournalRecord record) { return value->append(std::move(record)); };
        return OperationCommitJob::create(std::move(commit));
    };
    auto created = OperationExecutionJob::create(std::move(request));
    return created.value() ? std::move(created).take_value() : OperationExecutionJob::Pointer{};
}
}

CK_TEST(operation_batch_job_serializes_items_and_preserves_partial_outcomes) {
    MemoryCopy first_memory;
    MemoryCopy third_memory;
    unsigned first_commits = 0;
    unsigned third_commits = 0;
    auto first = complete_job(first_memory, journal(std::byte{1}), first_commits);
    auto third = complete_job(third_memory, journal(std::byte{3}), third_commits);

    auto failed_journal = journal(std::byte{2});
    OperationCopyRequest failed_copy{
        0, failed_journal,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(false); },
        [](std::stop_token) { return Observation<StreamCopyJob::Pointer>::unavailable(ObservationState::Failed, {}); },
        [](std::stop_token) { return metadata(); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [failed_journal](OperationJournalRecord record) { return failed_journal->append(std::move(record)); },
        {},
    };
    auto failed_copy_job = OperationCopyJob::create(std::move(failed_copy));
    CK_CHECK(first && failed_copy_job.value() && third);
    if (!first || !failed_copy_job.value() || !third) return;
    OperationExecutionRequest failed_request;
    failed_request.copy = std::move(failed_copy_job).take_value();
    failed_request.prepare_commit = [](std::stop_token) {
        return Observation<OperationCommitJob::Pointer>::unavailable(ObservationState::Failed, {});
    };
    auto failed = OperationExecutionJob::create(std::move(failed_request));
    CK_CHECK(failed.value());
    if (!failed.value()) return;

    OperationBatchRequest request;
    request.jobs.push_back(std::move(first));
    request.jobs.push_back(std::move(failed).take_value());
    request.jobs.push_back(std::move(third));
    auto created = OperationBatchJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 80; ++step) {
        if (job->state() != OperationBatchState::Ready && job->state() != OperationBatchState::Running)
            break;
        job->advance();
    }
    CK_CHECK(job->state() == OperationBatchState::Failed);
    CK_CHECK(job->result().items.size() == 3);
    CK_CHECK(job->result().items[0].outcome == OperationItemOutcome::Complete);
    CK_CHECK(job->result().items[1].outcome == OperationItemOutcome::Failed);
    CK_CHECK(job->result().items[2].outcome == OperationItemOutcome::Complete);
    CK_CHECK(first_commits == 1 && third_commits == 1);
    const auto completed_cleanup = job->cleanup_item(0);
    CK_CHECK(!completed_cleanup.value() &&
             completed_cleanup.unavailable_detail()->error.code ==
                 static_cast<std::int64_t>(AdapterError::PlanDecisionStale));
    const auto missing_cleanup = job->cleanup_item(3);
    CK_CHECK(!missing_cleanup.value() &&
             missing_cleanup.unavailable_detail()->error.code ==
                 static_cast<std::int64_t>(AdapterError::InvalidIdentifier));
}

CK_TEST(operation_batch_job_cancellation_does_not_start_pending_items) {
    MemoryCopy first_memory;
    MemoryCopy second_memory;
    unsigned first_commits = 0;
    unsigned second_commits = 0;
    auto first = complete_job(first_memory, journal(std::byte{4}), first_commits);
    auto second = complete_job(second_memory, journal(std::byte{5}), second_commits);
    CK_CHECK(first && second);
    if (!first || !second) return;
    OperationBatchRequest request;
    request.jobs.push_back(std::move(first));
    request.jobs.push_back(std::move(second));
    auto created = OperationBatchJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    while (job->current_item() == 0 && job->state() != OperationBatchState::Complete)
        job->advance();
    CK_CHECK(job->current_item() == 1);
    std::stop_source stop;
    stop.request_stop();
    CK_CHECK(job->advance(stop.get_token()) == OperationBatchState::Cancelled);
    CK_CHECK(job->result().items[0].outcome == OperationItemOutcome::Complete);
    CK_CHECK(job->result().items[1].outcome == OperationItemOutcome::Pending);
    CK_CHECK(first_commits == 1 && second_commits == 0);
}

CK_TEST(native_operation_copy_batch_rejects_null_worker_jobs) {
    std::vector<NativeOperationCopyJob::Pointer> jobs;
    jobs.push_back({});
    const auto result = create_native_operation_copy_batch(std::move(jobs));
    CK_CHECK(!result.value() && result.state() == ObservationState::Failed &&
        result.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::PlanInvalid));
}
