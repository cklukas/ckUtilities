// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_copy_job.hpp"

#include <cstdint>

namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char* operation, ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
template <class T>
Observation<T> rejected(AdapterError code, const char* operation) {
    return Observation<T>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}

Observation<OperationCopyJob::Pointer> OperationCopyJob::create(OperationCopyRequest request) {
    if (!request.journal || request.item >= request.journal->limits().max_items ||
        request.journal->state(request.item) != OperationJournalState::Planned || !request.revalidate_source ||
        !request.revalidate_destination || !request.prepare_copy || !request.prepare_metadata ||
        !request.verify_copy || !request.append_journal)
        return rejected<Pointer>(AdapterError::PlanInvalid, "operation_copy.request");
    return Observation<Pointer>::available(Pointer(new OperationCopyJob(std::move(request))));
}

void OperationCopyJob::fail(UnavailableObservation detail, bool reconciliation) {
    result_.failure = std::move(detail);
    result_.state = reconciliation ? OperationCopyState::NeedsReconciliation : OperationCopyState::Failed;
}

bool OperationCopyJob::append(OperationJournalState state) {
    const auto before = request_.journal->records().size();
    const auto sequence = static_cast<std::uint64_t>(before) + 1;
    auto result = request_.append_journal({sequence, request_.item, state, {}});
    if (!result.value()) { fail(*result.unavailable_detail(), true); return false; }
    if (request_.journal->records().size() != before + 1 || request_.journal->records().back().sequence != sequence ||
        request_.journal->state(request_.item) != state) {
        fail(error(AdapterError::InvalidNativeResult, "operation_copy.journal_publish"), true);
        return false;
    }
    return true;
}

bool OperationCopyJob::validate(const std::function<Observation<bool>(std::stop_token)>& callback,
                                const char* operation, std::stop_token stop) {
    if (stop.stop_requested()) {
        if (append(OperationJournalState::Cancelled)) result_.state = OperationCopyState::Cancelled;
        return false;
    }
    try {
        auto result = callback(stop);
        if (!result.value()) { fail(*result.unavailable_detail(), false); return false; }
        if (!*result.value()) { fail(error(AdapterError::PlanDecisionStale, operation), false); return false; }
    } catch (...) {
        fail(error(AdapterError::InvalidNativeResult, operation), true);
        return false;
    }
    return true;
}

OperationCopyState OperationCopyJob::advance(std::stop_token stop) {
    if (result_.state == OperationCopyState::Verified || result_.state == OperationCopyState::Failed ||
        result_.state == OperationCopyState::Cancelled || result_.state == OperationCopyState::NeedsReconciliation)
        return result_.state;
    if (result_.state == OperationCopyState::Ready) {
        if (!validate(request_.revalidate_source, "operation_copy.source_changed", stop) ||
            !validate(request_.revalidate_destination, "operation_copy.destination_changed", stop))
            return result_.state;
        if (append(OperationJournalState::Prepared)) result_.state = OperationCopyState::Preparing;
        return result_.state;
    }
    if (result_.state == OperationCopyState::Preparing) {
        if (stop.stop_requested()) {
            if (append(OperationJournalState::Cancelled)) result_.state = OperationCopyState::Cancelled;
            return result_.state;
        }
        try {
            auto prepared = request_.prepare_copy(stop);
            if (!prepared.value()) { fail(*prepared.unavailable_detail(), true); return result_.state; }
            copy_ = std::move(prepared).take_value();
            if (!copy_) { fail(error(AdapterError::InvalidNativeResult, "operation_copy.prepare_null"), true); return result_.state; }
        } catch (...) {
            fail(error(AdapterError::InvalidNativeResult, "operation_copy.prepare_exception"), true);
            return result_.state;
        }
        result_.state = OperationCopyState::Copying;
        return result_.state;
    }
    if (result_.state == OperationCopyState::Copying) {
        const auto copied = copy_->advance(stop);
        if (copied == StreamCopyState::Cancelled) {
            if (append(OperationJournalState::Cancelled)) result_.state = OperationCopyState::Cancelled;
        } else if (copied == StreamCopyState::Failed) {
            if (copy_->failure()) result_.failure = copy_->failure();
            if (append(OperationJournalState::Failed)) result_.state = OperationCopyState::Failed;
        } else if (copied == StreamCopyState::Complete) {
            if (append(OperationJournalState::Copied)) result_.state = OperationCopyState::PreparingMetadata;
        }
        return result_.state;
    }
    if (result_.state == OperationCopyState::PreparingMetadata) {
        if (stop.stop_requested()) {
            if (append(OperationJournalState::Cancelled)) result_.state = OperationCopyState::Cancelled;
            return result_.state;
        }
        try {
            auto prepared = request_.prepare_metadata(stop);
            if (!prepared.value()) { fail(*prepared.unavailable_detail(), true); return result_.state; }
            metadata_ = std::move(prepared).take_value();
            if (!metadata_) {
                fail(error(AdapterError::InvalidNativeResult, "operation_copy.metadata_prepare_null"), true);
                return result_.state;
            }
        } catch (...) {
            fail(error(AdapterError::InvalidNativeResult, "operation_copy.metadata_prepare_exception"), true);
            return result_.state;
        }
        result_.state = OperationCopyState::ApplyingMetadata;
        return result_.state;
    }
    if (result_.state == OperationCopyState::ApplyingMetadata) {
        const auto metadata_state = metadata_->advance(stop);
        if (metadata_state == OperationMetadataState::Cancelled) {
            if (metadata_->failure()) result_.failure = metadata_->failure();
            if (append(OperationJournalState::Cancelled)) result_.state = OperationCopyState::Cancelled;
        } else if (metadata_state == OperationMetadataState::Failed) {
            if (metadata_->failure()) result_.failure = metadata_->failure();
            if (append(OperationJournalState::Failed)) result_.state = OperationCopyState::Failed;
        } else if (metadata_state == OperationMetadataState::Complete) {
            result_.state = OperationCopyState::Verifying;
        }
        return result_.state;
    }
    if (stop.stop_requested()) {
        if (append(OperationJournalState::Cancelled)) result_.state = OperationCopyState::Cancelled;
        return result_.state;
    }
    try {
        auto verified = request_.verify_copy(stop);
        if (!verified.value()) { fail(*verified.unavailable_detail(), true); return result_.state; }
        if (!*verified.value()) { fail(error(AdapterError::PlanDecisionStale, "operation_copy.verify"), false); return result_.state; }
    } catch (...) {
        fail(error(AdapterError::InvalidNativeResult, "operation_copy.verify_exception"), true);
        return result_.state;
    }
    if (append(OperationJournalState::Verified)) result_.state = OperationCopyState::Verified;
    return result_.state;
}

} // namespace ck::files
