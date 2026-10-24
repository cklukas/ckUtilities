// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "security.hpp"
#include "directory_cursor.hpp"
#include "cvision/testing/cktest.hpp"

namespace {
using namespace ck::files;
void put(NativeBytes& bytes, std::size_t at, std::uint32_t value, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) bytes[at + index] = static_cast<std::byte>((value >> (8U * index)) & 255U);
}
NativeBytes descriptor() {
    NativeBytes result(20);
    result[0] = std::byte{1};
    put(result, 2, 0x8000, 2);
    return result;
}
}
CK_TEST(security_access_requests_are_explicit_and_never_infer_audit_rights) {
    const auto combined = EntryAccess::MetadataAndSecurity | EntryAccess::MetadataAndAttributes;
    CK_CHECK(valid_entry_access(combined) && requests_security(combined) && requests_attributes(combined));
    CK_CHECK(!requests_audit_security(combined));
    CK_CHECK(requests_audit_security(EntryAccess::MetadataAndFullSecurity));
    CK_CHECK(!valid_entry_access(static_cast<EntryAccess>(4)));
    CK_CHECK(valid_entry_access(EntryAccess::MetadataAndLayout) && requests_layout(EntryAccess::MetadataAndLayout));
    CK_CHECK(!valid_entry_access(static_cast<EntryAccess>(32)));
    CK_CHECK(valid_entry_access(EntryAccess::MetadataAndInodeFlags) && requests_inode_flags(EntryAccess::MetadataAndInodeFlags));
    CK_CHECK(!requests_layout(EntryAccess::MetadataAndInodeFlags) && !requests_inode_flags(EntryAccess::MetadataAndLayout));
    CK_CHECK(!valid_security_part(static_cast<SecurityPart>(-1)));
    CK_CHECK(!valid_security_part(static_cast<SecurityPart>(7)));
    CK_CHECK(!valid_security_part(SecurityPart::Unknown));
    CK_CHECK(NativeSecurityValue{}.presence == SecurityPresence::Unknown);
    CK_CHECK(NativeSecurityValue{}.encoding == SecurityEncoding::Unknown);
}
CK_TEST(linux_security_structure_requires_whole_versioned_records_and_bounds) {
    NativeBytes source(4 + 8 * 3);
    put(source, 0, 2, 4);
    CK_CHECK(*detail::validate_linux_acl(source, {}).value() == 3);
    CK_CHECK(detail::validate_linux_acl(source, {1024, 2}).state() == ObservationState::Unknown);
    CK_CHECK(detail::validate_linux_acl(source, {1, 100}).state() == ObservationState::Unknown);
    for (std::size_t length = 0; length < source.size(); ++length)
        if (length < 4 || (length - 4) % 8 != 0)
            CK_CHECK(detail::validate_linux_acl(std::span(source).first(length), {}).state() == ObservationState::Failed);
    put(source, 0, 3, 4);
    CK_CHECK(detail::validate_linux_acl(source, {}).state() == ObservationState::Unsupported);
}
CK_TEST(windows_security_structure_keeps_absent_null_and_empty_dacl_distinct) {
    auto absent = descriptor();
    auto null_acl = absent; put(null_acl, 2, 0x8004, 2);
    auto empty_acl = null_acl; empty_acl.resize(28); put(empty_acl, 16, 20, 4);
    empty_acl[20] = std::byte{2}; put(empty_acl, 22, 8, 2);
    for (const auto& value : {absent, null_acl, empty_acl}) CK_CHECK(detail::validate_windows_descriptor(value, {}).value() != nullptr);
    CK_CHECK(absent != null_acl && null_acl != empty_acl);
    put(empty_acl, 2, 0x8000, 2);
    CK_CHECK(detail::validate_windows_descriptor(empty_acl, {}).state() == ObservationState::Failed);
}
CK_TEST(windows_security_structure_bounds_sids_aces_and_entry_limits_without_casts) {
    auto source = descriptor(); source.resize(44);
    put(source, 2, 0x8004, 2); put(source, 4, 20, 4); put(source, 16, 28, 4);
    source[20] = std::byte{1}; // SID with zero subauthorities is eight bytes.
    source[28] = std::byte{2}; put(source, 30, 16, 2); put(source, 32, 2, 2);
    put(source, 38, 4, 2); put(source, 42, 4, 2); // Two opaque four-byte ACE records.
    CK_CHECK(*detail::validate_windows_descriptor(source, {}).value() == 2);
    CK_CHECK(detail::validate_windows_descriptor(source, {1024, 1}).state() == ObservationState::Unknown);
    for (std::size_t length = 0; length < source.size(); ++length)
        CK_CHECK(detail::validate_windows_descriptor(std::span(source).first(length), {}).state() == ObservationState::Failed);
    for (const auto field : {std::size_t{4}, std::size_t{8}, std::size_t{12}, std::size_t{16}}) {
        auto bad = source; put(bad, field, 0xffffffff, 4);
        CK_CHECK(detail::validate_windows_descriptor(bad, {}).state() == ObservationState::Failed);
    }
    auto bad = source; bad[21] = std::byte{16};
    CK_CHECK(detail::validate_windows_descriptor(bad, {}).state() == ObservationState::Failed);
    bad = source; put(bad, 38, 0, 2);
    CK_CHECK(detail::validate_windows_descriptor(bad, {}).state() == ObservationState::Failed);
}
