// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_delete_recovery_evidence.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace ck::files {
namespace {
constexpr std::array<std::byte, 4> magic{
    std::byte{'C'}, std::byte{'K'}, std::byte{'D'}, std::byte{'E'}};

template <class T>
Observation<T> invalid(AdapterError code, const char* operation,
                       ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

void put_u8(std::vector<std::byte>& bytes, std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}
void put_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
    put_u8(bytes, static_cast<std::uint8_t>(value));
    put_u8(bytes, static_cast<std::uint8_t>(value >> 8U));
}
void put_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        put_u8(bytes, static_cast<std::uint8_t>(value >> shift));
}
void put_identifier(std::vector<std::byte>& bytes, const NativeIdentifier& value) {
    bytes.insert(bytes.end(), value.bytes.begin(), value.bytes.end());
}

std::optional<std::uint8_t> scheme_byte(IdentityScheme scheme) noexcept {
    switch (scheme) {
    case IdentityScheme::PosixDeviceInode: return std::uint8_t{1};
    case IdentityScheme::WindowsFileId128: return std::uint8_t{2};
    case IdentityScheme::WindowsFileIndex64: return std::uint8_t{3};
    case IdentityScheme::Unknown: return std::nullopt;
    }
    return std::nullopt;
}

std::optional<IdentityScheme> identity_scheme(std::uint8_t value) noexcept {
    switch (value) {
    case 1: return IdentityScheme::PosixDeviceInode;
    case 2: return IdentityScheme::WindowsFileId128;
    case 3: return IdentityScheme::WindowsFileIndex64;
    default: return std::nullopt;
    }
}

bool append_name(std::vector<std::byte>& bytes, const NativeName& name, std::size_t max_units) {
    if (!name.is_valid()) return false;
    if (const auto* posix = name.posix_bytes()) {
        if (posix->size() > max_units || posix->size() > std::numeric_limits<std::uint32_t>::max()) return false;
        put_u8(bytes, 0);
        put_u64(bytes, static_cast<std::uint64_t>(posix->size()));
        for (const auto value : *posix) put_u8(bytes, static_cast<std::uint8_t>(value));
        return true;
    }
    const auto* windows = name.windows_units();
    if (!windows || windows->size() > max_units || windows->size() > std::numeric_limits<std::uint32_t>::max()) return false;
    put_u8(bytes, 1);
    put_u64(bytes, static_cast<std::uint64_t>(windows->size()));
    for (const auto value : *windows) put_u16(bytes, value);
    return true;
}

bool append_identity(std::vector<std::byte>& bytes, const NativeDurableIdentity& identity) {
    const auto scheme = scheme_byte(identity.scheme);
    if (!scheme || identity.object == NativeIdentifier{}) return false;
    put_u8(bytes, *scheme);
    put_u8(bytes, identity.generation ? 1 : 0);
    put_u16(bytes, 0);
    put_identifier(bytes, identity.volume);
    put_identifier(bytes, identity.object);
    if (identity.generation) put_u64(bytes, *identity.generation);
    return true;
}

class Reader final {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}
    bool take(std::size_t count, std::span<const std::byte>& result) noexcept {
        if (count > bytes_.size() - offset_) return false;
        result = bytes_.subspan(offset_, count);
        offset_ += count;
        return true;
    }
    bool u8(std::uint8_t& result) noexcept {
        std::span<const std::byte> value;
        if (!take(1, value)) return false;
        result = static_cast<std::uint8_t>(value[0]);
        return true;
    }
    bool u16(std::uint16_t& result) noexcept {
        std::uint8_t a = 0, b = 0;
        if (!u8(a) || !u8(b)) return false;
        result = static_cast<std::uint16_t>(a) | (static_cast<std::uint16_t>(b) << 8U);
        return true;
    }
    bool u64(std::uint64_t& result) noexcept {
        result = 0;
        for (unsigned shift = 0; shift != 64; shift += 8) {
            std::uint8_t value = 0;
            if (!u8(value)) return false;
            result |= static_cast<std::uint64_t>(value) << shift;
        }
        return true;
    }
    bool empty() const noexcept { return offset_ == bytes_.size(); }
private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

Observation<NativeName> read_name(Reader& reader, std::size_t max_units) {
    std::uint8_t encoding = 0;
    std::uint64_t length = 0;
    if (!reader.u8(encoding) || !reader.u64(length) || length > max_units || length > std::numeric_limits<std::size_t>::max())
        return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_delete_evidence.name_header");
    if (encoding == 0) {
        std::span<const std::byte> raw;
        if (!reader.take(static_cast<std::size_t>(length), raw))
            return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_delete_evidence.name_bytes");
        std::string value;
        value.reserve(raw.size());
        for (const auto byte : raw) value.push_back(static_cast<char>(static_cast<unsigned char>(byte)));
        auto name = NativeName::from_posix(std::move(value));
        if (!name) return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_delete_evidence.name_posix");
        return Observation<NativeName>::available(std::move(*name));
    }
    if (encoding != 1)
        return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_delete_evidence.name_encoding");
    std::u16string value;
    value.reserve(static_cast<std::size_t>(length));
    for (std::uint64_t index = 0; index != length; ++index) {
        std::uint16_t unit = 0;
        if (!reader.u16(unit)) return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_delete_evidence.name_units");
        value.push_back(static_cast<char16_t>(unit));
    }
    auto name = NativeName::from_windows(std::move(value));
    if (!name) return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_delete_evidence.name_windows");
    return Observation<NativeName>::available(std::move(*name));
}

