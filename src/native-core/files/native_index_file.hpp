// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_index_store.hpp"
#include "native_path.hpp"
namespace ck::files {
// Worker-only scratch creation, separate from the read-only source adapter.
// Explicit host-native directory address, no default/temp/env fallback. Parent
// components have normal navigation semantics; final links/navigation rejected.
// POSIX requires an existing current-user-owned 0700 root on a qualified local
// filesystem (macOS also requires no extended ACL and ownership enabled).
// Windows requires persistent ACLs and atomically installs a protected DACL.
// The successful owner is unnamed on POSIX, exclusive/delete-on-close on Windows.
// Same-user processes and privileged administrators remain within the trust
// boundary. This is scratch storage, not encrypted storage or a durable journal.
Observation<std::unique_ptr<IndexByteFile>> create_native_index_file(const NativeAbsolutePath &root);
} // namespace ck::files
