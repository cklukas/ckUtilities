// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "data_channels.hpp"
#include <span>

namespace ck::files::detail {
inline constexpr std::size_t windows_stream_name_offset = 24;
// Decode a successful FILE_STREAM_INFO query. Failure/empty-list OS outcomes
// are handled separately by the adapter. Never accept a truncated prefix as a
// complete inventory. SDK offsets are checked in the native Windows adapter.
Observation<DataChannelCatalog> decode_windows_stream_records(
    std::span<const std::byte> bytes, InventoryLimits limits = {});
} // namespace ck::files::detail
