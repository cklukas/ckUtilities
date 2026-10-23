// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_metadata.hpp"

namespace ck::files {

NativeIdentifier NativeIdentifier::from_u64(std::uint64_t value) noexcept {
    NativeIdentifier result;
    for (std::size_t index = 0; index < 8; ++index) {
        result.bytes[index] = static_cast<std::byte>(value & 0xFFU);
        value >>= 8U;
    }
    return result;
}

IdentityComparison compare_identity(const ObjectIdentity& left, const ObjectIdentity& right) noexcept {
    if (!left.scope.is_valid() || !right.scope.is_valid() || left.scope != right.scope || left.scheme != right.scheme)
        return IdentityComparison::Unknown;
    switch (left.scheme) {
    case IdentityScheme::PosixDeviceInode:
    case IdentityScheme::WindowsFileId128:
    case IdentityScheme::WindowsFileIndex64:
        break;
    default:
        return IdentityComparison::Unknown;
    }
    // Some filesystems report a successful query with an unavailable all-zero
    // object ID. Never equate two such sentinels, even in the same scope.
    if (left.object == NativeIdentifier{} || right.object == NativeIdentifier{}) return IdentityComparison::Unknown;
    if (left.volume != right.volume || left.object != right.object) return IdentityComparison::Different;
    if (left.generation.has_value() != right.generation.has_value()) return IdentityComparison::Unknown;
    return left.generation == right.generation ? IdentityComparison::Same : IdentityComparison::Different;
}

std::optional<FileTimestamp> FileTimestamp::from_unix(std::int64_t seconds, std::uint32_t nanos) noexcept {
    if (nanos >= 1'000'000'000U) return std::nullopt;
    return FileTimestamp(seconds, nanos);
}

FileTimestamp FileTimestamp::from_windows_ticks(std::uint64_t ticks) noexcept {
    constexpr std::uint64_t ticks_per_second = 10'000'000;
    constexpr std::int64_t epoch_seconds = 11'644'473'600;
    // Divide before subtracting the epoch: both extreme tick values and times
    // before 1970 are representable without signed subtraction overflow.
    const auto seconds = static_cast<std::int64_t>(ticks / ticks_per_second) - epoch_seconds;
    const auto nanos = static_cast<std::uint32_t>((ticks % ticks_per_second) * 100U);
    return FileTimestamp(seconds, nanos);
}

} // namespace ck::files
