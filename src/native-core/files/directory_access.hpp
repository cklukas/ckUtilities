// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "storage_info.hpp"
namespace ck::files {
enum class DirectoryAccess { List, Search, AddFile, AddDirectory };
constexpr bool valid_directory_access(DirectoryAccess access) noexcept {
    return access >= DirectoryAccess::List && access <= DirectoryAccess::AddDirectory;
}
enum class DirectoryAccessFamily { LinuxFaccessat2, DarwinFaccessat, WindowsOpenById };
struct DirectoryAccessCheck {
    DirectoryAccess requested;
    DirectoryAccessFamily family;
    ObjectIdentity directory;
    std::uint32_t native_requested_mask;
};
// Successful native access check at query time, NOT permission to execute an
// operation or proof of creatability/preservation. POSIX Add* checks W_OK|X_OK;
// Windows requests the corresponding directory right plus metadata/sync rights.
// No child is created. Quotas, names, policy, mount state, inheritance, storage,
// concurrent changes and execution-time access still need independent checks.
namespace detail {
// Qualifies directory kind, provider guards and before/after native identity.
Observation<ObjectIdentity> qualify_access_directory(const Observation<BasicMetadata>& before,
    const Observation<BasicMetadata>& after, StorageModel model);
}
} // namespace ck::files
