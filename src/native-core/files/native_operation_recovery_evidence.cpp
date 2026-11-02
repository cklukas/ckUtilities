// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_operation_recovery_evidence.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace ck::files {
namespace {
constexpr std::array<std::byte, 4> magic{
    std::byte{'C'}, std::byte{'K'}, std::byte{'R'}, std::byte{'E'}};
constexpr std::uint8_t address_version = 3;
constexpr std::uint8_t digest_version = 4;
constexpr std::uint8_t published_identity_version = 5;
constexpr std::uint8_t staging_identity_version = 2;
constexpr std::uint8_t legacy_version = 1;

template <class T>
Observation<T> invalid(AdapterError code, const char* operation,
                       ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

bool valid_limits(const NativeOperationRecoveryEvidenceLimits& limits) noexcept {
    return limits.max_bytes > 0 && limits.max_bytes <= NativeOperationRecoveryEvidenceLimits::hard_max_bytes &&
        limits.max_name_units > 0 && limits.max_name_units <= NativeOperationRecoveryEvidenceLimits::hard_max_name_units;
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

void put_u8(std::vector<std::byte>& bytes, std::uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
void put_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
    put_u8(bytes, static_cast<std::uint8_t>(value));
    put_u8(bytes, static_cast<std::uint8_t>(value >> 8U));
}
void put_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) put_u8(bytes, static_cast<std::uint8_t>(value >> shift));
}
void put_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8) put_u8(bytes, static_cast<std::uint8_t>(value >> shift));
}
void put_identifier(std::vector<std::byte>& bytes, const NativeIdentifier& value) {
    bytes.insert(bytes.end(), value.bytes.begin(), value.bytes.end());
}

