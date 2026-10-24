// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_index_record.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
namespace {
using namespace ck::files;
using Bytes = std::vector<std::byte>;
template <class T> T take(Observation<T> value) {
    if (!value.value())
        throw std::runtime_error("index record fixture unavailable: " + value.unavailable_detail()->error.operation);
    return std::move(value).take_value();
}
DirectoryIndexContext context() {
    DirectoryIndexContext c;
    for (std::size_t i = 0; i < c.generation.size(); ++i)
        c.generation[i] = static_cast<std::byte>(i + 1);
    return c;
}
DirectoryIndexRecord simple() {
    return {42,
            *NativeName::from_posix("file"),
            Observation<BasicMetadata>::unavailable(ObservationState::Denied, {ErrorDomain::Posix, 13, "open"}),
            {}};
}
void number(Bytes &bytes, std::size_t offset, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8)) & 255U);
}
void checksum(Bytes &bytes) {
    number(bytes, bytes.size() - 4, detail::directory_index_crc32c(std::span(bytes).first(bytes.size() - 4)), 4);
}
Bytes hex(std::string_view text) {
    Bytes out;
    for (std::size_t i = 0; i < text.size(); i += 2) {
        unsigned v = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const char c = text[i + j];
            v = v * 16 + static_cast<unsigned>(c >= 'a' ? c - 'a' + 10 : c - '0');
        }
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}
BasicMetadata full(const DirectoryIndexContext &c) {
    BasicMetadata m;
    m.identity = Observation<ObjectIdentity>::available(
        {c.source_scope, IdentityScheme::WindowsFileId128, NativeIdentifier::from_u64(0x0102030405060708ULL),
         NativeIdentifier::from_u64(9), std::numeric_limits<std::uint64_t>::max()});
    m.kind = Observation<ObjectKind>::available(ObjectKind::SymbolicLink);
    m.logical_size = Observation<std::uint64_t>::available(std::numeric_limits<std::uint64_t>::max());
    m.allocated_size = Observation<std::uint64_t>::available(0);
    m.link_count = Observation<std::uint64_t>::available(123);
    m.posix_uid = Observation<std::uint64_t>::available(0xffffffffULL);
    m.posix_gid = Observation<std::uint64_t>::available(41);
    m.posix_permissions = Observation<std::uint32_t>::available(04755);
    m.bsd_flags = Observation<std::uint32_t>::available(0xffffffffU);
    m.windows_attributes = Observation<std::uint32_t>::available(0xaabbccddU);
    m.reparse_tag = Observation<std::uint32_t>::available(0xa000000cU);
    m.linux_attributes = Observation<LinuxFileAttributes>::available({0x8000000000000001ULL, 0x0102030405060708ULL});
    m.mount_id = Observation<std::uint64_t>::available(32);
    m.accessed = Observation<FileTimestamp>::available(
        *FileTimestamp::from_unix(std::numeric_limits<std::int64_t>::min(), 999999999));
    m.modified =
        Observation<FileTimestamp>::available(*FileTimestamp::from_unix(std::numeric_limits<std::int64_t>::max(), 0));
    m.changed = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(-1, 7));
    m.created = Observation<FileTimestamp>::available(
        FileTimestamp::from_windows_ticks(std::numeric_limits<std::uint64_t>::max()));
    return m;
}
constexpr std::array failures{ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
                              ObservationState::Gone,        ObservationState::Changed, ObservationState::Failed};
} // namespace
CK_TEST(index_record_crc32c_matches_rfc3720_appendix_b4_vectors) {
    std::array<std::byte, 32> data{};
    CK_CHECK(detail::directory_index_crc32c(data) == 0x8a9136aaU);
    data.fill(std::byte{0xff});
    CK_CHECK(detail::directory_index_crc32c(data) == 0x62a8ab43U);
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<std::byte>(i);
    CK_CHECK(detail::directory_index_crc32c(data) == 0x46dd794eU);
    std::reverse(data.begin(), data.end());
    CK_CHECK(detail::directory_index_crc32c(data) == 0x113fdb5cU);
    CK_CHECK(detail::directory_index_crc32c({}) == 0);
}
CK_TEST(index_record_wire_schema_has_fixed_little_endian_golden_bytes) {
    const auto c = context();
    const auto expected = hex("434b464944583031410000000102030405060708090a0b0c0d0e0f102a00000000000000000400000066696c"
                              "6503010d00000000000000040000006f70656e0200000000000000000000000000c9ce549a");
    const auto record = take(encode_directory_index_record(simple(), c));
    CK_CHECK(record == expected);
    const auto decoded = take(decode_directory_index_record(expected, c));
    CK_CHECK(decoded.row_id == 42 && decoded.name == simple().name &&
             decoded.metadata.state() == ObservationState::Denied);
    CK_CHECK(decoded.metadata.unavailable_detail()->error == simple().metadata.unavailable_detail()->error &&
             decoded.hidden.state() == ObservationState::Unknown);
}
CK_TEST(index_record_preserves_every_basic_metadata_field_and_full_numeric_ranges) {
    const auto c = context();
    DirectoryIndexRecord input{std::numeric_limits<std::uint64_t>::max(), *NativeName::from_posix("raw\xff"),
                               Observation<BasicMetadata>::available(full(c)), Observation<bool>::available(true)};
    const auto bytes = take(encode_directory_index_record(input, c));
    const auto output = take(decode_directory_index_record(bytes, c));
    const auto &m = *output.metadata.value();
    CK_CHECK(output.row_id == input.row_id && output.name == input.name && take(output.hidden));
    CK_CHECK(compare_identity(take(m.identity), take(input.metadata.value()->identity)) == IdentityComparison::Same);
    CK_CHECK(take(m.kind) == ObjectKind::SymbolicLink &&
             take(m.logical_size) == std::numeric_limits<std::uint64_t>::max() && take(m.allocated_size) == 0);
    CK_CHECK(take(m.link_count) == 123 && take(m.posix_uid) == 0xffffffffULL && take(m.posix_gid) == 41 &&
             take(m.posix_permissions) == 04755);
    CK_CHECK(take(m.bsd_flags) == 0xffffffffU && take(m.windows_attributes) == 0xaabbccddU &&
             take(m.reparse_tag) == 0xa000000cU);
    CK_CHECK(take(m.linux_attributes).value == 0x8000000000000001ULL &&
             take(m.linux_attributes).known_mask == 0x0102030405060708ULL && take(m.mount_id) == 32);
    CK_CHECK(take(m.accessed).seconds() == std::numeric_limits<std::int64_t>::min() &&
             take(m.accessed).nanoseconds() == 999999999);
    CK_CHECK(take(m.modified).seconds() == std::numeric_limits<std::int64_t>::max() &&
             take(m.changed) == *FileTimestamp::from_unix(-1, 7));
    CK_CHECK(take(m.created) == FileTimestamp::from_windows_ticks(std::numeric_limits<std::uint64_t>::max()));
    CK_CHECK(take(encode_directory_index_record(output, c)) == bytes);
}
CK_TEST(index_record_retains_all_posix_bytes_and_unpaired_utf16_units_without_display_roundtrip) {
    const auto c = context();
    for (unsigned i = 1; i <= 255; ++i) {
        if (i == '/' || i == '.')
            continue;
        auto record = simple();
        record.name = *NativeName::from_posix(std::string(1, static_cast<char>(i)));
        const auto out = take(decode_directory_index_record(take(encode_directory_index_record(record, c)), c));
        CK_CHECK(out.name == record.name);
    }
    for (unsigned i = 0xd800; i <= 0xdfff; ++i) {
        auto record = simple();
        record.name = *NativeName::from_windows(std::u16string{u'x', static_cast<char16_t>(i)});
        const auto out = take(decode_directory_index_record(take(encode_directory_index_record(record, c)), c));
        CK_CHECK(out.name == record.name);
    }
    auto record = simple();
    record.name = *NativeName::from_posix("literal\\u{D800}");
    const auto out = take(decode_directory_index_record(take(encode_directory_index_record(record, c)), c));
    CK_CHECK(out.name == record.name);
}
CK_TEST(index_record_preserves_every_failure_state_domain_code_and_diagnostic_bytes) {
    const auto c = context();
    for (const auto state : failures)
        for (const auto domain :
             {ErrorDomain::None, ErrorDomain::Posix, ErrorDomain::Win32, ErrorDomain::Adapter, ErrorDomain::NtStatus}) {
            auto record = simple();
            const NativeError error{domain, std::numeric_limits<std::int64_t>::min(), std::string("raw\0\xff", 5)};
            record.metadata = Observation<BasicMetadata>::unavailable(state, error);
            record.hidden = Observation<bool>::unavailable(state, error);
            const auto out = take(decode_directory_index_record(take(encode_directory_index_record(record, c)), c));
            CK_CHECK(out.metadata.state() == state && out.hidden.state() == state &&
                     out.metadata.unavailable_detail()->error == error &&
                     out.hidden.unavailable_detail()->error == error);
            auto m = full(c);
            m.kind = Observation<ObjectKind>::unavailable(state, error);
            m.identity = Observation<ObjectIdentity>::unavailable(state, error);
            m.logical_size = Observation<std::uint64_t>::unavailable(state, error);
            m.created = Observation<FileTimestamp>::unavailable(state, error);
            record.metadata = Observation<BasicMetadata>::available(m);
            const auto wire = take(encode_directory_index_record(record, c));
            const auto decoded = take(decode_directory_index_record(wire, c));
            CK_CHECK(take(encode_directory_index_record(decoded, c)) == wire);
        }
}
CK_TEST(index_record_rejects_truncation_single_bit_corruption_and_extra_bytes_without_partial_records) {
    const auto c = context();
    const auto wire = take(encode_directory_index_record(simple(), c));
    for (std::size_t n = 0; n < wire.size(); ++n)
        CK_CHECK(!decode_directory_index_record(std::span(wire).first(n), c).value());
    for (std::size_t i = 0; i < wire.size(); ++i)
        for (unsigned bit = 0; bit < 8; ++bit) {
            auto changed = wire;
            changed[i] ^= static_cast<std::byte>(1U << bit);
            CK_CHECK(!decode_directory_index_record(changed, c).value());
        }
    auto extra = wire;
    extra.push_back(std::byte{});
    CK_CHECK(!decode_directory_index_record(extra, c).value());
}
CK_TEST(index_record_generation_and_source_namespace_are_explicit_live_context) {
    const auto c = context();
    const auto wire = take(encode_directory_index_record(simple(), c));
    auto other = c;
    other.generation[0] ^= std::byte{1};
    CK_CHECK(decode_directory_index_record(wire, other).state() == ObservationState::Changed);
    auto wrong = c;
    wrong.generation.fill(std::byte{});
    CK_CHECK(!decode_directory_index_record(wire, wrong).value());
    auto record = simple();
    auto m = full(c);
    auto id = take(m.identity);
    id.scope = AdapterScope{};
    m.identity = Observation<ObjectIdentity>::available(id);
    record.metadata = Observation<BasicMetadata>::available(m);
    CK_CHECK(encode_directory_index_record(record, c).state() == ObservationState::Failed);
    auto moved = c;
    auto token = std::move(moved.source_scope);
    (void)token;
    CK_CHECK(!encode_directory_index_record(simple(), moved).value());
    CK_CHECK(!decode_directory_index_record(wire, moved).value());
}
CK_TEST(index_record_preserves_unqualified_identity_observations_without_making_them_qualified) {
    const auto c = context();
    for (auto scheme : {IdentityScheme::Unknown, IdentityScheme::PosixDeviceInode, IdentityScheme::WindowsFileId128,
                        IdentityScheme::WindowsFileIndex64}) {
        auto record = simple();
        auto m = full(c);
        auto id = take(m.identity);
        id.scheme = scheme;
        id.object = {};
        id.generation.reset();
        m.identity = Observation<ObjectIdentity>::available(id);
        record.metadata = Observation<BasicMetadata>::available(m);
        const auto out = take(decode_directory_index_record(take(encode_directory_index_record(record, c)), c));
        CK_CHECK(out.metadata.value()->identity.value() &&
                 compare_identity(take(out.metadata.value()->identity), take(out.metadata.value()->identity)) ==
                     IdentityComparison::Unknown);
    }
}
CK_TEST(index_record_resources_are_independent_and_exact_boundaries_are_not_truncated) {
    const auto c = context();
    const auto record = simple();
    const auto wire = take(encode_directory_index_record(record, c));
    DirectoryIndexRecordLimits limits;
    limits.record_bytes = wire.size();
    limits.name_bytes = 4;
    limits.diagnostic_bytes = 4;
    CK_CHECK(encode_directory_index_record(record, c, limits).value() &&
             decode_directory_index_record(wire, c, limits).value());
    for (int mode = 0; mode < 3; ++mode) {
        auto small = limits;
        if (mode == 0)
            --small.record_bytes;
        if (mode == 1)
            --small.name_bytes;
        if (mode == 2)
            --small.diagnostic_bytes;
        CK_CHECK(encode_directory_index_record(record, c, small).state() == ObservationState::Unknown &&
                 decode_directory_index_record(wire, c, small).state() == ObservationState::Unknown);
    }
    for (int mode = 0; mode < 5; ++mode) {
        auto bad = limits;
        if (mode == 0)
            bad.record_bytes = 15;
        if (mode == 1)
            bad.record_bytes = DirectoryIndexRecordLimits::hard_record_bytes + 1;
        if (mode == 2)
            bad.name_bytes = 0;
        if (mode == 3)
            bad.name_bytes = DirectoryIndexRecordLimits::hard_name_bytes + 1;
        if (mode == 4)
            bad.diagnostic_bytes = DirectoryIndexRecordLimits::hard_diagnostic_bytes + 1;
        CK_CHECK(encode_directory_index_record(record, c, bad).state() == ObservationState::Failed &&
                 decode_directory_index_record(wire, c, bad).state() == ObservationState::Failed);
    }
    auto windows = record;
    windows.name = *NativeName::from_windows(u"ab");
    limits.record_bytes = 256;
    limits.diagnostic_bytes = 16;
    limits.name_bytes = 3;
    CK_CHECK(encode_directory_index_record(windows, c, limits).state() == ObservationState::Unknown);
}
CK_TEST(index_record_checks_structural_fields_even_when_corruption_has_a_valid_checksum) {
    const auto c = context();
    const auto wire = take(encode_directory_index_record(simple(), c));
    for (int mode = 0; mode < 10; ++mode) {
        auto changed = wire;
        if (mode == 0)
            number(changed, 28, 0, 8); // row id
        if (mode == 1)
            changed[36] = std::byte{2}; // native encoding
        if (mode == 2)
            number(changed, 37, 0xffffffffU, 4); // declared name length
        if (mode == 3)
            changed[41] = std::byte{'/'};
        if (mode == 4)
            changed[41] = std::byte{};
        if (mode == 5)
            changed[45] = std::byte{255}; // outer observation state
        if (mode == 6)
            changed[46] = std::byte{255}; // error domain
        if (mode == 7)
            number(changed, 55, 0xffffffffU, 4); // operation length
        if (mode == 8) {
            changed[36] = std::byte{1};
            number(changed, 37, 3, 4);
        } // odd UTF-16
        if (mode == 9) {
            changed.insert(changed.end() - 4, std::byte{});
            number(changed, 8, changed.size() - 16, 4);
        } // trailing payload
        checksum(changed);
        CK_CHECK(!decode_directory_index_record(changed, c).value());
    }
}
CK_TEST(index_record_rejects_invalid_available_values_and_moved_native_names) {
    const auto c = context();
    auto record = simple();
    record.row_id = 0;
    CK_CHECK(!encode_directory_index_record(record, c).value());
    record = simple();
    auto name = std::move(record.name);
    (void)name;
    CK_CHECK(static_cast<bool>(encode_directory_index_record(record, c).value()) == record.name.is_valid());
    for (bool kind : {false, true}) {
        record = simple();
        auto m = full(c);
        if (kind)
            m.kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
        else {
            auto id = take(m.identity);
            id.scheme = static_cast<IdentityScheme>(255);
            m.identity = Observation<ObjectIdentity>::available(id);
        }
        record.metadata = Observation<BasicMetadata>::available(m);
        CK_CHECK(!encode_directory_index_record(record, c).value());
    }
}

