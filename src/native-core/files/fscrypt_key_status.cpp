// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fscrypt_key_status.hpp"
#include <algorithm>
namespace ck::files {
namespace {
template<class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}
Observation<FscryptFilesystemKeyState> fscrypt_filesystem_key_state(const DirectoryFscryptKeyStatus& report) {
    switch (report.key.status) {
    case 1: return Observation<FscryptFilesystemKeyState>::available(FscryptFilesystemKeyState::Absent);
    case 2: return Observation<FscryptFilesystemKeyState>::available(FscryptFilesystemKeyState::Present);
    case 3: return Observation<FscryptFilesystemKeyState>::available(FscryptFilesystemKeyState::IncompletelyRemoved);
    default: return error<FscryptFilesystemKeyState>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "fscrypt.key_status_code");
    }
}
Observation<bool> fscrypt_key_added_by_self(const DirectoryFscryptKeyStatus& report) {
    if (report.policy.version() != 2) return error<bool>(ObservationState::Unsupported, AdapterError::NotApplicable, "fscrypt.key_self_v1");
    const auto state = fscrypt_filesystem_key_state(report);
    if (!state.value()) { const auto& e = *state.unavailable_detail(); return Observation<bool>::unavailable(e.state, e.error); }
    return Observation<bool>::available((report.key.flags & 1U) != 0);
}
Observation<std::uint32_t> fscrypt_key_user_count(const DirectoryFscryptKeyStatus& report) {
    if (report.policy.version() != 2) return error<std::uint32_t>(ObservationState::Unsupported, AdapterError::NotApplicable, "fscrypt.key_users_v1");
    const auto state = fscrypt_filesystem_key_state(report);
    if (!state.value()) { const auto& e = *state.unavailable_detail(); return Observation<std::uint32_t>::unavailable(e.state, e.error); }
    return Observation<std::uint32_t>::available(report.key.user_count);
}
Observation<ObjectIdentity> detail::qualify_fscrypt_key_policy(const Observation<DirectoryFscryptPolicy>& before,
    const Observation<DirectoryFscryptPolicy>& after) {
    using Result = Observation<ObjectIdentity>;
    for (const auto* item : {&before, &after}) if (!item->value()) {
        const auto& e = *item->unavailable_detail(); return Result::unavailable(e.state, e.error);
    }
    const auto* a = std::get_if<FscryptPolicyData>(&before.value()->policy);
    if (!a) return error<ObjectIdentity>(ObservationState::Unsupported, AdapterError::NotApplicable, "fscrypt.key_no_policy");
    const auto* b = std::get_if<FscryptPolicyData>(&after.value()->policy);
    if (!b || !std::equal(a->raw().begin(), a->raw().end(), b->raw().begin(), b->raw().end()))
        return error<ObjectIdentity>(ObservationState::Changed, AdapterError::MetadataChanged, "fscrypt.key_policy_changed");
    if (before.value()->directory.scheme != IdentityScheme::PosixDeviceInode || after.value()->directory.scheme != IdentityScheme::PosixDeviceInode)
        return error<ObjectIdentity>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "fscrypt.key_identity_scheme");
    const auto compared = compare_identity(before.value()->directory, after.value()->directory);
    if (compared != IdentityComparison::Same) return error<ObjectIdentity>(
        compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
        compared == IdentityComparison::Different ? AdapterError::MetadataChanged : AdapterError::UnqualifiedIdentity, "fscrypt.key_directory_identity");
    return Result::available(after.value()->directory);
}
} // namespace ck::files