void put_digest(std::vector<std::byte>& bytes, const std::array<std::byte, 32>& digest) {
    bytes.insert(bytes.end(), digest.begin(), digest.end());
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
    bool u32(std::uint32_t& result) noexcept {
        std::uint8_t a = 0, b = 0, c = 0, d = 0;
        if (!u8(a) || !u8(b) || !u8(c) || !u8(d)) return false;
        result = static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8U) |
            (static_cast<std::uint32_t>(c) << 16U) | (static_cast<std::uint32_t>(d) << 24U);
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

bool append_name(std::vector<std::byte>& bytes, const NativeName& name, std::size_t max_units) {
    if (!name.is_valid()) return false;
    if (const auto* posix = name.posix_bytes()) {
        if (posix->size() > max_units || posix->size() > std::numeric_limits<std::uint32_t>::max()) return false;
        put_u8(bytes, 0);
        put_u32(bytes, static_cast<std::uint32_t>(posix->size()));
        for (const auto value : *posix) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
        return true;
    }
    const auto* windows = name.windows_units();
    if (!windows || windows->size() > max_units || windows->size() > std::numeric_limits<std::uint32_t>::max()) return false;
    put_u8(bytes, 1);
    put_u32(bytes, static_cast<std::uint32_t>(windows->size()));
    for (const auto value : *windows) put_u16(bytes, value);
    return true;
}

bool append_address(std::vector<std::byte>& bytes, const NativeAbsolutePath& address,
                    std::size_t max_bytes) {
    if (!address.is_valid()) return false;
    put_u8(bytes, static_cast<std::uint8_t>(address.encoding()));
    if (const auto* posix = address.posix_bytes()) {
        if (posix->size() > max_bytes || posix->size() > std::numeric_limits<std::uint32_t>::max()) return false;
        put_u32(bytes, static_cast<std::uint32_t>(posix->size()));
        for (const auto value : *posix) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
        return true;
    }
    const auto* windows = address.windows_units();
    if (!windows || windows->size() > max_bytes / 2 || windows->size() > std::numeric_limits<std::uint32_t>::max())
        return false;
    put_u32(bytes, static_cast<std::uint32_t>(windows->size()));
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

Observation<NativeName> read_name(Reader& reader, std::size_t max_units) {
    std::uint8_t encoding = 0;
    std::uint32_t length = 0;
    if (!reader.u8(encoding) || !reader.u32(length) || length > max_units)
        return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_recovery_evidence.name_header");
    if (encoding == 0) {
        std::span<const std::byte> raw;
        if (!reader.take(length, raw)) return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_recovery_evidence.name_bytes");
        std::string value;
        value.reserve(length);
        for (const auto byte : raw) value.push_back(static_cast<char>(static_cast<unsigned char>(byte)));
        auto name = NativeName::from_posix(std::move(value));
        if (!name) return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_recovery_evidence.name_posix");
        return Observation<NativeName>::available(std::move(*name));
    }
    if (encoding != 1) return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_recovery_evidence.name_encoding");
    std::u16string value;
    value.reserve(length);
    for (std::uint32_t index = 0; index != length; ++index) {
        std::uint16_t unit = 0;
        if (!reader.u16(unit)) return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_recovery_evidence.name_units");
        value.push_back(static_cast<char16_t>(unit));
    }
    auto name = NativeName::from_windows(std::move(value));
    if (!name) return invalid<NativeName>(AdapterError::InvalidNativeResult, "native_recovery_evidence.name_windows");
    return Observation<NativeName>::available(std::move(*name));
}

Observation<NativeAbsolutePath> read_address(Reader& reader, std::size_t max_bytes) {
    std::uint8_t encoding = 0;
    std::uint32_t length = 0;
    if (!reader.u8(encoding) || !reader.u32(length))
        return invalid<NativeAbsolutePath>(AdapterError::InvalidNativeResult,
                                           "native_recovery_evidence.address_header");
    if (encoding == static_cast<std::uint8_t>(NameEncoding::PosixBytes)) {
        if (length > max_bytes) return invalid<NativeAbsolutePath>(AdapterError::InventoryLimit,
                                                                    "native_recovery_evidence.address_size");
        std::span<const std::byte> raw;
        if (!reader.take(length, raw)) return invalid<NativeAbsolutePath>(AdapterError::InvalidNativeResult,
                                                                           "native_recovery_evidence.address_bytes");
        std::string value;
        value.reserve(length);
        for (const auto byte : raw) value.push_back(static_cast<char>(static_cast<unsigned char>(byte)));
        return NativeAbsolutePath::from_posix(std::move(value));
    }
    if (encoding != static_cast<std::uint8_t>(NameEncoding::WindowsUtf16) || length > max_bytes / 2)
        return invalid<NativeAbsolutePath>(AdapterError::InvalidNativeResult,
                                           "native_recovery_evidence.address_encoding");
    std::u16string value;
    value.reserve(length);
    for (std::uint32_t index = 0; index != length; ++index) {
        std::uint16_t unit = 0;
        if (!reader.u16(unit)) return invalid<NativeAbsolutePath>(AdapterError::InvalidNativeResult,
                                                                   "native_recovery_evidence.address_units");
        value.push_back(static_cast<char16_t>(unit));
    }
    return NativeAbsolutePath::from_windows(std::move(value));
}

Observation<NativeDurableIdentity> read_identity(Reader& reader) {
    std::uint8_t scheme = 0, flags = 0;
    std::uint16_t reserved = 0;
    if (!reader.u8(scheme) || !reader.u8(flags) || !reader.u16(reserved) || reserved != 0 || flags > 1)
        return invalid<NativeDurableIdentity>(AdapterError::InvalidNativeResult, "native_recovery_evidence.identity_header");
    const auto decoded_scheme = identity_scheme(scheme);
    if (!decoded_scheme) return invalid<NativeDurableIdentity>(AdapterError::InvalidNativeResult, "native_recovery_evidence.identity_scheme");
    std::span<const std::byte> volume, object;
    if (!reader.take(16, volume) || !reader.take(16, object)) return invalid<NativeDurableIdentity>(AdapterError::InvalidNativeResult, "native_recovery_evidence.identity_bytes");
    NativeDurableIdentity result;
    result.scheme = *decoded_scheme;
    std::copy(volume.begin(), volume.end(), result.volume.bytes.begin());
    std::copy(object.begin(), object.end(), result.object.bytes.begin());
    if (result.object == NativeIdentifier{}) return invalid<NativeDurableIdentity>(AdapterError::UnqualifiedIdentity, "native_recovery_evidence.identity_object");
    if (flags != 0) {
        std::uint64_t generation = 0;
        if (!reader.u64(generation)) return invalid<NativeDurableIdentity>(AdapterError::InvalidNativeResult, "native_recovery_evidence.identity_generation");
        result.generation = generation;
    }
    return Observation<NativeDurableIdentity>::available(std::move(result));
}
} // namespace

std::optional<NativeDurableIdentity> NativeDurableIdentity::from_live(const ObjectIdentity& value) noexcept {
    if (!value.scope.is_valid() || !scheme_byte(value.scheme) || value.object == NativeIdentifier{}) return std::nullopt;
    return NativeDurableIdentity{value.scheme, value.volume, value.object, value.generation};
}

bool NativeDurableIdentity::matches_live(const ObjectIdentity& value) const noexcept {
    const auto live = from_live(value);
    return live && *live == *this;
}

Observation<std::vector<std::byte>> encode_native_operation_recovery_evidence(
    const NativeOperationRecoveryEvidence& evidence, NativeOperationRecoveryEvidenceLimits limits) {
    if (!valid_limits(limits)) return invalid<std::vector<std::byte>>(AdapterError::InvalidLimit, "native_recovery_evidence.limits");
    std::vector<std::byte> result;
    result.reserve(256);
    result.insert(result.end(), magic.begin(), magic.end());
    put_u8(result, evidence.published_identity ? published_identity_version :
        (evidence.content_digest ? digest_version :
         (evidence.destination_address ? address_version : staging_identity_version)));
    put_u8(result, 0);
    if (!append_name(result, evidence.staging_name, limits.max_name_units) ||
        !append_name(result, evidence.destination_name, limits.max_name_units))
        return invalid<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "native_recovery_evidence.encode");
    put_u8(result, evidence.staging_identity ? 1 : 0);
    if ((evidence.staging_identity && !append_identity(result, *evidence.staging_identity)) ||
        !append_identity(result, evidence.source_parent_identity) ||
        !append_identity(result, evidence.source_identity) ||
        !append_identity(result, evidence.destination_identity))
        return invalid<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "native_recovery_evidence.encode");
    if (evidence.content_digest) {
        put_u8(result, evidence.destination_address ? 1 : 0);
        if (evidence.destination_address && !append_address(result, *evidence.destination_address,
                                                             NativePathLimits::hard_native_bytes))
            return invalid<std::vector<std::byte>>(AdapterError::InvalidNativeResult,
                                                   "native_recovery_evidence.address");
        put_digest(result, *evidence.content_digest);
        if (evidence.published_identity) {
            put_u8(result, 1);
            if (!append_identity(result, *evidence.published_identity))
                return invalid<std::vector<std::byte>>(AdapterError::InvalidNativeResult,
                                                       "native_recovery_evidence.published_identity");
        }
    } else if (evidence.published_identity) {
        return invalid<std::vector<std::byte>>(AdapterError::InvalidNativeResult,
                                               "native_recovery_evidence.published_digest");
    } else if (evidence.destination_address && !append_address(result, *evidence.destination_address,
                                                               NativePathLimits::hard_native_bytes))
        return invalid<std::vector<std::byte>>(AdapterError::InvalidNativeResult,
                                               "native_recovery_evidence.address");
    if (result.size() > limits.max_bytes) return invalid<std::vector<std::byte>>(AdapterError::InvalidLimit, "native_recovery_evidence.size");
    return Observation<std::vector<std::byte>>::available(std::move(result));
}

