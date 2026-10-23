// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "data_layout.hpp"
#include <algorithm>
#include <limits>

namespace ck::files::detail {
namespace {
template<class T> Observation<T> failed(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}
Observation<FileDataLayout> collect_data_layout(const DataRangeQuery& query, DataLayoutFamily family,
    Observation<ObjectIdentity> owner, std::uint64_t size, InventoryLimits limits, std::stop_token stop) {
    using Result = Observation<FileDataLayout>;
    if (!limits.valid()) return failed<FileDataLayout>(ObservationState::Failed, AdapterError::InvalidLimit, "layout.limits");
    if (!query || (family != DataLayoutFamily::PosixSeekDataHole && family != DataLayoutFamily::WindowsAllocatedRanges))
        return failed<FileDataLayout>(ObservationState::Failed, AdapterError::InvalidIdentifier, "layout.query");
    if (limits.max_native_bytes < sizeof(FileDataRange))
        return failed<FileDataLayout>(ObservationState::Unknown, AdapterError::InventoryLimit, "layout.buffer_limit");
    FileDataLayout result{family, std::move(owner), size, {}};
    const auto capacity = std::min(limits.max_records, limits.max_native_bytes / sizeof(FileDataRange));
    std::uint64_t at = 0;
    // At most capacity+1 native callbacks. A final end witness is allowed even
    // when the retained record budget is exactly full; no prefix escapes.
    for (std::size_t call = 0; call <= capacity; ++call) {
        if (stop.stop_requested()) return failed<FileDataLayout>(ObservationState::Unknown, AdapterError::InterruptedRead, "layout.cancelled");
        auto reply = query(at, size);
        if (stop.stop_requested()) return failed<FileDataLayout>(ObservationState::Unknown, AdapterError::InterruptedRead, "layout.cancelled");
        if (!reply.value()) { const auto& error = *reply.unavailable_detail(); return Result::unavailable(error.state, error.error); }
        if (!*reply.value()) return Result::available(std::move(result));
        const auto range = **reply.value();
        if (family == DataLayoutFamily::PosixSeekDataHole && range.offset >= size)
            return failed<FileDataLayout>(ObservationState::Changed, AdapterError::MetadataChanged, "layout.size_changed");
        if (range.length == 0 || range.length > std::numeric_limits<std::uint64_t>::max() - range.offset || range.offset >= size ||
            (family == DataLayoutFamily::PosixSeekDataHole && range.offset < at) ||
            range.offset + range.length <= at)
            return failed<FileDataLayout>(ObservationState::Failed, AdapterError::InvalidNativeResult, "layout.range");
        if (result.ranges.size() == capacity)
            return failed<FileDataLayout>(ObservationState::Unknown, AdapterError::InventoryLimit, "layout.record_limit");
        const auto begin = std::max(at, range.offset), end = std::min(size, range.offset + range.length);
        result.ranges.push_back({begin, end - begin}); at = end;
        if (at == size) return Result::available(std::move(result));
    }
    return failed<FileDataLayout>(ObservationState::Failed, AdapterError::InvalidNativeResult, "layout.progress");
}
Observation<bool> data_layout_metadata_unchanged(const BasicMetadata& before, const BasicMetadata& after) {
    using Result = Observation<bool>;
    for (const auto* metadata : {&before, &after}) {
        if (!metadata->identity.value()) { const auto& e = *metadata->identity.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!metadata->kind.value()) { const auto& e = *metadata->kind.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!metadata->logical_size.value()) { const auto& e = *metadata->logical_size.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    }
    const auto identity = compare_identity(*before.identity.value(), *after.identity.value());
    if (identity == IdentityComparison::Unknown)
        return failed<bool>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "layout.identity");
    if (identity != IdentityComparison::Same || *before.kind.value() != ObjectKind::RegularFile ||
        *after.kind.value() != ObjectKind::RegularFile || *before.logical_size.value() != *after.logical_size.value())
        return failed<bool>(ObservationState::Changed, AdapterError::MetadataChanged, "layout.metadata_changed");
    // Available time observations are useful change witnesses. Their absence
    // does not make this into a snapshot; all results remain live observations.
    for (const auto& pair : {std::pair{&before.modified, &after.modified}, std::pair{&before.changed, &after.changed}})
        if (pair.first->value() && pair.second->value() && *pair.first->value() != *pair.second->value())
            return failed<bool>(ObservationState::Changed, AdapterError::MetadataChanged, "layout.time_changed");
    return Result::available(true);
}
Observation<bool> data_layout_access_guard(const BasicMetadata& metadata, StorageModel model) {
    using Result = Observation<bool>;
    if (!metadata.kind.value()) { const auto& e = *metadata.kind.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (*metadata.kind.value() != ObjectKind::RegularFile)
        return failed<bool>(ObservationState::Unsupported, AdapterError::NotApplicable, "layout.object_kind");
    // Linux has no generic provider-locality query. Explicit read-access opt-in
    // is not a universal no-hydration promise, nor a source read permission grant.
    if (model == StorageModel::LinuxStatx) return Result::available(true);
    if (model != StorageModel::DarwinStatFlags && model != StorageModel::WindowsHandleAttributes)
        return failed<bool>(ObservationState::Failed, AdapterError::InvalidIdentifier, "layout.model");
    const auto review = provider_review_required(metadata, model);
    if (!review.value()) { const auto& e = *review.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (*review.value()) return failed<bool>(ObservationState::Unknown, AdapterError::ProviderReviewRequired, "layout.provider_review");
    return Result::available(true);
}
} // namespace ck::files::detail
