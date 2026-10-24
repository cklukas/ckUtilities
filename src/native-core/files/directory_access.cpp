// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_access.hpp"
namespace ck::files::detail {
Observation<ObjectIdentity> qualify_access_directory(const Observation<BasicMetadata>& before,
    const Observation<BasicMetadata>& after, StorageModel model) {
    using Result = Observation<ObjectIdentity>;
    for (const auto* metadata : {&before, &after}) {
        if (!metadata->value()) { const auto& e = *metadata->unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto& value = *metadata->value();
        if (!value.kind.value()) { const auto& e = *value.kind.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (*value.kind.value() < ObjectKind::RegularFile || *value.kind.value() > ObjectKind::Other) return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "directory.access_kind"});
        if (*value.kind.value() != ObjectKind::Directory) return Result::unavailable(ObservationState::Changed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotDirectory), "directory.access_kind"});
        // Linux has no generic provider-state query. This metadata-only access
        // check makes no locality/hydration claim; do not emulate such a query.
        if (model != StorageModel::LinuxStatx) {
            const auto review = provider_review_required(value, model);
            if (!review.value()) { const auto& e = *review.unavailable_detail(); return Result::unavailable(e.state, e.error); }
            if (*review.value()) return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::ProviderReviewRequired), "directory.access_provider"});
        }
        if (!value.identity.value()) { const auto& e = *value.identity.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    }
    const auto compared = compare_identity(*before.value()->identity.value(), *after.value()->identity.value());
    if (compared != IdentityComparison::Same) return Result::unavailable(
        compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(compared == IdentityComparison::Different ? AdapterError::MetadataChanged : AdapterError::UnqualifiedIdentity), "directory.access_identity"});
    return Result::available(*after.value()->identity.value());
}
} // namespace ck::files::detail
