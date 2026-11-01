// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_operation_recovery_evidence.hpp"

namespace ck::files {

// Durable evidence for one identity-bound source retirement. It contains no
// path and is only useful after a recovery worker reopens the recorded parent
// through a separately qualified route and matches both identities.
struct NativeDeleteRecoveryEvidence final {
    NativeName source_name;
    NativeDurableIdentity source_parent_identity;
    NativeDurableIdentity source_identity;
};

Observation<std::vector<std::byte>> encode_native_delete_recovery_evidence(
    const NativeDeleteRecoveryEvidence& evidence,
    NativeOperationRecoveryEvidenceLimits limits = {});
Observation<NativeDeleteRecoveryEvidence> decode_native_delete_recovery_evidence(
    std::span<const std::byte> bytes,
    NativeOperationRecoveryEvidenceLimits limits = {});

} // namespace ck::files
