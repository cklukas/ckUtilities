// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_copy_job.hpp"

#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

using namespace ck::files;

namespace {
struct MemoryCopy final {
    std::vector<std::byte> source{std::byte{'c'}, std::byte{'o'}, std::byte{'p'}, std::byte{'y'}};
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

OperationJournal::Pointer journal() {
    OperationJournalId id;
    id.bytes[0] = std::byte{0x42};
    auto created = OperationJournal::create(FileOperation::Copy, id, {2, 32, 128});
    if (!created.value()) return {};
    auto value = std::move(created).take_value();
    if (!value->append({1, 0, OperationJournalState::Planned, {}}).value()) return {};
    return value;
}

Observation<OperationMetadataJob::Pointer> no_op_metadata_job() {
    OperationMetadataRequest request;
    request.apply_traversal_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    request.apply_security_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    request.apply_final_times = [](std::stop_token) { return Observation<bool>::available(true); };
    request.flush_destination = [](std::stop_token) { return Observation<bool>::available(true); };
    return OperationMetadataJob::create(std::move(request));
}
}

CK_TEST(operation_copy_job_durably_orders_prepare_copy_and_verify) {
    auto value = journal();
    MemoryCopy memory;
    unsigned source_checks = 0;
    unsigned destination_checks = 0;
    unsigned preparations = 0;
    auto request = OperationCopyRequest{
        0, value,
        [&source_checks](std::stop_token) { ++source_checks; return Observation<bool>::available(true); },
        [&destination_checks](std::stop_token) { ++destination_checks; return Observation<bool>::available(true); },
        [&preparations, &memory](std::stop_token) {
            ++preparations;
            return StreamCopyJob::create(memory.request());
        },
        [](std::stop_token) { return no_op_metadata_job(); },
        [&memory](std::stop_token) { return Observation<bool>::available(memory.destination == memory.source); },
        [value](OperationJournalRecord record) { return value->append(std::move(record)); },
        {},
    };
    auto created = OperationCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 20 && job->state() != OperationCopyState::Verified &&
         job->state() != OperationCopyState::Failed; ++step) job->advance();
    CK_CHECK(job->state() == OperationCopyState::Verified && source_checks == 1 && destination_checks == 1 && preparations == 1);
    CK_CHECK(value->state(0) == OperationJournalState::Verified && memory.destination == memory.source);
}

CK_TEST(operation_copy_job_cancellation_before_prepare_is_journaled_without_native_prepare) {
    auto value = journal();
    unsigned preparations = 0;
    auto request = OperationCopyRequest{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [&preparations](std::stop_token) {
            ++preparations;
            return Observation<StreamCopyJob::Pointer>::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "test.prepare"});
        },
        [](std::stop_token) { return no_op_metadata_job(); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [value](OperationJournalRecord record) { return value->append(std::move(record)); },
        {},
    };
    auto created = OperationCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    std::stop_source stop;
    stop.request_stop();
    CK_CHECK(job->advance(stop.get_token()) == OperationCopyState::Cancelled && preparations == 0);
    CK_CHECK(value->state(0) == OperationJournalState::Cancelled);
}

CK_TEST(operation_copy_job_prepared_journal_failure_prevents_copy_preparation) {
    auto value = journal();
    unsigned preparations = 0;
    auto request = OperationCopyRequest{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [&preparations](std::stop_token) {
            ++preparations;
            return Observation<StreamCopyJob::Pointer>::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "test.prepare_unreachable"});
        },
        [](std::stop_token) { return no_op_metadata_job(); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [value](OperationJournalRecord record) {
            if (record.state == OperationJournalState::Prepared)
                return Observation<bool>::unavailable(ObservationState::Unknown,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.prepared_flush"});
            return value->append(std::move(record));
        },
        {},
    };
    auto created = OperationCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    CK_CHECK(job->advance() == OperationCopyState::NeedsReconciliation && preparations == 0 &&
             value->state(0) == OperationJournalState::Planned);
}

