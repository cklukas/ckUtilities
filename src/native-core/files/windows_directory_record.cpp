// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_directory_record.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace ck::files::detail {
namespace {
std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t position) noexcept {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < 4; ++index)
        result |= std::to_integer<std::uint32_t>(bytes[position + index]) << (index * 8U);
    return result;
}
Observation<WindowsDirectoryRecord> invalid(const char* operation) {
    return Observation<WindowsDirectoryRecord>::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), operation});
}
}
Observation<WindowsDirectoryRecord> decode_windows_directory_record(
    std::span<const std::byte> bytes, std::size_t position) {
    constexpr auto header = windows_directory_name_offset;
    if (position % 8 != 0 || position > bytes.size() || bytes.size() - position < header)
        return invalid("directory.record_offset");
    const auto name_bytes = static_cast<std::size_t>(read_u32(bytes, position + windows_directory_name_length_offset));
    if (name_bytes == 0 || name_bytes % 2 != 0 || name_bytes > bytes.size() - position - header)
        return invalid("directory.name_length");
    const auto next = static_cast<std::size_t>(read_u32(bytes, position));
    std::optional<std::size_t> next_offset;
    if (next != 0) {
        if (next % 8 != 0 || next < header + name_bytes || next > bytes.size() - position - header)
            return invalid("directory.next_offset");
        next_offset = position + next;
    }
    std::u16string spelling(name_bytes / 2, u'\0');
    for (std::size_t index = 0; index < spelling.size(); ++index) {
        const auto at = position + header + index * 2;
        const auto unit = std::to_integer<std::uint32_t>(bytes[at]) |
            (std::to_integer<std::uint32_t>(bytes[at + 1]) << 8U);
        spelling[index] = static_cast<char16_t>(unit);
    }
    if (spelling == u"." || spelling == u"..")
        return Observation<WindowsDirectoryRecord>::available({std::nullopt, next_offset});
    auto name = NativeName::from_windows(std::move(spelling));
    if (!name) return invalid("directory.name");
    return Observation<WindowsDirectoryRecord>::available({std::move(name), next_offset});
}
} // namespace ck::files::detail