Observation<NativeOperationRecoveryEvidence> decode_native_operation_recovery_evidence(
    std::span<const std::byte> bytes, NativeOperationRecoveryEvidenceLimits limits) {
    if (!valid_limits(limits)) return invalid<NativeOperationRecoveryEvidence>(AdapterError::InvalidLimit, "native_recovery_evidence.limits");
    if (bytes.size() > limits.max_bytes) return invalid<NativeOperationRecoveryEvidence>(AdapterError::InvalidLimit, "native_recovery_evidence.size");
    Reader reader(bytes);
    std::span<const std::byte> decoded_magic;
    std::uint8_t decoded_version = 0, reserved = 0;
    if (!reader.take(magic.size(), decoded_magic) || !std::equal(magic.begin(), magic.end(), decoded_magic.begin()) ||
        !reader.u8(decoded_version) || !reader.u8(reserved) ||
        (decoded_version != published_identity_version && decoded_version != digest_version &&
         decoded_version != address_version && decoded_version != staging_identity_version &&
         decoded_version != legacy_version) || reserved != 0)
        return invalid<NativeOperationRecoveryEvidence>(AdapterError::InvalidNativeResult, "native_recovery_evidence.header");
    auto staging = read_name(reader, limits.max_name_units);
    auto destination = read_name(reader, limits.max_name_units);
    std::optional<NativeDurableIdentity> staging_identity;
    if (decoded_version >= staging_identity_version) {
        std::uint8_t present = 0;
        if (!reader.u8(present) || present > 1)
            return invalid<NativeOperationRecoveryEvidence>(AdapterError::InvalidNativeResult, "native_recovery_evidence.staging_identity_header");
        if (present != 0) {
            auto decoded = read_identity(reader);
            if (!decoded.value())
                return Observation<NativeOperationRecoveryEvidence>::unavailable(
                    decoded.state(), decoded.unavailable_detail()->error);
            staging_identity = std::move(decoded).take_value();
        }
    }
    auto source_parent = read_identity(reader);
    auto source = read_identity(reader);
    auto destination_identity = read_identity(reader);
    std::optional<NativeAbsolutePath> destination_address;
    std::optional<std::array<std::byte, 32>> content_digest;
    std::optional<NativeDurableIdentity> published_identity;
    if (decoded_version == address_version) {
        auto decoded = read_address(reader, NativePathLimits::hard_native_bytes);
        if (!decoded.value())
            return Observation<NativeOperationRecoveryEvidence>::unavailable(
                decoded.state(), decoded.unavailable_detail()->error);
        destination_address = std::move(decoded).take_value();
    } else if (decoded_version == digest_version || decoded_version == published_identity_version) {
        std::uint8_t address_present = 0;
        if (!reader.u8(address_present) || address_present > 1)
            return invalid<NativeOperationRecoveryEvidence>(AdapterError::InvalidNativeResult,
                                                             "native_recovery_evidence.address_header");
        if (address_present != 0) {
            auto decoded = read_address(reader, NativePathLimits::hard_native_bytes);
            if (!decoded.value())
                return Observation<NativeOperationRecoveryEvidence>::unavailable(
                    decoded.state(), decoded.unavailable_detail()->error);
            destination_address = std::move(decoded).take_value();
        }
        std::span<const std::byte> digest;
        if (!reader.take(32, digest))
            return invalid<NativeOperationRecoveryEvidence>(AdapterError::InvalidNativeResult,
                                                             "native_recovery_evidence.digest");
        std::array<std::byte, 32> value{};
        std::copy(digest.begin(), digest.end(), value.begin());
        content_digest = value;
        if (decoded_version == published_identity_version) {
            std::uint8_t present = 0;
            if (!reader.u8(present) || present != 1)
                return invalid<NativeOperationRecoveryEvidence>(AdapterError::InvalidNativeResult,
                                                                 "native_recovery_evidence.published_identity_header");
            auto decoded = read_identity(reader);
            if (!decoded.value())
                return Observation<NativeOperationRecoveryEvidence>::unavailable(
                    decoded.state(), decoded.unavailable_detail()->error);
            published_identity = std::move(decoded).take_value();
        }
    }
    if (!staging.value() || !destination.value() || !source_parent.value() || !source.value() ||
        !destination_identity.value() || !reader.empty())
        return invalid<NativeOperationRecoveryEvidence>(AdapterError::InvalidNativeResult, "native_recovery_evidence.trailing");
    return Observation<NativeOperationRecoveryEvidence>::available({
        std::move(staging).take_value(), std::move(destination).take_value(),
        std::move(source_parent).take_value(), std::move(source).take_value(),
        std::move(destination_identity).take_value(), std::move(staging_identity),
        std::move(destination_address), std::move(content_digest), std::move(published_identity)});
}
} // namespace ck::files
