// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "opened_entry.hpp"

namespace ck::files {
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
} // namespace ck::files