CK_TEST(operation_copy_job_copied_journal_failure_prevents_metadata_stage) {
    auto value = journal();
    MemoryCopy memory;
    unsigned metadata_preparations = 0;
    auto request = OperationCopyRequest{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [&memory](std::stop_token) { return StreamCopyJob::create(memory.request()); },
        [&metadata_preparations](std::stop_token) {
            ++metadata_preparations;
            return no_op_metadata_job();
        },
        [&memory](std::stop_token) { return Observation<bool>::available(memory.destination == memory.source); },
        [value](OperationJournalRecord record) {
            if (record.state == OperationJournalState::Copied)
                return Observation<bool>::unavailable(ObservationState::Unknown,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.copied_flush"});
            return value->append(std::move(record));
        },
        {},
    };
    auto created = OperationCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 20 && job->state() != OperationCopyState::NeedsReconciliation; ++step)
        job->advance();
    CK_CHECK(job->state() == OperationCopyState::NeedsReconciliation && metadata_preparations == 0 &&
             value->state(0) == OperationJournalState::Prepared && memory.destination == memory.source);
}

CK_TEST(operation_copy_job_verified_journal_failure_requires_reconciliation_after_verification) {
    auto value = journal();
    MemoryCopy memory;
    unsigned verifications = 0;
    auto request = OperationCopyRequest{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [&memory](std::stop_token) { return StreamCopyJob::create(memory.request()); },
        [](std::stop_token) { return no_op_metadata_job(); },
        [&verifications](std::stop_token) {
            ++verifications;
            return Observation<bool>::available(true);
        },
        [value](OperationJournalRecord record) {
            if (record.state == OperationJournalState::Verified)
                return Observation<bool>::unavailable(ObservationState::Unknown,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.verified_flush"});
            return value->append(std::move(record));
        },
        {},
    };
    auto created = OperationCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 20 && job->state() != OperationCopyState::NeedsReconciliation; ++step)
        job->advance();
    CK_CHECK(job->state() == OperationCopyState::NeedsReconciliation && verifications == 1 &&
             value->state(0) == OperationJournalState::Copied && memory.destination == memory.source);
}

CK_TEST(operation_copy_job_unknown_verification_failure_requires_reconciliation) {
    auto value = journal();
    MemoryCopy memory;
    auto request = OperationCopyRequest{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [&memory](std::stop_token) { return StreamCopyJob::create(memory.request()); },
        [](std::stop_token) { return no_op_metadata_job(); },
        [](std::stop_token) { return Observation<bool>::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "test.verify"}); },
        [value](OperationJournalRecord record) { return value->append(std::move(record)); },
        {},
    };
    auto created = OperationCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 20 && job->state() != OperationCopyState::NeedsReconciliation; ++step) job->advance();
    CK_CHECK(job->state() == OperationCopyState::NeedsReconciliation && value->state(0) == OperationJournalState::Copied);
}

CK_TEST(operation_copy_job_metadata_failure_prevents_verification) {
    auto value = journal();
    MemoryCopy memory;
    unsigned verifications = 0;
    auto request = OperationCopyRequest{
        0, value,
        [](std::stop_token) { return Observation<bool>::available(true); },
        [](std::stop_token) { return Observation<bool>::available(true); },
        [&memory](std::stop_token) { return StreamCopyJob::create(memory.request()); },
        [](std::stop_token) {
            OperationMetadataRequest metadata;
            metadata.apply_traversal_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
            metadata.apply_security_metadata = [](std::stop_token) {
                return Observation<bool>::unavailable(ObservationState::Unknown,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery),
                     "test.metadata_security"});
            };
            metadata.apply_final_times = [](std::stop_token) { return Observation<bool>::available(true); };
            metadata.flush_destination = [](std::stop_token) { return Observation<bool>::available(true); };
            return OperationMetadataJob::create(std::move(metadata));
        },
        [&verifications](std::stop_token) {
            ++verifications;
            return Observation<bool>::available(true);
        },
        [value](OperationJournalRecord record) { return value->append(std::move(record)); },
        {},
    };
    auto created = OperationCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 20 && job->state() != OperationCopyState::Failed &&
         job->state() != OperationCopyState::NeedsReconciliation; ++step) job->advance();
    CK_CHECK(job->state() == OperationCopyState::NeedsReconciliation && verifications == 0 &&
             value->state(0) == OperationJournalState::Copied);
}
