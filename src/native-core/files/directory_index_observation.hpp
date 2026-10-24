// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "file_metadata.hpp"
namespace ck::files::detail {
// Only checks observable contradictions. Available(true) is not snapshot proof.
inline Observation<bool> check_index_directory(const AdapterScope &scope, const Observation<ObjectIdentity> &directory,
                                               const Observation<BasicMetadata> &before,
                                               const Observation<BasicMetadata> &value) {
    const auto error = [](AdapterError code, const char *op, ObservationState state = ObservationState::Failed) {
        return UnavailableObservation{state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op}};
    };
    const auto differs = [](const Observation<FileTimestamp> &a, const Observation<FileTimestamp> &b) {
        return a.value() && b.value() && *a.value() != *b.value();
    };

    if (value.state() == ObservationState::Gone || value.state() == ObservationState::Changed) {
        return Observation<bool>::unavailable(value.state(), value.unavailable_detail()->error);
    }
    if (directory.value() && directory.value()->scope != scope) {
        const auto e = error(AdapterError::InvalidIdentifier, "index_listing.directory_scope");
        return Observation<bool>::unavailable(e.state, e.error);
    }
    if (!value.value())
        return Observation<bool>::available(true); // uncertainty retained, never converted into snapshot stability
    const auto &m = *value.value();
    if (m.kind.value() && *m.kind.value() != ObjectKind::Directory) {
        const auto e = error(AdapterError::NotDirectory, "index_listing.directory_kind");
        return Observation<bool>::unavailable(e.state, e.error);
    }
    if (m.identity.value() && m.identity.value()->scope != scope) {
        const auto e = error(AdapterError::InvalidIdentifier, "index_listing.metadata_scope");
        return Observation<bool>::unavailable(e.state, e.error);
    }
    if ((directory.value() && m.identity.value() &&
         compare_identity(*directory.value(), *m.identity.value()) == IdentityComparison::Different) ||
        (before.value() && before.value()->identity.value() && m.identity.value() &&
         compare_identity(*before.value()->identity.value(), *m.identity.value()) == IdentityComparison::Different) ||
        (before.value() &&
         (differs(before.value()->modified, m.modified) || differs(before.value()->changed, m.changed)))) {
        const auto e =
            error(AdapterError::MetadataChanged, "index_listing.directory_changed", ObservationState::Changed);
        return Observation<bool>::unavailable(e.state, e.error);
    }
    return Observation<bool>::available(true);
}
} // namespace ck::files::detail