Observation<NativeDurableIdentity> read_identity(Reader& reader) {
    std::uint8_t scheme = 0, flags = 0;
    std::uint16_t reserved = 0;
    if (!reader.u8(scheme) || !reader.u8(flags) || !reader.u16(reserved) || reserved != 0 || flags > 1)
        return invalid<NativeDurableIdentity>(AdapterError::InvalidNativeResult, "native_delete_evidence.identity_header");
    const auto decoded_scheme = identity_scheme(scheme);
    if (!decoded_scheme) return invalid<NativeDurableIdentity>(AdapterError::InvalidNativeResult, "native_delete_evidence.identity_scheme");
    std::span<const std::byte> volume, object;
    if (!reader.take(16, volume) || !reader.take(16, object))
        return invalid<NativeDurableIdentity>(AdapterError::InvalidNativeResult, "native_delete_evidence.identity_bytes");
    NativeDurableIdentity result;
    result.scheme = *decoded_scheme;
    std::copy(volume.begin(), volume.end(), result.volume.bytes.begin());
    std::copy(object.begin(), object.end(), result.object.bytes.begin());
    if (result.object == NativeIdentifier{}) return invalid<NativeDurableIdentity>(AdapterError::UnqualifiedIdentity, "native_delete_evidence.identity_object");
    if (flags != 0) {
        std::uint64_t generation = 0;
        if (!reader.u64(generation)) return invalid<NativeDurableIdentity>(AdapterError::InvalidNativeResult, "native_delete_evidence.identity_generation");
        result.generation = generation;
    }
    return Observation<NativeDurableIdentity>::available(std::move(result));
}
} // namespace

Observation<std::vector<std::byte>> encode_native_delete_recovery_evidence(
    const NativeDeleteRecoveryEvidence& evidence, NativeOperationRecoveryEvidenceLimits limits) {
    if (limits.max_bytes == 0 || limits.max_bytes > NativeOperationRecoveryEvidenceLimits::hard_max_bytes ||
        limits.max_name_units == 0 || limits.max_name_units > NativeOperationRecoveryEvidenceLimits::hard_max_name_units)
        return invalid<std::vector<std::byte>>(AdapterError::InvalidLimit, "native_delete_evidence.limits");
    std::vector<std::byte> result;
    result.reserve(128);
    result.insert(result.end(), magic.begin(), magic.end());
    put_u8(result, 1);
    put_u8(result, 0);
    if (!append_name(result, evidence.source_name, limits.max_name_units) ||
        !append_identity(result, evidence.source_parent_identity) ||
        !append_identity(result, evidence.source_identity))
        return invalid<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "native_delete_evidence.encode");
    if (result.size() > limits.max_bytes)
        return invalid<std::vector<std::byte>>(AdapterError::InvalidLimit, "native_delete_evidence.size");
    return Observation<std::vector<std::byte>>::available(std::move(result));
}

Observation<NativeDeleteRecoveryEvidence> decode_native_delete_recovery_evidence(
    std::span<const std::byte> bytes, NativeOperationRecoveryEvidenceLimits limits) {
    if (limits.max_bytes == 0 || limits.max_bytes > NativeOperationRecoveryEvidenceLimits::hard_max_bytes ||
        limits.max_name_units == 0 || limits.max_name_units > NativeOperationRecoveryEvidenceLimits::hard_max_name_units)
        return invalid<NativeDeleteRecoveryEvidence>(AdapterError::InvalidLimit, "native_delete_evidence.limits");
    if (bytes.size() > limits.max_bytes) return invalid<NativeDeleteRecoveryEvidence>(AdapterError::InvalidLimit, "native_delete_evidence.size");
    Reader reader(bytes);
    std::span<const std::byte> decoded_magic;
    std::uint8_t version = 0, reserved = 0;
    if (!reader.take(magic.size(), decoded_magic) || !std::equal(magic.begin(), magic.end(), decoded_magic.begin()) ||
        !reader.u8(version) || !reader.u8(reserved) || version != 1 || reserved != 0)
        return invalid<NativeDeleteRecoveryEvidence>(AdapterError::InvalidNativeResult, "native_delete_evidence.header");
    auto name = read_name(reader, limits.max_name_units);
    auto parent = read_identity(reader);
    auto source = read_identity(reader);
    if (!name.value() || !parent.value() || !source.value() || !reader.empty())
        return invalid<NativeDeleteRecoveryEvidence>(AdapterError::InvalidNativeResult, "native_delete_evidence.trailing");
    return Observation<NativeDeleteRecoveryEvidence>::available({
        std::move(name).take_value(), std::move(parent).take_value(), std::move(source).take_value()});
}
} // namespace ck::files
