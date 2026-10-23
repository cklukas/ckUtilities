// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "link_metadata.hpp"
#include <algorithm>

namespace ck::files::detail {
namespace {
template <class T> Observation<T> error(ObservationState state, AdapterError code, const char* operation) {
    return Observation<T>::unavailable(state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
std::uint32_t little(std::span<const std::byte> bytes, std::size_t at, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < count; ++index)
        value |= std::to_integer<std::uint32_t>(bytes[at + index]) << (index * 8U);
    return value;
}
}
Observation<PosixSymlinkData> read_posix_link(const LinkReadQuery& query, InventoryLimits limits) {
    using Result = Observation<PosixSymlinkData>;
    if (!limits.valid()) return error<PosixSymlinkData>(ObservationState::Failed, AdapterError::InvalidLimit, "link.limits");
    const auto maximum = limits.max_native_bytes + 1;
    auto capacity = std::min<std::size_t>(256, maximum);
    for (;;) {
        NativeBytes bytes(capacity);
        const auto read = query(bytes);
        if (const auto* failure = std::get_if<UnavailableObservation>(&read)) return Result::unavailable(failure->state, failure->error);
        const auto count = std::get<std::size_t>(read);
        if (count > capacity) return error<PosixSymlinkData>(ObservationState::Failed, AdapterError::InvalidNativeResult, "readlink.count");
        if (count > limits.max_native_bytes) return error<PosixSymlinkData>(ObservationState::Unknown, AdapterError::InventoryLimit, "readlink.target_limit");
        if (count < capacity) {
            std::string target;
            target.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                if (bytes[index] == std::byte{}) return error<PosixSymlinkData>(ObservationState::Failed, AdapterError::InvalidNativeResult, "readlink.embedded_nul");
                target.push_back(static_cast<char>(std::to_integer<unsigned char>(bytes[index])));
            }
            return Result::available({std::move(target)});
        }
        if (capacity == maximum) return error<PosixSymlinkData>(ObservationState::Unknown, AdapterError::InventoryLimit, "readlink.target_limit");
        capacity = capacity > maximum / 2 ? maximum : capacity * 2;
    }
}
Observation<WindowsReparseData> decode_windows_reparse(std::span<const std::byte> bytes, InventoryLimits limits) {
    using Result = Observation<WindowsReparseData>;
    const auto invalid = [] { return error<WindowsReparseData>(ObservationState::Failed, AdapterError::InvalidNativeResult, "reparse.record"); };
    if (!limits.valid()) return error<WindowsReparseData>(ObservationState::Failed, AdapterError::InvalidLimit, "link.limits");
    if (bytes.size() > limits.max_native_bytes) return error<WindowsReparseData>(ObservationState::Unknown, AdapterError::InventoryLimit, "reparse.buffer_limit");
    if (bytes.size() < 8 || bytes.size() > windows_reparse_max_bytes) return invalid();
    const auto tag = little(bytes, 0, 4);
    if (tag < 2) return invalid();
    const bool microsoft = (tag & 0x80000000U) != 0;
    const std::size_t header = microsoft ? 8 : 24;
    const auto length = static_cast<std::size_t>(little(bytes, 4, 2));
    if (bytes.size() < header || length != bytes.size() - header) return invalid();
    WindowsReparseData result;
    result.tag = tag;
    result.raw.assign(bytes.begin(), bytes.end());
    result.kind = microsoft ? WindowsReparseKind::OpaqueMicrosoft : WindowsReparseKind::OpaqueGuid;
    if (!microsoft) {
        NativeIdentifier guid;
        std::copy_n(bytes.begin() + 8, 16, guid.bytes.begin());
        result.owner_guid = guid;
    } else if (tag == 0xa000000cU || tag == 0xa0000003U) {
        const bool symlink = tag == 0xa000000cU;
        const std::size_t prefix = symlink ? 12 : 8;
        if (length < prefix || (length - prefix) % 2 != 0) return invalid();
        const auto path_bytes = length - prefix;
        const auto string = [&](std::size_t field) -> std::optional<std::u16string> {
            const auto offset = static_cast<std::size_t>(little(bytes, field, 2));
            const auto size = static_cast<std::size_t>(little(bytes, field + 2, 2));
            if (offset % 2 != 0 || size % 2 != 0 || offset > path_bytes || size > path_bytes - offset) return std::nullopt;
            std::u16string text;
            for (std::size_t at = 0; at < size; at += 2) {
                const auto unit = static_cast<char16_t>(little(bytes, 8 + prefix + offset + at, 2));
                if (unit == u'\0') return std::nullopt;
                text.push_back(unit);
            }
            return text;
        };
        auto substitute = string(8);
        auto print = string(12);
        if (!substitute || substitute->empty() || !print) return invalid();
        result.names = WindowsLinkNames{std::move(*substitute), std::move(*print), symlink ? little(bytes, 16, 4) : 0};
        result.kind = symlink ? WindowsReparseKind::SymbolicLink : WindowsReparseKind::MountPoint;
    }
    return Result::available(std::move(result));
}
} // namespace ck::files::detail
