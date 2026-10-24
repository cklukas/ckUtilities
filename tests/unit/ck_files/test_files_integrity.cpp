// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "opened_entry.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <stdexcept>
namespace {
using namespace ck::files;
class EmptyEntry final : public OpenedEntry {
    Observation<BasicMetadata> inspect_basic() const override { throw std::runtime_error("unexpected default integrity I/O"); }
};
BasicMetadata metadata(ObjectKind kind = ObjectKind::RegularFile) {
    BasicMetadata m; m.kind = Observation<ObjectKind>::available(kind);
    m.identity = Observation<ObjectIdentity>::available({AdapterScope{}, IdentityScheme::WindowsFileId128, NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}});
    m.windows_attributes = Observation<std::uint32_t>::available(0); return m;
}
auto qualify(const BasicMetadata& a, const BasicMetadata& b) {
    return detail::qualify_integrity_subject(Observation<BasicMetadata>::available(a), Observation<BasicMetadata>::available(b));
}
WindowsIntegrityData data(std::uint16_t algorithm, std::uint32_t flags = 0) {
    std::array<std::byte, 16> bytes{}; bytes[0] = static_cast<std::byte>(algorithm & 255U); bytes[1] = static_cast<std::byte>(algorithm >> 8);
    for (std::size_t i = 0; i < 4; ++i) bytes[4 + i] = static_cast<std::byte>((flags >> (8 * i)) & 255U);
    return std::move(WindowsIntegrityData::decode(bytes)).take_value();
}
}
CK_TEST(integrity_default_is_unsupported_not_disabled_and_invalid_limits_do_not_do_io) {
    EmptyEntry entry;
    CK_CHECK(entry.inspect_integrity().state() == ObservationState::Unsupported);
    InventoryLimits limits; limits.max_native_bytes = 0;
    CK_CHECK(entry.inspect_integrity(limits).state() == ObservationState::Failed);
}
CK_TEST(integrity_decoder_requires_the_complete_fixed_response_without_truncation) {
    std::array<std::byte, 32> bytes{};
    for (std::size_t size = 0; size <= bytes.size(); ++size)
        CK_CHECK(static_cast<bool>(WindowsIntegrityData::decode(std::span(bytes).first(size)).value()) == (size == 16));
}
CK_TEST(integrity_decoder_retains_exact_little_endian_fields_reserved_bytes_and_unknown_bits) {
    std::array<std::byte, 16> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(i + 0x80);
    const auto result = WindowsIntegrityData::decode(bytes); CK_CHECK(result.value()); const auto& decoded = *result.value();
    CK_CHECK(decoded.checksum_algorithm() == 0x8180 && decoded.reserved() == 0x8382);
    CK_CHECK(decoded.flags() == 0x87868584U && decoded.checksum_chunk_bytes() == 0x8b8a8988U && decoded.cluster_bytes() == 0x8f8e8d8cU);
    CK_CHECK(std::equal(decoded.raw().begin(), decoded.raw().end(), bytes.begin(), bytes.end()));
    CK_CHECK(reported_integrity_algorithm(decoded).state() == ObservationState::Unsupported);
}
CK_TEST(integrity_algorithm_and_off_bit_interpretation_does_not_assert_data_health_or_replay_support) {
    for (const auto code : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{2}}) for (const auto flags : {0U,1U,0x80000000U,0xffffffffU}) {
        const auto value = data(code, flags); const auto algorithm = reported_integrity_algorithm(value);
        CK_CHECK(algorithm.value() && static_cast<unsigned>(*algorithm.value()) == code);
        CK_CHECK(reported_integrity_enforcement_off(value) == ((flags & 1U) ? ReportedFlag::Set : ReportedFlag::Clear));
        CK_CHECK(value.flags() == flags);
    }
    for (const auto code : {std::uint16_t{3}, std::uint16_t{123}, std::uint16_t{65535}})
        CK_CHECK(reported_integrity_algorithm(data(code)).state() == ObservationState::Unsupported);
}
CK_TEST(integrity_subject_qualification_rejects_changed_and_unqualified_native_owners) {
    const auto before = metadata(); CK_CHECK(qualify(before,before).value());
    for (int mode = 0; mode < 8; ++mode) {
        auto after = before; auto id = *before.identity.value();
        if (mode == 0) id.object = NativeIdentifier::from_u64(99);
        if (mode == 1) id.volume = NativeIdentifier::from_u64(99);
        if (mode == 2) id.scope = AdapterScope{};
        if (mode == 3) id.object = {};
        if (mode == 4) id.scheme = IdentityScheme::PosixDeviceInode;
        if (mode == 5) id.generation = 1;
        after.identity = Observation<ObjectIdentity>::available(id);
        if (mode == 6) after.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
        if (mode == 7) after.kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
        CK_CHECK(qualify(before,after).state() == (mode < 2 || mode == 6 ? ObservationState::Changed : mode == 7 ? ObservationState::Failed : ObservationState::Unknown));
    }
    auto legacy = before; auto id = *legacy.identity.value(); id.scheme = IdentityScheme::WindowsFileIndex64;
    legacy.identity = Observation<ObjectIdentity>::available(id); CK_CHECK(qualify(legacy,legacy).value());
}
CK_TEST(integrity_subject_qualification_refuses_special_reparse_and_known_provider_cases) {
    for (const auto kind : {ObjectKind::SymbolicLink,ObjectKind::Junction,ObjectKind::ReparsePoint,ObjectKind::Fifo,ObjectKind::Socket,ObjectKind::CharacterDevice,ObjectKind::BlockDevice,ObjectKind::Other}) {
        const auto m = metadata(kind); CK_CHECK(qualify(m,m).state() == ObservationState::Unsupported);
    }
    for (const auto flag : {0x400U,0x1000U,0x400000U,0x80000U,0x100000U}) {
        auto m = metadata(); m.windows_attributes = Observation<std::uint32_t>::available(flag);
        CK_CHECK(qualify(m,m).state() == ObservationState::Unknown);
    }
}
CK_TEST(integrity_subject_qualification_detects_integrity_flag_changes_but_not_unrelated_flags) {
    auto before = metadata(), after = before; after.windows_attributes = Observation<std::uint32_t>::available(0x8000);
    CK_CHECK(qualify(before,after).state() == ObservationState::Changed);
    before.windows_attributes = after.windows_attributes; CK_CHECK(qualify(before,after).value());
    after.windows_attributes = Observation<std::uint32_t>::available(0x8000 | 0x20 | 0x2);
    CK_CHECK(qualify(before,after).value());
    const auto directory = metadata(ObjectKind::Directory); CK_CHECK(qualify(directory,directory).value());
}
CK_TEST(integrity_subject_qualification_preserves_every_unavailable_basic_field_state) {
    const auto before = metadata();
    for (const auto state : {ObservationState::Unsupported,ObservationState::Unknown,ObservationState::Denied,ObservationState::Gone,ObservationState::Changed,ObservationState::Failed})
        for (const bool second : {false,true}) for (int field = 0; field < 4; ++field) {
            const NativeError error{ErrorDomain::Win32,5,"injected.integrity"}; auto a = Observation<BasicMetadata>::available(before), b = a; auto m = before;
            if (field == 0) { if (second) b = Observation<BasicMetadata>::unavailable(state,error); else a = Observation<BasicMetadata>::unavailable(state,error); }
            if (field == 1) m.kind = Observation<ObjectKind>::unavailable(state,error);
            if (field == 2) m.identity = Observation<ObjectIdentity>::unavailable(state,error);
            if (field == 3) m.windows_attributes = Observation<std::uint32_t>::unavailable(state,error);
            if (field != 0) { if (second) b = Observation<BasicMetadata>::available(m); else a = Observation<BasicMetadata>::available(m); }
            const auto result = detail::qualify_integrity_subject(a,b);
            CK_CHECK(result.state() == state && result.unavailable_detail()->error == error);
        }
}
