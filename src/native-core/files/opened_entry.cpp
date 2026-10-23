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
} // namespace ck::files
