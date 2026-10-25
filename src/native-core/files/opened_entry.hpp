// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "data_channels.hpp"
#include "attributes.hpp"
#include "security.hpp"
#include "storage_info.hpp"
#include "link_metadata.hpp"
#include "data_layout.hpp"
#include "file_limits.hpp"
#include "integrity_info.hpp"
#include "inode_flags.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <vector>

namespace ck::files {

// Requested OS access only; never authority for mutation, loss or hydration.
// Layout is an explicit regular-file query opt-in and can require data-read
// permission on platforms that need an additional readable handle.
// InodeFlags independently requests a Linux regular-file readable handle.
enum class EntryAccess : unsigned {
    MetadataOnly = 0, MetadataAndAttributes = 1, MetadataAndSecurity = 2,
    MetadataAndFullSecurity = 6, MetadataAndLayout = 8, MetadataAndInodeFlags = 16, ContentRead = 32
};
constexpr EntryAccess operator|(EntryAccess left, EntryAccess right) noexcept {
    return static_cast<EntryAccess>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}
constexpr bool valid_entry_access(EntryAccess access) noexcept {
    const auto bits = static_cast<unsigned>(access);
    return (bits & ~63U) == 0 && ((bits & 4U) == 0 || (bits & 2U) != 0);
}
constexpr bool requests_attributes(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 1U) != 0; }
constexpr bool requests_security(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 2U) != 0; }
constexpr bool requests_audit_security(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 4U) != 0; }
constexpr bool requests_inode_flags(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 16U) != 0; }
constexpr bool requests_content_read(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 32U) != 0; }
constexpr bool requests_layout(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 8U) != 0; }

struct ContentReadLimits {
    std::size_t max_bytes = 64 * 1024;
    static constexpr std::size_t hard_max_bytes = 4 * 1024 * 1024;
    bool valid() const noexcept { return max_bytes > 0 && max_bytes <= hard_max_bytes; }
};
struct ContentRead {
    std::vector<std::byte> bytes;
    bool complete = false;
};

struct ContentWriteLimits {
    std::size_t max_bytes = 4 * 1024 * 1024;
    static constexpr std::size_t hard_max_bytes = 4 * 1024 * 1024;
    bool valid() const noexcept { return max_bytes > 0 && max_bytes <= hard_max_bytes; }
};

// Owns the opened object, independently of the original parent/name. Queries
// use that object even if its entry is renamed, replaced or unlinked. This does
// not freeze content, prove continued parent membership, or authorize mutation.
// Opening and inspecting belong to workers; native calls can block. This initial
// boundary has no write, shell or elevation API. Attribute values can themselves
// carry data/security information; reads are explicit and bounded.
class OpenedEntry {
public:
    virtual ~OpenedEntry() = default;
    OpenedEntry(const OpenedEntry&) = delete;
    OpenedEntry& operator=(const OpenedEntry&) = delete;
    virtual Observation<BasicMetadata> inspect_basic() const = 0;
    // Directory objects only. Opens a metadata-only parent reference through
    // the held object. No original path reopen or enumeration; native support
    // and permissions may refuse. A live edge observation, not a stable path.
    virtual Observation<std::unique_ptr<OpenedEntry>> open_parent_directory() const;
    // Bound for this held inode only. On a directory this concerns the
    // directory itself, never prospective child files. No content access.
    virtual Observation<FileLinkLimits> inspect_link_limits() const;
    // Fixed read-only integrity configuration query through the retained
    // object. Does not enable/disable integrity or read/verify any content.
    virtual Observation<FileIntegrityInfo> inspect_integrity(InventoryLimits limits = {}) const;
    // Linux regular-file FS_IOC_GETFLAGS. Explicit independent read-access
    // opt-in; no content reads or setters. Other platforms are Unsupported.
    virtual Observation<FileInodeFlags> inspect_inode_flags(InventoryLimits limits = {}) const;
    virtual Observation<StorageInfo> inspect_storage() const;
    // Explicit opt-in layout access can require native data-read permission;
    // this query seeks/queries metadata and never reads file content. Main data
    // only, regular files only. No hydration consent or dense-file fallback.
    virtual Observation<FileDataLayout> inspect_data_layout(InventoryLimits limits = {}, std::stop_token stop = {}) const;
    virtual Observation<LinkMetadata> read_link_metadata(InventoryLimits limits = {}) const;
    // Native channel identifiers and sizes only; no channel content is read.
    // Coverage is explicit. Unsupported is not evidence that channels are absent.
    // This live query does not establish complete metadata or snapshot stability.
    virtual Observation<DataChannelCatalog> inspect_data_channels(InventoryLimits limits = {}) const;
    virtual Observation<AttributeCatalog> inspect_attributes(InventoryLimits limits = {}) const;
    virtual Observation<AttributeValue> read_attribute(const NativeFeatureIdentifier& identifier, InventoryLimits limits = {}) const;
    virtual Observation<NativeSecurityValue> read_security(SecurityPart part, InventoryLimits limits = {}) const;
    virtual Observation<ContentRead> read_content(ContentReadLimits limits = {}, std::uint64_t offset = 0,
                                                   std::stop_token stop = {}) const;
    // Explicit staging-file write boundary. The object must have been created
    // by the worker's held parent with write access; this is not a path-based
    // replacement API and does not publish a destination name.
    virtual Observation<std::size_t> write_content(std::uint64_t offset, std::span<const std::byte> bytes,
                                                    ContentWriteLimits limits = {}, std::stop_token stop = {});
    virtual Observation<bool> flush_content();
protected:
    OpenedEntry() = default;
};

} // namespace ck::files
