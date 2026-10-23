// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "security.hpp"

namespace ck::files::detail {
namespace {
Observation<std::size_t> failure(ObservationState state, AdapterError code, const char* operation) {
    return Observation<std::size_t>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
std::uint32_t little(std::span<const std::byte> bytes, std::size_t at, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < count; ++index)
        value |= std::to_integer<std::uint32_t>(bytes[at + index]) << (index * 8U);
    return value;
}
}
Observation<std::size_t> validate_linux_acl(std::span<const std::byte> bytes, InventoryLimits limits) {
    if (!limits.valid()) return failure(ObservationState::Failed, AdapterError::InvalidLimit, "security.limits");
    if (bytes.size() > limits.max_native_bytes) return failure(ObservationState::Unknown, AdapterError::InventoryLimit, "security.bytes");
    if (bytes.size() < 4 || (bytes.size() - 4) % 8 != 0)
        return failure(ObservationState::Failed, AdapterError::InvalidNativeResult, "security.linux_acl_record");
    if (little(bytes, 0, 4) != 2) return failure(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "security.linux_acl_version");
    const auto count = (bytes.size() - 4) / 8;
    if (count > limits.max_records) return failure(ObservationState::Unknown, AdapterError::InventoryLimit, "security.records");
    return Observation<std::size_t>::available(count);
}
Observation<std::size_t> validate_windows_descriptor(std::span<const std::byte> bytes, InventoryLimits limits) {
    const auto invalid = [] { return failure(ObservationState::Failed, AdapterError::InvalidNativeResult, "security.relative_descriptor"); };
    if (!limits.valid()) return failure(ObservationState::Failed, AdapterError::InvalidLimit, "security.limits");
    if (bytes.size() > limits.max_native_bytes) return failure(ObservationState::Unknown, AdapterError::InventoryLimit, "security.bytes");
    if (bytes.size() < 20 || bytes[0] != std::byte{1} || (little(bytes, 2, 2) & 0x8000U) == 0) return invalid();
    const auto control = little(bytes, 2, 2);
    for (const auto field : {std::size_t{4}, std::size_t{8}}) {
        const auto offset = static_cast<std::size_t>(little(bytes, field, 4));
        if (offset == 0) continue;
        if (offset < 20 || offset % 4 != 0 || offset > bytes.size() || bytes.size() - offset < 8) return invalid();
        const auto subauthorities = std::to_integer<std::size_t>(bytes[offset + 1]);
        if (bytes[offset] != std::byte{1} || subauthorities > 15 || subauthorities * 4 > bytes.size() - offset - 8) return invalid();
    }
    std::size_t entries = 0;
    for (const auto field : {std::size_t{12}, std::size_t{16}}) {
        const auto offset = static_cast<std::size_t>(little(bytes, field, 4));
        const auto present = (control & (field == 12 ? 0x10U : 0x4U)) != 0;
        if (!present && offset != 0) return invalid();
        if (offset == 0) continue; // Present/null and absent ACL remain distinct in the original control bits.
        if (offset < 20 || offset % 4 != 0 || offset > bytes.size() || bytes.size() - offset < 8) return invalid();
        const auto size = static_cast<std::size_t>(little(bytes, offset + 2, 2));
        const auto count = static_cast<std::size_t>(little(bytes, offset + 4, 2));
        if (size < 8 || size > bytes.size() - offset) return invalid();
        if (count > limits.max_records - entries) return failure(ObservationState::Unknown, AdapterError::InventoryLimit, "security.records");
        entries += count;
        std::size_t at = 8;
        for (std::size_t index = 0; index < count; ++index) {
            if (size - at < 4) return invalid();
            const auto ace_size = static_cast<std::size_t>(little(bytes, offset + at + 2, 2));
            if (ace_size < 4 || ace_size % 4 != 0 || ace_size > size - at) return invalid();
            at += ace_size;
        }
    }
    return Observation<std::size_t>::available(entries);
}
} // namespace ck::files::detail
