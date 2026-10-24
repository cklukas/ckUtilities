// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "windows_parent_record.hpp"
#include <algorithm>
namespace ck::files::detail {
namespace {
std::uint32_t u32(std::span<const std::byte> bytes, std::size_t at, std::size_t count = 4) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= std::to_integer<std::uint32_t>(bytes[at + i]) << (i * 8U);
    return value;
}
Observation<WindowsParentIds> failed(AdapterError code, const char* operation) {
    return Observation<WindowsParentIds>::unavailable(ObservationState::Failed, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}
}
Observation<WindowsParentIds> decode_windows_parent_record(std::span<const std::byte> bytes, unsigned requested_major) {
    using Result = Observation<WindowsParentIds>;
    if (requested_major != 2 && requested_major != 3) return failed(AdapterError::InvalidIdentifier, "parent.usn_version_request");
    if (bytes.size() < 8 || u32(bytes, 0) != bytes.size() || bytes.size() % 8 != 0)
        return failed(AdapterError::InvalidNativeResult, "parent.usn_length");
    if (u32(bytes, 4, 2) != requested_major || u32(bytes, 6, 2) != 0)
        return failed(AdapterError::InvalidNativeResult, "parent.usn_version");
    const std::size_t id_size = requested_major == 3 ? 16 : 8, header = requested_major == 3 ? 76 : 60;
    if (bytes.size() < header) return failed(AdapterError::InvalidNativeResult, "parent.usn_header");
    const auto name_bytes = u32(bytes, header - 4, 2), name_offset = u32(bytes, header - 2, 2);
    if (name_bytes % 2 != 0 || name_offset % 2 != 0 || name_offset < header || name_offset > bytes.size() || name_bytes > bytes.size() - name_offset)
        return failed(AdapterError::InvalidNativeResult, "parent.usn_name");
    WindowsParentIds result{requested_major == 3 ? IdentityScheme::WindowsFileId128 : IdentityScheme::WindowsFileIndex64, {}, {}, u32(bytes, header - 8)};
    std::copy_n(bytes.begin() + 8, id_size, result.child.bytes.begin());
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(8 + id_size), id_size, result.parent.bytes.begin());
    if (result.child == NativeIdentifier{} || result.parent == NativeIdentifier{})
        return Result::unavailable(ObservationState::Unknown, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "parent.usn_identity"});
    return Result::available(std::move(result));
}
} // namespace ck::files::detail