CK_TEST(index_record_valid_checksum_does_not_bypass_scalar_enum_or_boolean_validation) {
    const auto c = context();
    DirectoryIndexRecord record{std::numeric_limits<std::uint64_t>::max(), *NativeName::from_posix("raw\xff"),
                                Observation<BasicMetadata>::available(full(c)), Observation<bool>::available(true)};
    const auto wire = take(encode_directory_index_record(record, c));
    CK_CHECK(wire.size() == 240);
    for (int mode = 0; mode < 7; ++mode) {
        auto data = wire;
        if (mode == 0)
            data[46] = std::byte{255}; // identity observation
        if (mode == 1)
            data[47] = std::byte{255}; // identity scheme
        if (mode == 2)
            data[80] = std::byte{2}; // generation present boolean
        if (mode == 3)
            data[90] = std::byte{255}; // kind
        if (mode == 4)
            number(data, 191, 1000000000U, 4); // invalid nanoseconds
        if (mode == 5)
            data[235] = std::byte{2}; // hidden value
        if (mode == 6)
            data[234] = std::byte{255}; // hidden observation
        checksum(data);
        CK_CHECK(decode_directory_index_record(data, c).state() == ObservationState::Failed);
    }
}
namespace {
template <class T> void failure_field(Observation<T> &field, int index, ObservationState state) {
    field = Observation<T>::unavailable(state, {ErrorDomain::Adapter, -1000 - index, "field." + std::to_string(index)});
}
template <class T> void check_field(const Observation<T> &field, int index, ObservationState state) {
    CK_CHECK(field.state() == state && field.unavailable_detail()->error.code == -1000 - index &&
             field.unavailable_detail()->error.operation == "field." + std::to_string(index));
}
} // namespace
CK_TEST(index_record_preserves_every_individual_metadata_failure_field_in_schema_order) {
    const auto c = context();
    for (const auto state : failures) {
        BasicMetadata m;
        failure_field(m.identity, 1, state);
        failure_field(m.kind, 2, state);
        failure_field(m.logical_size, 3, state);
        failure_field(m.allocated_size, 4, state);
        failure_field(m.link_count, 5, state);
        failure_field(m.posix_uid, 6, state);
        failure_field(m.posix_gid, 7, state);
        failure_field(m.posix_permissions, 8, state);
        failure_field(m.bsd_flags, 9, state);
        failure_field(m.windows_attributes, 10, state);
        failure_field(m.reparse_tag, 11, state);
        failure_field(m.linux_attributes, 12, state);
        failure_field(m.mount_id, 13, state);
        failure_field(m.accessed, 14, state);
        failure_field(m.modified, 15, state);
        failure_field(m.changed, 16, state);
        failure_field(m.created, 17, state);
        DirectoryIndexRecord record{1, *NativeName::from_posix("name"), Observation<BasicMetadata>::available(m), {}};
        const auto out = take(decode_directory_index_record(take(encode_directory_index_record(record, c)), c));
        const auto &result = *out.metadata.value();
        check_field(result.identity, 1, state);
        check_field(result.kind, 2, state);
        check_field(result.logical_size, 3, state);
        check_field(result.allocated_size, 4, state);
        check_field(result.link_count, 5, state);
        check_field(result.posix_uid, 6, state);
        check_field(result.posix_gid, 7, state);
        check_field(result.posix_permissions, 8, state);
        check_field(result.bsd_flags, 9, state);
        check_field(result.windows_attributes, 10, state);
        check_field(result.reparse_tag, 11, state);
        check_field(result.linux_attributes, 12, state);
        check_field(result.mount_id, 13, state);
        check_field(result.accessed, 14, state);
        check_field(result.modified, 15, state);
        check_field(result.changed, 16, state);
        check_field(result.created, 17, state);
    }
}

