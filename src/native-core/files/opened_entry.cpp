// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "opened_entry.hpp"
#include "native_name.hpp"

namespace ck::files {
Observation<std::unique_ptr<OpenedEntry>> OpenedEntry::open_parent_directory() const {
    return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "parent.native_query"});
}
Observation<FileDataLayout> OpenedEntry::inspect_data_layout(InventoryLimits limits, std::stop_token stop) const {
    const bool valid = limits.valid(), cancelled = stop.stop_requested();
    return Observation<FileDataLayout>::unavailable(!valid ? ObservationState::Failed : cancelled ? ObservationState::Unknown : ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(!valid ? AdapterError::InvalidLimit : cancelled ? AdapterError::InterruptedRead : AdapterError::UnsupportedQuery), "layout.native_query"});
}
Observation<LinkMetadata> OpenedEntry::read_link_metadata(InventoryLimits limits) const {
    return Observation<LinkMetadata>::unavailable(limits.valid() ? ObservationState::Unsupported : ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(limits.valid() ? AdapterError::UnsupportedQuery : AdapterError::InvalidLimit), "link.native_query"});
}
Observation<FileLinkLimits> OpenedEntry::inspect_link_limits() const {
    return Observation<FileLinkLimits>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "entry.link_limits"});
}
Observation<FileIntegrityInfo> OpenedEntry::inspect_integrity(InventoryLimits limits) const {
    return Observation<FileIntegrityInfo>::unavailable(limits.valid() ? ObservationState::Unsupported : ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(limits.valid() ? AdapterError::UnsupportedQuery : AdapterError::InvalidLimit), "integrity.native_query"});
}
Observation<FileInodeFlags> OpenedEntry::inspect_inode_flags(InventoryLimits limits) const {
    return Observation<FileInodeFlags>::unavailable(limits.valid() ? ObservationState::Unsupported : ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(limits.valid() ? AdapterError::UnsupportedQuery : AdapterError::InvalidLimit), "inode_flags.native_query"});
}
Observation<StorageInfo> OpenedEntry::inspect_storage() const {
    return Observation<StorageInfo>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "storage.native_query"});
}
Observation<DataChannelCatalog> OpenedEntry::inspect_data_channels(InventoryLimits limits) const {
    if (!limits.valid())
        return Observation<DataChannelCatalog>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "channels.limits"});
    return Observation<DataChannelCatalog>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "channels.native_query"});
}
Observation<AttributeCatalog> OpenedEntry::inspect_attributes(InventoryLimits limits) const {
    return Observation<AttributeCatalog>::unavailable(limits.valid() ? ObservationState::Unsupported : ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(limits.valid() ? AdapterError::UnsupportedQuery : AdapterError::InvalidLimit), "attributes.native_query"});
}
Observation<AttributeValue> OpenedEntry::read_attribute(const NativeFeatureIdentifier&, InventoryLimits limits) const {
    return Observation<AttributeValue>::unavailable(limits.valid() ? ObservationState::Unsupported : ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(limits.valid() ? AdapterError::UnsupportedQuery : AdapterError::InvalidLimit), "attributes.native_query"});
}
Observation<NativeSecurityValue> OpenedEntry::read_security(SecurityPart part, InventoryLimits limits) const {
    if (!valid_security_part(part) || !limits.valid())
        return Observation<NativeSecurityValue>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(!valid_security_part(part) ? AdapterError::InvalidIdentifier : AdapterError::InvalidLimit), "security.query"});
    return Observation<NativeSecurityValue>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "security.native_query"});
}
Observation<ContentRead> OpenedEntry::read_content(ContentReadLimits limits, std::uint64_t, std::stop_token stop) const {
    if (!limits.valid())
        return Observation<ContentRead>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "content.limits"});
    return Observation<ContentRead>::unavailable(
        stop.stop_requested() ? ObservationState::Unknown : ObservationState::Unsupported,
        {ErrorDomain::Adapter,
         static_cast<std::int64_t>(stop.stop_requested() ? AdapterError::InterruptedRead : AdapterError::UnsupportedQuery),
         "content.native_query"});
}
Observation<std::size_t> OpenedEntry::write_content(std::uint64_t, std::span<const std::byte> bytes,
                                                    ContentWriteLimits limits, std::stop_token stop) {
    if (!limits.valid() || bytes.size() > limits.max_bytes)
        return Observation<std::size_t>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "content.write_limits"});
    return Observation<std::size_t>::unavailable(
        stop.stop_requested() ? ObservationState::Unknown : ObservationState::Unsupported,
        {ErrorDomain::Adapter,
         static_cast<std::int64_t>(stop.stop_requested() ? AdapterError::InterruptedRead : AdapterError::UnsupportedQuery),
         "content.write_native_query"});
}
Observation<bool> OpenedEntry::flush_content() {
    return Observation<bool>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "content.flush_native_query"});
}
Observation<bool> OpenedEntry::apply_portable_basic_metadata(const BasicMetadata&) {
    return Observation<bool>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery),
         "metadata.portable_basic_native_query"});
}
Observation<bool> OpenedEntry::apply_nonsecurity_attribute(const AttributeValue&) {
    return Observation<bool>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery),
         "attributes.nonsecurity_native_query"});
}
Observation<bool> OpenedEntry::publish_noreplace(const DirectoryCursor&, const NativeName& name) {
    if (!name.is_valid())
        return Observation<bool>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "publish_noreplace.name"});
    return Observation<bool>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "publish_noreplace.native_query"});
}
} // namespace ck::files
