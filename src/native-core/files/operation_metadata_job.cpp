// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_metadata_job.hpp"

#include <cstdint>

namespace ck::files {
namespace {
UnavailableObservation error(AdapterError code, const char* operation, ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}
}

Observation<OperationMetadataJob::Pointer> OperationMetadataJob::create(OperationMetadataRequest request) {
    if (!request.apply_traversal_metadata || !request.apply_security_metadata || !request.apply_final_times ||
        !request.flush_destination)
        return Observation<Pointer>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "metadata_job.adapters"});
    return Observation<Pointer>::available(Pointer(new OperationMetadataJob(std::move(request))));
}

void OperationMetadataJob::fail(UnavailableObservation detail) noexcept {
    result_.failure = std::move(detail);
    result_.state = OperationMetadataState::Failed;
}

void OperationMetadataJob::cancel() noexcept {
    result_.failure = error(AdapterError::InterruptedRead, "metadata_job.cancelled", ObservationState::Unknown);
    result_.state = OperationMetadataState::Cancelled;
}

OperationMetadataState OperationMetadataJob::invoke(
    const std::function<Observation<bool>(std::stop_token)>& callback, const char* operation, std::stop_token stop) {
    if (stop.stop_requested()) { cancel(); return state(); }
    try {
        auto result = callback(stop);
        if (!result.value()) { fail(*result.unavailable_detail()); return state(); }
        if (!*result.value()) { fail(error(AdapterError::InvalidNativeResult, operation)); return state(); }
    } catch (...) {
        fail(error(AdapterError::InvalidNativeResult, operation));
        return state();
    }
    return state();
}

OperationMetadataState OperationMetadataJob::advance(std::stop_token stop) {
    if (state() == OperationMetadataState::Complete || state() == OperationMetadataState::Failed ||
        state() == OperationMetadataState::Cancelled) return state();
    switch (state()) {
    case OperationMetadataState::Ready:
        result_.state = OperationMetadataState::ApplyingTraversal;
        if (invoke(request_.apply_traversal_metadata, "metadata_job.traversal", stop) != OperationMetadataState::ApplyingTraversal)
            return state();
        result_.state = OperationMetadataState::ApplyingSecurity; return state();
    case OperationMetadataState::ApplyingSecurity:
        if (invoke(request_.apply_security_metadata, "metadata_job.security", stop) != OperationMetadataState::ApplyingSecurity)
            return state();
        result_.state = OperationMetadataState::ApplyingTimes; return state();
    case OperationMetadataState::ApplyingTimes:
        if (invoke(request_.apply_final_times, "metadata_job.times", stop) != OperationMetadataState::ApplyingTimes)
            return state();
        result_.state = OperationMetadataState::Flushing; return state();
    case OperationMetadataState::Flushing:
        if (invoke(request_.flush_destination, "metadata_job.flush", stop) != OperationMetadataState::Flushing)
            return state();
        result_.state = OperationMetadataState::Complete; return state();
    case OperationMetadataState::ApplyingTraversal:
        return state();
    case OperationMetadataState::Complete:
    case OperationMetadataState::Failed:
    case OperationMetadataState::Cancelled: return state();
    }
    return state();
}

} // namespace ck::files
