// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_record.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>
#include <tuple>
namespace ck::files {
namespace {
using Bytes = std::vector<std::byte>;
constexpr std::array magic{std::byte{'C'}, std::byte{'K'}, std::byte{'F'}, std::byte{'I'},
                           std::byte{'D'}, std::byte{'X'}, std::byte{'0'}, std::byte{'1'}};
constexpr std::array states{ObservationState::Available, ObservationState::Unsupported, ObservationState::Unknown,
                            ObservationState::Denied,    ObservationState::Gone,        ObservationState::Changed,
                            ObservationState::Failed};
constexpr std::array domains{ErrorDomain::None, ErrorDomain::Posix, ErrorDomain::Win32, ErrorDomain::Adapter,
                             ErrorDomain::NtStatus};
constexpr std::array kinds{ObjectKind::RegularFile, ObjectKind::Directory,       ObjectKind::SymbolicLink,
                           ObjectKind::Junction,    ObjectKind::ReparsePoint,    ObjectKind::Fifo,
                           ObjectKind::Socket,      ObjectKind::CharacterDevice, ObjectKind::BlockDevice,
                           ObjectKind::Other};
constexpr std::array schemes{IdentityScheme::Unknown, IdentityScheme::PosixDeviceInode,
                             IdentityScheme::WindowsFileId128, IdentityScheme::WindowsFileIndex64};
struct Rejected {
    UnavailableObservation detail;
};
[[noreturn]] void reject(ObservationState state, AdapterError code, const char *operation) {
    throw Rejected{{state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}}};
}
[[noreturn]] void invalid() {
    reject(ObservationState::Failed, AdapterError::InvalidNativeResult, "index_record.malformed");
}
[[noreturn]] void limit() {
    reject(ObservationState::Unknown, AdapterError::InventoryLimit, "index_record.budget");
}
void validate(const DirectoryIndexContext &context, DirectoryIndexRecordLimits limits) {
    if (limits.record_bytes < 16 || limits.record_bytes > DirectoryIndexRecordLimits::hard_record_bytes ||
        !limits.name_bytes || limits.name_bytes > DirectoryIndexRecordLimits::hard_name_bytes ||
        limits.diagnostic_bytes > DirectoryIndexRecordLimits::hard_diagnostic_bytes)
        reject(ObservationState::Failed, AdapterError::InvalidLimit, "index_record.limits");
    if (!context.source_scope.is_valid() ||
        std::all_of(context.generation.begin(), context.generation.end(), [](std::byte b) { return b == std::byte{}; }))
        reject(ObservationState::Failed, AdapterError::InvalidIdentifier, "index_record.context");
}
template <class T, std::size_t N> std::uint8_t tag(T value, const std::array<T, N> &values) {
    const auto i = std::find(values.begin(), values.end(), value);
    if (i == values.end())
        invalid();
    return static_cast<std::uint8_t>(i - values.begin());
}
struct Writer {
    const DirectoryIndexContext &context;
    DirectoryIndexRecordLimits limits;
    Bytes bytes;
    std::size_t diagnostics = 0;
    void append(std::span<const std::byte> data) {
        if (data.size() > limits.record_bytes - 4 - bytes.size())
            limit();
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void integer(std::uint64_t value, std::size_t width) {
        std::array<std::byte, 8> encoded{};
        for (std::size_t i = 0; i < width; ++i)
            encoded[i] = static_cast<std::byte>((value >> (i * 8)) & 255U);
        append(std::span(encoded).first(width));
    }
    void diagnostic(const std::string &text) {
        if (text.size() > limits.diagnostic_bytes - diagnostics)
            limit();
        diagnostics += text.size();
        integer(text.size(), 4);
        append(std::as_bytes(std::span(text.data(), text.size())));
    }
};
struct Reader {
    const DirectoryIndexContext &context;
    DirectoryIndexRecordLimits limits;
    std::span<const std::byte> bytes;
    std::size_t offset = 0, diagnostics = 0;
    std::span<const std::byte> take(std::size_t count) {
        if (count > bytes.size() - offset)
            invalid();
        const auto result = bytes.subspan(offset, count);
        offset += count;
        return result;
    }
    std::uint64_t integer(std::size_t width) {
        const auto data = take(width);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i)
            value |= std::to_integer<std::uint64_t>(data[i]) << (i * 8);
        return value;
    }
    template <class T, std::size_t N> T enumeration(const std::array<T, N> &values) {
        const auto code = integer(1);
        if (code >= N)
            invalid();
        return values[static_cast<std::size_t>(code)];
    }
    std::string diagnostic() {
        const auto count = integer(4);
        if (count > limits.diagnostic_bytes - diagnostics)
            limit();
        diagnostics += static_cast<std::size_t>(count);
        const auto data = take(static_cast<std::size_t>(count));
        return {reinterpret_cast<const char *>(data.data()), data.size()};
    }
};
void put(Writer &w, std::uint64_t value) {
    w.integer(value, 8);
}
void put(Writer &w, std::uint32_t value) {
    w.integer(value, 4);
}
void put(Writer &w, bool value) {
    w.integer(value ? 1 : 0, 1);
}
void put(Writer &w, ObjectKind value) {
    w.integer(tag(value, kinds), 1);
}
void put(Writer &w, const LinuxFileAttributes &value) {
    put(w, value.value);
    put(w, value.known_mask);
}
void put(Writer &w, const FileTimestamp &value) {
    w.integer(std::bit_cast<std::uint64_t>(value.seconds()), 8);
    put(w, value.nanoseconds());
}
void put(Writer &w, const ObjectIdentity &value) {
    if (value.scope != w.context.source_scope)
        reject(ObservationState::Failed, AdapterError::UnqualifiedIdentity, "index_record.identity_scope");
    w.integer(tag(value.scheme, schemes), 1);
    w.append(value.volume.bytes);
    w.append(value.object.bytes);
    put(w, value.generation.has_value());
    if (value.generation)
        put(w, *value.generation);
}
void put(Writer &w, const BasicMetadata &m);
BasicMetadata metadata(Reader &r);
template <class T> void observed(Writer &w, const Observation<T> &value) {
    w.integer(tag(value.state(), states), 1);
    if (value.value()) {
        put(w, *value.value());
        return;
    }
    const auto &e = value.unavailable_detail()->error;
    w.integer(tag(e.domain, domains), 1);
    w.integer(std::bit_cast<std::uint64_t>(e.code), 8);
    w.diagnostic(e.operation);
}
template <class T> T get(Reader &r) {
    if constexpr (std::is_same_v<T, std::uint64_t>)
        return r.integer(8);
    else if constexpr (std::is_same_v<T, std::uint32_t>)
        return static_cast<std::uint32_t>(r.integer(4));
    else if constexpr (std::is_same_v<T, bool>) {
        const auto value = r.integer(1);
        if (value > 1)
            invalid();
        return value != 0;
    } else if constexpr (std::is_same_v<T, ObjectKind>)
        return r.enumeration(kinds);
    else if constexpr (std::is_same_v<T, LinuxFileAttributes>) {
        const auto value = r.integer(8), mask = r.integer(8);
        return {value, mask};
    } else if constexpr (std::is_same_v<T, FileTimestamp>) {
        const auto seconds = std::bit_cast<std::int64_t>(r.integer(8));
        const auto nanos = static_cast<std::uint32_t>(r.integer(4));
        const auto value = FileTimestamp::from_unix(seconds, nanos);
        if (!value)
            invalid();
        return *value;
    } else if constexpr (std::is_same_v<T, ObjectIdentity>) {
        ObjectIdentity value{r.context.source_scope, r.enumeration(schemes), {}, {}, {}};
        const auto volume = r.take(16);
        std::copy(volume.begin(), volume.end(), value.volume.bytes.begin());
        const auto object = r.take(16);
        std::copy(object.begin(), object.end(), value.object.bytes.begin());
        if (get<bool>(r)) {
            value.generation = r.integer(8);
        }
        return value;
    } else if constexpr (std::is_same_v<T, BasicMetadata>)
        return metadata(r);
    else
        static_assert(!sizeof(T), "unsupported directory-index field");
}
template <class T> Observation<T> observed(Reader &r) {
    const auto state = r.enumeration(states);
    if (state == ObservationState::Available)
        return Observation<T>::available(get<T>(r));
    const auto domain = r.enumeration(domains);
    const auto code = std::bit_cast<std::int64_t>(r.integer(8));
    auto operation = r.diagnostic();
    return Observation<T>::unavailable(state, {domain, code, std::move(operation)});
}
// Binding the complete aggregate makes adding a metadata field a compile-time
// schema decision instead of silently omitting it from stored records.
template <class M> auto metadata_fields(M &m) {
    auto &[identity, kind, logical_size, allocated_size, link_count, uid, gid, permissions, bsd_flags,
           windows_attributes, reparse_tag, linux_attributes, mount_id, accessed, modified, changed, created] = m;
    return std::tie(identity, kind, logical_size, allocated_size, link_count, uid, gid, permissions, bsd_flags,
                    windows_attributes, reparse_tag, linux_attributes, mount_id, accessed, modified, changed, created);
}
void put(Writer &w, const BasicMetadata &m) {
    std::apply([&](const auto &...fields) { (observed(w, fields), ...); }, metadata_fields(m));
}
template <class T> void load(Reader &r, Observation<T> &field) {
    field = observed<T>(r);
}
BasicMetadata metadata(Reader &r) {
    BasicMetadata m;
    std::apply([&](auto &...fields) { (load(r, fields), ...); }, metadata_fields(m));
    return m;
}
} // namespace
std::uint32_t detail::directory_index_crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto value : bytes) {
        crc ^= std::to_integer<std::uint32_t>(value);
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0x82f63b78U : 0U);
    }
    return ~crc;
}
Observation<std::size_t> detail::directory_index_record_wire_size(std::span<const std::byte> header,
                                                                  DirectoryIndexRecordLimits limits) {
    using Result = Observation<std::size_t>;
    try {
        if (limits.record_bytes < 16 || limits.record_bytes > limits.hard_record_bytes)
            reject(ObservationState::Failed, AdapterError::InvalidLimit, "index_record.limits");
        if (header.size() != 12)
            invalid();
        if (!std::equal(magic.begin(), magic.end(), header.begin()))
            reject(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "index_record.schema");
        std::uint64_t length = 0;
        for (std::size_t i = 0; i != 4; ++i)
            length |= std::to_integer<std::uint64_t>(header[8 + i]) << (i * 8);
        if (length + 16 > limits.record_bytes)
            limit();
        return Result::available(static_cast<std::size_t>(length + 16));
    } catch (const Rejected &e) {
        return Result::unavailable(e.detail.state, e.detail.error);
    }
}
Observation<std::vector<std::byte>> encode_directory_index_record(const DirectoryIndexRecord &record,
                                                                  const DirectoryIndexContext &context,
                                                                  DirectoryIndexRecordLimits limits) {
    using Result = Observation<Bytes>;
    try {
        validate(context, limits);
        if (!record.row_id || !record.name.is_valid())
            invalid();
        Writer w{context, limits, {}, 0};
        w.append(magic);
        w.integer(0, 4);
        w.append(context.generation);
        w.integer(record.row_id, 8);
        const auto *bytes = record.name.posix_bytes();
        const auto *units = record.name.windows_units();
        const auto count = bytes ? bytes->size() : units->size();
        const auto width = bytes ? std::size_t{1} : sizeof(char16_t);
        if (count > limits.name_bytes / width)
            limit();
        w.integer(bytes ? 0 : 1, 1);
        w.integer(count * width, 4);
        if (bytes)
            w.append(std::as_bytes(std::span(bytes->data(), bytes->size())));
        else
            for (const auto unit : *units)
                w.integer(unit, 2);
        observed(w, record.metadata);
        observed(w, record.hidden);
        const auto length = w.bytes.size() - 12;
        for (std::size_t i = 0; i < 4; ++i)
            w.bytes[8 + i] = static_cast<std::byte>((length >> (i * 8)) & 255U);
        const auto checksum = detail::directory_index_crc32c(w.bytes);
        // Writer reserves these four bytes throughout the payload construction.
        for (std::size_t i = 0; i < 4; ++i)
            w.bytes.push_back(static_cast<std::byte>((checksum >> (i * 8)) & 255U));
        return Result::available(std::move(w.bytes));
    } catch (const Rejected &e) {
        return Result::unavailable(e.detail.state, e.detail.error);
    }
}
Observation<DirectoryIndexRecord> decode_directory_index_record(std::span<const std::byte> wire,
                                                                const DirectoryIndexContext &context,
                                                                DirectoryIndexRecordLimits limits) {
    using Result = Observation<DirectoryIndexRecord>;
    try {
        validate(context, limits);
        if (wire.size() > limits.record_bytes)
            limit();
        if (wire.size() < 16)
            invalid();
        if (!std::equal(magic.begin(), magic.end(), wire.begin()))
            reject(ObservationState::Unsupported, AdapterError::UnsupportedQuery, "index_record.schema");
        Reader envelope{context, limits, wire};
        envelope.take(8);
        const auto length = envelope.integer(4);
        if (length != wire.size() - 16)
            invalid();
        const auto expected = detail::directory_index_crc32c(wire.first(wire.size() - 4));
        Reader tail{context, limits, wire.last(4)};
        if (tail.integer(4) != expected)
            reject(ObservationState::Failed, AdapterError::InvalidNativeResult, "index_record.checksum");
        Reader r{context, limits, wire.subspan(12, static_cast<std::size_t>(length))};
        const auto generation = r.take(16);
        if (!std::equal(generation.begin(), generation.end(), context.generation.begin()))
            reject(ObservationState::Changed, AdapterError::MetadataChanged, "index_record.generation");
        const auto row = r.integer(8);
        if (!row)
            invalid();
        const auto encoding = r.integer(1), name_bytes = r.integer(4);
        if (name_bytes > limits.name_bytes)
            limit();
        if (encoding > 1 || (encoding == 1 && name_bytes % 2))
            invalid();
        const auto data = r.take(static_cast<std::size_t>(name_bytes));
        std::optional<NativeName> name;
        if (encoding == 0)
            name = NativeName::from_posix(std::string(reinterpret_cast<const char *>(data.data()), data.size()));
        else {
            std::u16string units;
            units.reserve(data.size() / 2);
            for (std::size_t i = 0; i < data.size(); i += 2)
                units.push_back(static_cast<char16_t>(std::to_integer<std::uint16_t>(data[i]) |
                                                      (std::to_integer<std::uint16_t>(data[i + 1]) << 8)));
            name = NativeName::from_windows(std::move(units));
        }
        if (!name)
            invalid();
        auto value = observed<BasicMetadata>(r);
        auto hidden = observed<bool>(r);
        if (r.offset != r.bytes.size())
            invalid();
        return Result::available({row, std::move(*name), std::move(value), std::move(hidden)});
    } catch (const Rejected &e) {
        return Result::unavailable(e.detail.state, e.detail.error);
    }
}
} // namespace ck::files
