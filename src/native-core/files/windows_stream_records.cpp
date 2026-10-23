// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_stream_records.hpp"

#include <cstdint>
#include <limits>
#include <set>
#include <utility>

namespace ck::files::detail {
namespace {
std::uint64_t number(std::span<const std::byte> bytes, std::size_t at, std::size_t count) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < count; ++index)
        value |= std::to_integer<std::uint64_t>(bytes[at + index]) << (index * 8U);
    return value;
}
Observation<DataChannelCatalog> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<DataChannelCatalog>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}
Observation<DataChannelCatalog> decode_windows_stream_records(std::span<const std::byte> bytes, InventoryLimits limits) {
    if (!limits.valid()) return error(ObservationState::Failed, AdapterError::InvalidLimit, "streams.limits");
    if (bytes.size() > limits.max_native_bytes)
        return error(ObservationState::Unknown, AdapterError::InventoryLimit, "streams.buffer_limit");
    DataChannelCatalog result;
    result.coverage = DataChannelCoverage::WindowsFileStreamInfo;
    std::set<std::u16string> identifiers;
    std::size_t position = 0;
    constexpr auto header = windows_stream_name_offset;
    for (;;) {
        if (position % 8 != 0 || position > bytes.size() || bytes.size() - position < header)
            return error(ObservationState::Failed, AdapterError::InvalidNativeResult, "streams.record_offset");
        const auto name_bytes = static_cast<std::size_t>(number(bytes, position + 4, 4));
        if (name_bytes == 0 || name_bytes % 2 != 0 || name_bytes > bytes.size() - position - header)
            return error(ObservationState::Failed, AdapterError::InvalidNativeResult, "streams.name_length");
        const auto logical = number(bytes, position + 8, 8);
        const auto allocation = number(bytes, position + 16, 8);
        if (logical > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            allocation > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return error(ObservationState::Failed, AdapterError::InvalidNativeResult, "streams.negative_size");
        const auto next = static_cast<std::size_t>(number(bytes, position, 4));
        if (next != 0 && (next % 8 != 0 || next < header + name_bytes || next > bytes.size() - position - header))
            return error(ObservationState::Failed, AdapterError::InvalidNativeResult, "streams.next_offset");
        if (result.channels.size() == limits.max_records)
            return error(ObservationState::Unknown, AdapterError::InventoryLimit, "streams.record_limit");
        std::u16string spelling(name_bytes / 2, u'\0');
        for (std::size_t index = 0; index < spelling.size(); ++index)
            spelling[index] = static_cast<char16_t>(number(bytes, position + header + index * 2, 2));
        auto identifier = NativeFeatureIdentifier::from_windows(spelling);
        if (!identifier || !identifiers.insert(spelling).second)
            return error(ObservationState::Failed, AdapterError::InvalidNativeResult, "streams.identifier");
        auto kind = DataChannelKind::Opaque;
        if (spelling == u"::$DATA") kind = DataChannelKind::MainData;
        else if (spelling.starts_with(u':') && spelling.ends_with(u":$DATA") && spelling.size() > 7 &&
                 spelling.substr(1, spelling.size() - 7).find(u':') == std::u16string::npos)
            kind = DataChannelKind::NamedData;
        result.channels.push_back({std::move(*identifier), kind,
            Observation<std::uint64_t>::available(logical), Observation<std::uint64_t>::available(allocation)});
        if (next == 0) return Observation<DataChannelCatalog>::available(std::move(result));
        position += next;
    }
}
} // namespace ck::files::detail
