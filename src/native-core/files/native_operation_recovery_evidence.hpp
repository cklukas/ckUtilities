// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "native_path.hpp"
#include "native_name.hpp"
#include "observation.hpp"

#include <cstddef>
#include <cstdint>
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace ck::files {

// AdapterScope identifies one live adapter instance and cannot establish
// authority after a process restart. Recovery binds these durable values to
// fresh held-object observations before taking any action.
struct NativeDurableIdentity final {
    IdentityScheme scheme = IdentityScheme::Unknown;
    NativeIdentifier volume;
    NativeIdentifier object;
    std::optional<std::uint64_t> generation;

    static std::optional<NativeDurableIdentity> from_live(const ObjectIdentity& value) noexcept;
    bool matches_live(const ObjectIdentity& value) const noexcept;
    friend bool operator==(const NativeDurableIdentity&, const NativeDurableIdentity&) = default;
};

struct NativeOperationRecoveryEvidence final {
    NativeName staging_name;
    NativeName destination_name;
    NativeDurableIdentity source_parent_identity;
    NativeDurableIdentity source_identity;
    NativeDurableIdentity destination_identity;
    // Absent for Planned/Prepared records because the staging object may not
    // exist yet. Recovery must not clean a name without this identity.
    std::optional<NativeDurableIdentity> staging_identity;
    // Optional exact requested destination address. Older journals omit it;
    // recovery must remain inspect-only when it is absent. The address is
    // never an identity and must be rebound to a fresh held directory before
    // any mutation.
    std::optional<NativeAbsolutePath> destination_address;
    // The independently verified whole-file content digest. This is a
    // receipt for later reconciliation, never authority to reopen a source or
    // mutate a destination by pathname.
    std::optional<std::array<std::byte, 32>> content_digest;
};

struct NativeOperationRecoveryEvidenceLimits final {
    std::size_t max_bytes = 4096;
    std::size_t max_name_units = 32 * 1024;
    static constexpr std::size_t hard_max_bytes = 1024 * 1024;
    static constexpr std::size_t hard_max_name_units = 256 * 1024;
};

// Local journal payload only: native bytes/UTF-16 units are retained exactly.
// It is never a display string and never reconstructs a path.
Observation<std::vector<std::byte>> encode_native_operation_recovery_evidence(
    const NativeOperationRecoveryEvidence& evidence,
    NativeOperationRecoveryEvidenceLimits limits = {});
Observation<NativeOperationRecoveryEvidence> decode_native_operation_recovery_evidence(
    std::span<const std::byte> bytes,
    NativeOperationRecoveryEvidenceLimits limits = {});

} // namespace ck::files