CK_TEST(index_record_available_metadata_matches_fixed_full_schema_wire_image) {
    const auto c = context();
    DirectoryIndexRecord record{std::numeric_limits<std::uint64_t>::max(), *NativeName::from_posix("raw\xff"),
                                Observation<BasicMetadata>::available(full(c)), Observation<bool>::available(true)};
    const auto expected =
        hex("434b464944583031e00000000102030405060708090a0b0c0d0e0f10ffffffffffffffff0004000000726177ff0000020807060504"
            "03020100000000000000000900000000000000000000000000000001ffffffffffffffff000200ffffffffffffffff000000000000"
            "000000007b0000000000000000ffffffff0000000000290000000000000000ed09000000ffffffff00ddccbbaa000c0000a0000100"
            "0000000000800807060504030201002000000000000000000000000000000080ffc99a3b00ffffffffffffff7f0000000000ffffff"
            "ffffffffff0700000000ca1a19c9aa0100009c9bee380001f970b79b");
    CK_CHECK(take(encode_directory_index_record(record, c)) == expected);
    CK_CHECK(take(decode_directory_index_record(expected, c)).name == record.name);
}
CK_TEST(index_record_frame_size_is_bounded_framing_not_record_authentication) {
    auto wire = take(encode_directory_index_record(simple(), context()));
    const auto header = std::span(wire).first(12);
    CK_CHECK(take(detail::directory_index_record_wire_size(header, {})) == wire.size());
    wire.back() ^= std::byte{1};
    CK_CHECK(take(detail::directory_index_record_wire_size(header, {})) == wire.size());
    CK_CHECK(!decode_directory_index_record(wire, context()).value());
}
CK_TEST(index_record_frame_size_rejects_foreign_schema_short_headers_and_invalid_limits) {
    auto wire = take(encode_directory_index_record(simple(), context()));
    for (std::size_t size : {std::size_t{0}, std::size_t{11}, std::size_t{13}})
        CK_CHECK(!detail::directory_index_record_wire_size(std::span(wire).first(size), {}).value());
    wire[0] ^= std::byte{1};
    CK_CHECK(detail::directory_index_record_wire_size(std::span(wire).first(12), {}).state() ==
             ObservationState::Unsupported);
    CK_CHECK(!detail::directory_index_record_wire_size(std::span(wire).first(12), {15, 1, 0}).value());
    CK_CHECK(!detail::directory_index_record_wire_size(std::span(wire).first(12),
                                                       {DirectoryIndexRecordLimits::hard_record_bytes + 1, 1, 0})
                  .value());
}
CK_TEST(index_record_frame_size_has_exact_minimum_maximum_and_overflow_boundaries) {
    auto wire = take(encode_directory_index_record(simple(), context()));
    CK_CHECK(take(detail::directory_index_record_wire_size(std::span(wire).first(12), {wire.size(), 1, 0})) ==
             wire.size());
    CK_CHECK(detail::directory_index_record_wire_size(std::span(wire).first(12), {wire.size() - 1, 1, 0}).state() ==
             ObservationState::Unknown);
    number(wire, 8, 0, 4);
    CK_CHECK(take(detail::directory_index_record_wire_size(std::span(wire).first(12), {16, 1, 0})) == 16);
    number(wire, 8, DirectoryIndexRecordLimits::hard_record_bytes - 16, 4);
    CK_CHECK(take(detail::directory_index_record_wire_size(std::span(wire).first(12),
                                                           {DirectoryIndexRecordLimits::hard_record_bytes, 1, 0})) ==
             DirectoryIndexRecordLimits::hard_record_bytes);
    number(wire, 8, UINT32_MAX, 4);
    CK_CHECK(detail::directory_index_record_wire_size(std::span(wire).first(12), {}).state() ==
             ObservationState::Unknown);
}
