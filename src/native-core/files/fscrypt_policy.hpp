// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "file_metadata.hpp"
#include <span>
namespace ck::files {
// Exact fscrypt policy bytes, not a key, unlock state, encryption-strength or
// replay/preservation guarantee. Mode/flag/reserved bytes remain uninterpreted.
class FscryptPolicyData final {
public:
    static constexpr std::size_t max_bytes = 24;
    static Observation<FscryptPolicyData> decode(std::span<const std::byte> bytes);
    std::span<const std::byte> raw() const noexcept { return std::span(bytes_).first(size_); }
    std::uint8_t version() const noexcept { return std::to_integer<std::uint8_t>(bytes_[0]); }
    std::uint8_t contents_mode() const noexcept { return std::to_integer<std::uint8_t>(bytes_[1]); }
    std::uint8_t filenames_mode() const noexcept { return std::to_integer<std::uint8_t>(bytes_[2]); }
    std::uint8_t flags() const noexcept { return std::to_integer<std::uint8_t>(bytes_[3]); }
    std::span<const std::byte> key_reference() const noexcept { return raw().subspan(version() == 0 ? 4 : 8); }
private:
    FscryptPolicyData(std::array<std::byte, max_bytes> bytes, std::size_t size) noexcept : bytes_(bytes), size_(size) {}
    std::array<std::byte, max_bytes> bytes_{};
    std::size_t size_;
};
struct FscryptPolicyAbsent { NativeError witness; }; // only native ENODATA, never an unsupported ioctl
struct DirectoryFscryptPolicy {
    ObjectIdentity directory;
    std::variant<FscryptPolicyAbsent, FscryptPolicyData> policy;
};
namespace detail {
// Held-directory identity/kind qualification and optional statx encryption-bit
// corroboration. Missing flag coverage never becomes a claim of no encryption.
Observation<ObjectIdentity> qualify_fscrypt_directory(const Observation<BasicMetadata>& before,
    const Observation<BasicMetadata>& after, std::optional<bool> policy_present = {});
}
} // namespace ck::files
