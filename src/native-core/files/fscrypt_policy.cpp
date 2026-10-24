// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "fscrypt_policy.hpp"
#include <algorithm>
namespace ck::files {
namespace {
template<class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}
Observation<FscryptPolicyData> FscryptPolicyData::decode(std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > max_bytes)
        return error<FscryptPolicyData>(ObservationState::Failed, AdapterError::InvalidNativeResult, "fscrypt.policy_size");
    const auto version = std::to_integer<unsigned>(bytes[0]);
    if (version != 0 && version != 2)
        return error<FscryptPolicyData>(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "fscrypt.policy_version");
    if (bytes.size() != (version == 0 ? 12U : 24U))
        return error<FscryptPolicyData>(ObservationState::Failed, AdapterError::InvalidNativeResult, "fscrypt.policy_size");
    std::array<std::byte, max_bytes> retained{};
    std::copy(bytes.begin(), bytes.end(), retained.begin());
    return Observation<FscryptPolicyData>::available(FscryptPolicyData(retained, bytes.size()));
}
Observation<ObjectIdentity> detail::qualify_fscrypt_directory(const Observation<BasicMetadata>& before,
    const Observation<BasicMetadata>& after, std::optional<bool> policy_present) {
    using Result = Observation<ObjectIdentity>;
    for (const auto* metadata : {&before, &after}) {
        if (!metadata->value()) { const auto& e = *metadata->unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto& m = *metadata->value();
        if (!m.kind.value()) { const auto& e = *m.kind.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (*m.kind.value() < ObjectKind::RegularFile || *m.kind.value() > ObjectKind::Other)
            return error<ObjectIdentity>(ObservationState::Failed, AdapterError::InvalidNativeResult, "fscrypt.directory_kind");
        if (*m.kind.value() != ObjectKind::Directory)
            return error<ObjectIdentity>(ObservationState::Changed, AdapterError::NotDirectory, "fscrypt.directory_kind");
        if (!m.identity.value()) { const auto& e = *m.identity.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (m.identity.value()->scheme != IdentityScheme::PosixDeviceInode)
            return error<ObjectIdentity>(ObservationState::Unknown, AdapterError::UnqualifiedIdentity, "fscrypt.identity_scheme");
        if (policy_present && m.linux_attributes.value()) {
            constexpr std::uint64_t encrypted = 0x800;
            const auto flags = *m.linux_attributes.value();
            if ((flags.known_mask & encrypted) != 0 && ((flags.value & encrypted) != 0) != *policy_present)
                return error<ObjectIdentity>(ObservationState::Changed, AdapterError::MetadataChanged, "fscrypt.conflicting_encryption_flag");
        }
    }
    const auto compared = compare_identity(*before.value()->identity.value(), *after.value()->identity.value());
    if (compared != IdentityComparison::Same)
        return error<ObjectIdentity>(compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
            compared == IdentityComparison::Different ? AdapterError::MetadataChanged : AdapterError::UnqualifiedIdentity, "fscrypt.directory_identity");
    return Result::available(*after.value()->identity.value());
}
} // namespace ck::files
