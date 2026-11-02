// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_cursor.hpp"
#include "operation_recovery.hpp"

#include <cstdint>
#include <memory>

namespace ck::files {

struct NativeOperationRecoveryCleanupRequest final {
    NativeOperationRecoveryArtifact artifact;
    std::shared_ptr<const DirectoryCursor> destination;
    ObjectIdentity destination_identity;
};

// Cleans one crash-recovery staging candidate only after fresh held-directory
// and object-identity checks. Missing staging identity is never guessed.
Observation<bool> cleanup_native_unpublished_staging(
    const NativeOperationRecoveryCleanupRequest& request);

struct NativeOperationRecoveryVerificationRequest final {
    NativeOperationRecoveryArtifact artifact;
    std::shared_ptr<const DirectoryCursor> destination;
    ObjectIdentity destination_identity;
    std::uint64_t max_bytes = 4ULL * 1024 * 1024 * 1024;
};

// Read-only post-publication verification. It binds the held destination
// directory, exact native destination name, published object identity, size,
// and durable content digest. A false result is a verified mismatch; an
// unavailable result is an inability to establish the receipt. No mutation or
// pathname-based fallback is performed.
Observation<bool> verify_native_published_destination(
    const NativeOperationRecoveryVerificationRequest& request);

} // namespace ck::files
