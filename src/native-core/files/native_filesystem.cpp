// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
namespace ck::files {
Observation<BasicMetadata> BasicFileSystem::inspect_native_basic(const NativeAbsolutePath& path) const {
    const auto native = path.host_path();
    if (!native.value()) { const auto& e = *native.unavailable_detail(); return Observation<BasicMetadata>::unavailable(e.state, e.error); }
    return inspect_basic(*native.value());
}
Observation<std::unique_ptr<DirectoryCursor>> BasicFileSystem::open_native_directory(const NativeAbsolutePath& path, DirectoryLinkPolicy policy) const {
    using Result = Observation<std::unique_ptr<DirectoryCursor>>;
    if (policy != DirectoryLinkPolicy::RejectFinalLink && policy != DirectoryLinkPolicy::FollowFinalLink)
        return Result::unavailable(ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "native_path.directory_policy"});
    const auto native = path.host_path();
    if (!native.value()) { const auto& e = *native.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    return open_directory(*native.value(), policy);
}
} // namespace ck::files
