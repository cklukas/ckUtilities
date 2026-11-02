// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_operation_recovery.hpp"

#include "directory_cursor.hpp"
#include "sha256.hpp"

#include <cstdint>

namespace ck::files {
namespace {
template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

Observation<bool> forward(const UnavailableObservation& detail) {
    return Observation<bool>::unavailable(detail.state, detail.error);
}

Observation<bool> verify_error(AdapterError code, const char* operation,
                               ObservationState state = ObservationState::Unknown) {
    return rejected<bool>(code, operation, state);
}
}

Observation<bool> cleanup_native_unpublished_staging(
    const NativeOperationRecoveryCleanupRequest& request) {
    switch (request.artifact.state) {
    case OperationJournalState::Prepared:
    case OperationJournalState::Copied:
    case OperationJournalState::Verified:
        break;
    default:
        return rejected<bool>(AdapterError::PlanDecisionStale,
                              "native_recovery.cleanup_state", ObservationState::Unknown);
    }
    if (!request.destination || !request.destination_identity.scope.is_valid() ||
        request.destination_identity.scheme == IdentityScheme::Unknown)
        return rejected<bool>(AdapterError::PlanInvalid, "native_recovery.cleanup_request");
    if (!request.artifact.evidence || !request.artifact.evidence->staging_identity)
        return rejected<bool>(AdapterError::UnqualifiedIdentity,
                              "native_recovery.cleanup_staging_identity", ObservationState::Unknown);

    const auto current_destination = request.destination->identity();
    if (!current_destination.value()) return forward(*current_destination.unavailable_detail());
    const auto destination_comparison = compare_identity(
        *current_destination.value(), request.destination_identity);
    if (destination_comparison != IdentityComparison::Same)
        return rejected<bool>(AdapterError::PlanDecisionStale,
                              "native_recovery.cleanup_destination_changed",
                              destination_comparison == IdentityComparison::Different
                                  ? ObservationState::Changed : ObservationState::Unknown);
    if (!request.artifact.evidence->destination_identity.matches_live(*current_destination.value()))
        return rejected<bool>(AdapterError::PlanDecisionStale,
                              "native_recovery.cleanup_destination_evidence",
                              ObservationState::Changed);

    auto opened = request.destination->open_child(request.artifact.evidence->staging_name,
                                                  EntryAccess::MetadataOnly);
    if (!opened.value()) return forward(*opened.unavailable_detail());
    auto metadata = (*opened.value())->inspect_basic();
    if (!metadata.value()) return forward(*metadata.unavailable_detail());
    if (!metadata.value()->identity.value() || !metadata.value()->kind.value() ||
        *metadata.value()->kind.value() != ObjectKind::RegularFile)
        return rejected<bool>(AdapterError::MetadataChanged,
                              "native_recovery.cleanup_kind", ObservationState::Changed);
    if (metadata.value()->identity.value()->scope != current_destination.value()->scope ||
        !request.artifact.evidence->staging_identity->matches_live(
            *metadata.value()->identity.value()))
        return rejected<bool>(AdapterError::PlanDecisionStale,
                              "native_recovery.cleanup_staging_changed",
                              ObservationState::Changed);

    auto discarded = request.destination->discard_child(
        request.artifact.evidence->staging_name, *metadata.value()->identity.value());
    if (!discarded.value()) return forward(*discarded.unavailable_detail());
    if (!*discarded.value())
        return rejected<bool>(AdapterError::InvalidNativeResult,
                              "native_recovery.cleanup_result");
    return Observation<bool>::available(true);
}

Observation<bool> verify_native_published_destination(
    const NativeOperationRecoveryVerificationRequest& request) {
    if (!request.destination || !request.destination_identity.scope.is_valid() ||
        request.destination_identity.scheme == IdentityScheme::Unknown || request.max_bytes == 0)
        return rejected<bool>(AdapterError::PlanInvalid, "native_recovery.verify_request");
    if (!request.artifact.evidence || !request.artifact.evidence->content_digest ||
        !request.artifact.evidence->published_identity ||
        !request.artifact.evidence->destination_name.is_valid())
        return verify_error(AdapterError::UnqualifiedIdentity, "native_recovery.verify_receipt");
    if (request.artifact.state != OperationJournalState::DestinationCommitted &&
        request.artifact.state != OperationJournalState::SourceRetired &&
        request.artifact.state != OperationJournalState::Complete)
        return verify_error(AdapterError::PlanDecisionStale, "native_recovery.verify_state");

    const auto current_destination = request.destination->identity();
    if (!current_destination.value()) return forward(*current_destination.unavailable_detail());
    if (compare_identity(*current_destination.value(), request.destination_identity) != IdentityComparison::Same ||
        !request.artifact.evidence->destination_identity.matches_live(*current_destination.value()))
        return verify_error(AdapterError::PlanDecisionStale,
                            "native_recovery.verify_destination_changed", ObservationState::Changed);

    auto opened = request.destination->open_child(request.artifact.evidence->destination_name,
                                                   EntryAccess::ContentRead);
    if (!opened.value()) return forward(*opened.unavailable_detail());
    auto metadata = (*opened.value())->inspect_basic();
    if (!metadata.value()) return forward(*metadata.unavailable_detail());
    if (!metadata.value()->identity.value() || !metadata.value()->kind.value() ||
        *metadata.value()->kind.value() != ObjectKind::RegularFile)
        return verify_error(AdapterError::MetadataChanged, "native_recovery.verify_kind",
                            ObservationState::Changed);
    if (!request.artifact.evidence->published_identity->matches_live(
            *metadata.value()->identity.value()))
        return verify_error(AdapterError::PlanDecisionStale,
                            "native_recovery.verify_published_identity", ObservationState::Changed);
    if (!metadata.value()->logical_size.value() ||
        *metadata.value()->logical_size.value() > request.max_bytes)
        return verify_error(AdapterError::InventoryLimit, "native_recovery.verify_size");

    Sha256 digest;
    std::uint64_t offset = 0;
    for (;;) {
        auto content = (*opened.value())->read_content({64 * 1024}, offset);
        if (!content.value()) return forward(*content.unavailable_detail());
        if (content.value()->bytes.size() > request.max_bytes - offset)
            return verify_error(AdapterError::InventoryLimit, "native_recovery.verify_bytes");
        digest.update(std::span<const std::byte>(content.value()->bytes));
        offset += content.value()->bytes.size();
        if (content.value()->complete) break;
        if (content.value()->bytes.empty())
            return verify_error(AdapterError::InvalidNativeResult,
                                "native_recovery.verify_no_progress");
    }
    if (offset != *metadata.value()->logical_size.value())
        return verify_error(AdapterError::MetadataChanged,
                            "native_recovery.verify_length", ObservationState::Changed);
    return Observation<bool>::available(
        digest.digest() == *request.artifact.evidence->content_digest);
}
} // namespace ck::files
