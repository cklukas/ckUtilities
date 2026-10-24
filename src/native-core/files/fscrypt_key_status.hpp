// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "fscrypt_policy.hpp"

namespace ck::files {
enum class FscryptFilesystemKeyState { Absent, Present, IncompletelyRemoved };
struct FscryptKeyStatusData {
    std::uint32_t status = 0, flags = 0, user_count = 0;
    std::array<std::uint32_t, 13> reserved{}; // retain future output, do not interpret
};
struct DirectoryFscryptKeyStatus {
    ObjectIdentity directory;
    FscryptPolicyData policy;
    FscryptKeyStatusData key;
};
// Filesystem-level keyring only. In particular, Absent does not inspect legacy
// process-subscribed v1 keyrings or prove that files are locked/inaccessible.
// Present is not access permission, hydration consent or a preservation promise.
// Unknown native states remain in key.status and produce Unsupported here.
Observation<FscryptFilesystemKeyState> fscrypt_filesystem_key_state(const DirectoryFscryptKeyStatus& report);
// These fields are defined for v2 identifiers only; v1 raw fields remain retained
// without interpreting zero as a meaningful user count or ownership assertion.
Observation<bool> fscrypt_key_added_by_self(const DirectoryFscryptKeyStatus& report);
Observation<std::uint32_t> fscrypt_key_user_count(const DirectoryFscryptKeyStatus& report);
namespace detail {
Observation<ObjectIdentity> qualify_fscrypt_key_policy(const Observation<DirectoryFscryptPolicy>& before,
    const Observation<DirectoryFscryptPolicy>& after);
}
} // namespace ck::files
