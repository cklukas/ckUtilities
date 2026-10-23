// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "data_channels.hpp"
#include <functional>
#include <span>

namespace ck::files {
using NativeBytes = std::vector<std::byte>;
enum class AttributeRole { Opaque, AdditionalData, Security, StorageRepresentation, InterpretationRequired };
enum class AttributeCoverage { Unknown, CredentialsVisiblePosix, WindowsExtendedAttributes };
struct AttributeDescriptor {
    NativeFeatureIdentifier identifier;
    AttributeRole role = AttributeRole::Opaque;
    Observation<std::uint64_t> value_size;
    Observation<std::uint32_t> native_flags;
};
struct AttributeCatalog {
    std::vector<AttributeDescriptor> attributes;
    AttributeCoverage coverage = AttributeCoverage::Unknown;
    Observation<ObjectIdentity> owner;
};
struct AttributeValue {
    NativeFeatureIdentifier identifier;
    NativeBytes bytes;
    Observation<ObjectIdentity> owner;
    Observation<std::uint32_t> native_flags;
};
AttributeRole attribute_role(const NativeFeatureIdentifier& identifier, std::uint32_t flags = 0) noexcept;

namespace detail {
// An empty span requests size; a nonempty span requests bytes. A size race is
// explicit, not a generic failure retry. No denial/I/O failure is retried.
struct BufferTooSmall { NativeError error; };
using BufferReply = std::variant<std::size_t, BufferTooSmall, UnavailableObservation>;
using BufferQuery = std::function<BufferReply(std::span<std::byte>)>;
Observation<NativeBytes> read_attribute_bytes(const BufferQuery& query, InventoryLimits limits);
Observation<std::vector<NativeFeatureIdentifier>> decode_posix_attribute_names(
    std::span<const std::byte> bytes, InventoryLimits limits);
struct WindowsEaRecord {
    NativeFeatureIdentifier identifier;
    std::uint32_t flags;
    NativeBytes value;
};
// Exactly one FILE_FULL_EA_INFORMATION record, as requested by the adapter.
// A zero-length wire value can be a missing-name witness in a filtered query;
// it is not automatically an existing empty Windows attribute.
Observation<WindowsEaRecord> decode_windows_ea_record(std::span<const std::byte> bytes);
}
} // namespace ck::files
