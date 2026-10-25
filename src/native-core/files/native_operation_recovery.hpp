// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_cursor.hpp"
#include "operation_recovery.hpp"

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

} // namespace ck::files
