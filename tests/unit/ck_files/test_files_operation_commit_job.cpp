// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_commit_job.hpp"

#include <cvision/testing/cktest.hpp>

#include <cstdint>
#include <stdexcept>

using namespace ck::files;

namespace {
OperationJournal::Pointer journal(FileOperation operation) {
    OperationJournalId id;
    id.bytes[0] = std::byte{0x23};
    auto result = OperationJournal::create(operation, id, {2, 32, 64});
    if (!result.value()) throw std::runtime_error("commit journal fixture failed");
    auto value = std::move(result).take_value();
    const auto append = [&](std::uint64_t sequence, OperationJournalState state) {
        if (!value->append({sequence, 0, state, {}}).value())
            throw std::runtime_error("commit journal state fixture failed");
    };
    append(1, OperationJournalState::Planned);
    append(2, OperationJournalState::Prepared);
    append(3, OperationJournalState::Copied);
    append(4, OperationJournalState::Verified);
    return value;
}

OperationCommitRequest request(FileOperation operation, OperationJournal::Pointer value) {
    OperationCommitRequest result;
    result.operation = operation;
    result.journal = std::move(value);
    result.revalidate_source = [](std::stop_token) { return Observation<bool>::available(true); };
    result.revalidate_destination = [](std::stop_token) { return Observation<bool>::available(true); };
    result.commit_destination = [](std::stop_token) { return Observation<bool>::available(true); };
    result.retire_source = [](std::stop_token) { return Observation<bool>::available(true); };
    const auto journal_copy = result.journal;
    result.append_journal = [journal_copy](OperationJournalRecord record) {
        return journal_copy->append(std::move(record));
    };
    return result;
}
}

CK_TEST(operation_commit_job_copy_persists_intent_before_publication_and_completes) {
    auto value = journal(FileOperation::Copy);
    unsigned revalidations = 0, publications = 0;
    auto request_value = request(FileOperation::Copy, value);
    request_value.revalidate_source = [&](std::stop_token) {
        ++revalidations;
        return Observation<bool>::available(true);
    };
    request_value.revalidate_destination = request_value.revalidate_source;
    request_value.commit_destination = [&](std::stop_token) {
        ++publications;
        return Observation<bool>::available(true);
    };
    auto created = OperationCommitJob::create(std::move(request_value));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    CK_CHECK(job->advance() == OperationCommitState::CommittingDestination);
    CK_CHECK(value->state(0) == OperationJournalState::CommitIntended && publications == 0);
    CK_CHECK(job->advance() == OperationCommitState::Complete);
    CK_CHECK(publications == 1 && revalidations == 2 && value->state(0) == OperationJournalState::Complete);
}

CK_TEST(operation_commit_job_move_cancellation_after_destination_retains_source) {
    auto value = journal(FileOperation::Move);
    auto request_value = request(FileOperation::Move, value);
    unsigned retirements = 0;
    request_value.retire_source = [&](std::stop_token) {
        ++retirements;
        return Observation<bool>::available(true);
    };
    auto created = OperationCommitJob::create(std::move(request_value));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    CK_CHECK(job->advance() == OperationCommitState::CommittingDestination);
    CK_CHECK(job->advance() == OperationCommitState::RetiringSource);
    std::stop_source stop;
    stop.request_stop();
    CK_CHECK(job->advance(stop.get_token()) == OperationCommitState::RetainedSource);
    CK_CHECK(retirements == 0 && value->state(0) == OperationJournalState::RetainedSource);
}

CK_TEST(operation_commit_job_ambiguous_publication_requires_reconciliation) {
    auto value = journal(FileOperation::Copy);
    auto request_value = request(FileOperation::Copy, value);
    request_value.commit_destination = [](std::stop_token) {
        return Observation<bool>::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.publish"});
    };
    auto created = OperationCommitJob::create(std::move(request_value));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    CK_CHECK(job->advance() == OperationCommitState::CommittingDestination);
    CK_CHECK(job->advance() == OperationCommitState::NeedsReconciliation);
    CK_CHECK(value->state(0) == OperationJournalState::CommitIntended && job->failure());
}

CK_TEST(operation_commit_job_revalidation_failure_is_durable_and_starts_no_mutation) {
    auto value = journal(FileOperation::Copy);
    unsigned publications = 0;
    auto request_value = request(FileOperation::Copy, value);
    request_value.revalidate_destination = [](std::stop_token) { return Observation<bool>::available(false); };
    request_value.commit_destination = [&](std::stop_token) {
        ++publications;
        return Observation<bool>::available(true);
    };
    auto created = OperationCommitJob::create(std::move(request_value));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    CK_CHECK(job->advance() == OperationCommitState::Failed);
    CK_CHECK(publications == 0 && value->state(0) == OperationJournalState::Failed);
}

CK_TEST(operation_commit_job_journal_intent_failure_prevents_publication) {
    auto value = journal(FileOperation::Copy);
    unsigned publications = 0;
    auto request_value = request(FileOperation::Copy, value);
    request_value.append_journal = [](OperationJournalRecord) {
        return Observation<bool>::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.intent_flush"});
    };
    request_value.commit_destination = [&](std::stop_token) {
        ++publications;
        return Observation<bool>::available(true);
    };
    auto created = OperationCommitJob::create(std::move(request_value));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    CK_CHECK(job->advance() == OperationCommitState::NeedsReconciliation);
    CK_CHECK(publications == 0 && value->state(0) == OperationJournalState::Verified);
}

CK_TEST(operation_commit_job_post_publication_journal_failure_stops_before_source_retirement) {
    auto value = journal(FileOperation::Move);
    unsigned records = 0, retirements = 0;
    auto request_value = request(FileOperation::Move, value);
    request_value.append_journal = [&records, &value](OperationJournalRecord record) {
        ++records;
        if (record.state == OperationJournalState::DestinationCommitted)
            return Observation<bool>::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.commit_flush"});
        return value->append(std::move(record));
    };
    request_value.retire_source = [&](std::stop_token) {
        ++retirements;
        return Observation<bool>::available(true);
    };
    auto created = OperationCommitJob::create(std::move(request_value));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    CK_CHECK(job->advance() == OperationCommitState::CommittingDestination);
    CK_CHECK(job->advance() == OperationCommitState::NeedsReconciliation);
    CK_CHECK(records == 2 && retirements == 0 && value->state(0) == OperationJournalState::CommitIntended);
}
