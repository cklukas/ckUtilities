// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_cursor.hpp"

#include <filesystem>
#include <memory>

namespace ck::files {

// Injected, read-only basic-inspection and directory boundary. Calls can block in the OS and
// belong to workers. This interface has no mutation, shell or elevation API.
class BasicFileSystem {
public:
    virtual ~BasicFileSystem() = default;
    virtual Observation<std::unique_ptr<DirectoryCursor>> open_directory(
        const std::filesystem::path& absolute_path, DirectoryLinkPolicy policy = DirectoryLinkPolicy::RejectFinalLink) const = 0;
    virtual Observation<BasicMetadata> inspect_basic(const std::filesystem::path& absolute_path) const = 0;
};

class NativeFileSystem final : public BasicFileSystem {
public:
    NativeFileSystem() = default;
    NativeFileSystem(const NativeFileSystem&) = delete;
    NativeFileSystem& operator=(const NativeFileSystem&) = delete;
    // Native absolute path, no UTF-8 round trip and no process-cwd fallback.
    // The final link/reparse point is inspected itself. Earlier path components
    // have normal host navigation semantics. This initial observation is not
    // a pinned directory-entry reference and cannot authorize a later action.
    // Windows accepts filesystem drive/UNC/volume-GUID namespaces only. Device
    // names are refused before opening; literal reserved basenames require an
    // extended filesystem path rather than ambiguous Win32 device spelling.
    Observation<BasicMetadata> inspect_basic(const std::filesystem::path& absolute_path) const override;
    // Owns a native directory stream, never reopens a path while reading. The
    // policy concerns the final component; parents retain host navigation rules.
    Observation<std::unique_ptr<DirectoryCursor>> open_directory(
        const std::filesystem::path& absolute_path, DirectoryLinkPolicy policy = DirectoryLinkPolicy::RejectFinalLink) const override;
private:
    AdapterScope scope_;
};

} // namespace ck::files
