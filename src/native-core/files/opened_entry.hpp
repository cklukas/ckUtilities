// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "file_metadata.hpp"
#include "data_channels.hpp"
#include "attributes.hpp"
#include "security.hpp"
#include "storage_info.hpp"

namespace ck::files {

// Requested OS access only; never authority for mutation, loss or hydration.
enum class EntryAccess : unsigned {
    MetadataOnly = 0, MetadataAndAttributes = 1, MetadataAndSecurity = 2,
    MetadataAndFullSecurity = 6
};
constexpr EntryAccess operator|(EntryAccess left, EntryAccess right) noexcept {
    return static_cast<EntryAccess>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}
constexpr bool valid_entry_access(EntryAccess access) noexcept {
    const auto bits = static_cast<unsigned>(access);
    return (bits & ~7U) == 0 && ((bits & 4U) == 0 || (bits & 2U) != 0);
}
constexpr bool requests_attributes(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 1U) != 0; }
constexpr bool requests_security(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 2U) != 0; }
constexpr bool requests_audit_security(EntryAccess access) noexcept { return (static_cast<unsigned>(access) & 4U) != 0; }

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
    virtual Observation<StorageInfo> inspect_storage() const;
    // Native channel identifiers and sizes only; no channel content is read.
    // Coverage is explicit. Unsupported is not evidence that channels are absent.
    // This live query does not establish complete metadata or snapshot stability.
    virtual Observation<DataChannelCatalog> inspect_data_channels(InventoryLimits limits = {}) const;
    virtual Observation<AttributeCatalog> inspect_attributes(InventoryLimits limits = {}) const;
    virtual Observation<AttributeValue> read_attribute(const NativeFeatureIdentifier& identifier, InventoryLimits limits = {}) const;
    virtual Observation<NativeSecurityValue> read_security(SecurityPart part, InventoryLimits limits = {}) const;
protected:
    OpenedEntry() = default;
};

} // namespace ck::files
