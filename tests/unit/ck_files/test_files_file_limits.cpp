// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_limits.hpp"
#include "opened_entry.hpp"
#include "cvision/testing/cktest.hpp"
#include <limits>
namespace {
using namespace ck::files;
BasicMetadata metadata(ObjectKind kind = ObjectKind::RegularFile) {
    BasicMetadata m; m.kind = Observation<ObjectKind>::available(kind);
    m.identity = Observation<ObjectIdentity>::available({AdapterScope{}, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}});
    m.link_count = Observation<std::uint64_t>::available(3); return m;
}
LocationInfo location() {
    LocationInfo result; result.family = LocationQueryFamily::LinuxDescriptor;
    result.directory = metadata(ObjectKind::Directory).identity;
    result.posix_file_size_bits = Observation<std::uint64_t>::available(64);
    result.posix_symlink_max = Observation<std::uint64_t>::available(4095); return result;
}
class EmptyEntry final : public OpenedEntry {
    Observation<BasicMetadata> inspect_basic() const override { return {}; }
};
}
CK_TEST(file_limits_signed_width_comparison_handles_exact_boundaries_without_shifting) {
    auto info = location();
    for (const auto bits : {32U, 48U, 63U, 64U}) {
        info.posix_file_size_bits = Observation<std::uint64_t>::available(bits);
        const auto maximum = (std::uint64_t{1} << (bits - 1)) - 1;
        for (const auto bytes : {std::uint64_t{0}, maximum, maximum + 1, std::numeric_limits<std::uint64_t>::max()}) {
            const auto result = assess_file_size_bits(bytes, info); CK_CHECK(result.value());
            CK_CHECK(result.value()->exceeds_reported_width() == (bytes > maximum));
            CK_CHECK(result.value()->proposed_size == bytes && result.value()->reported_signed_bits == bits);
        }
    }
    for (const auto bits : {std::uint64_t{65}, std::numeric_limits<std::uint64_t>::max()}) {
        info.posix_file_size_bits = Observation<std::uint64_t>::available(bits);
        const auto result = assess_file_size_bits(std::numeric_limits<std::uint64_t>::max(), info);
        CK_CHECK(result.value() && result.value()->required_signed_bits == 65 && !result.value()->exceeds_reported_width());
    }
    for (std::uint64_t bits = 0; bits < 32; ++bits) {
        info.posix_file_size_bits = Observation<std::uint64_t>::available(bits);
        CK_CHECK(assess_file_size_bits(0, info).state() == ObservationState::Failed);
    }
}
CK_TEST(file_limits_symlink_byte_count_has_no_unicode_path_or_target_validity_conversion) {
    auto info = location();
    for (const auto bytes : {std::uint64_t{0}, std::uint64_t{4095}, std::uint64_t{4096}, std::numeric_limits<std::uint64_t>::max()}) {
        const auto result = assess_posix_symlink_length(bytes, info); CK_CHECK(result.value());
        CK_CHECK(result.value()->payload_bytes == bytes && result.value()->exceeds_reported_maximum() == (bytes > 4095));
    }
    info.posix_symlink_max = Observation<std::uint64_t>::available(0);
    CK_CHECK(assess_posix_symlink_length(0, info).state() == ObservationState::Failed);
}
CK_TEST(file_limits_keep_family_and_each_unavailable_bound_explicit) {
    auto info = location();
    for (const auto family : {LocationQueryFamily::Unknown, LocationQueryFamily::WindowsHandle, static_cast<LocationQueryFamily>(255)}) {
        info.family = family; const auto expected = family == LocationQueryFamily::Unknown ? ObservationState::Unknown :
            family == LocationQueryFamily::WindowsHandle ? ObservationState::Unsupported : ObservationState::Failed;
        CK_CHECK(assess_file_size_bits(1, info).state() == expected && assess_posix_symlink_length(1, info).state() == expected);
    }
    for (const auto family : {LocationQueryFamily::LinuxDescriptor, LocationQueryFamily::DarwinDescriptor})
        for (const auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
            const NativeError error{ErrorDomain::Posix, 22, "injected.limit"}; info.family = family;
            info.posix_file_size_bits = info.posix_symlink_max = Observation<std::uint64_t>::unavailable(state, error);
            const auto size = assess_file_size_bits(1, info);
            const auto link = assess_posix_symlink_length(1, info);
            CK_CHECK(size.state() == state && size.unavailable_detail()->error == error && link.state() == state);
            CK_CHECK(assess_posix_symlink_length(1, info).unavailable_detail()->error == error);
        }
}
CK_TEST(file_limits_pathconf_decoder_never_turns_indeterminate_or_error_into_infinity) {
    const NativeError no_error{ErrorDomain::Posix, 0, "fpathconf.test"};
    const auto unlimited = detail::decode_posix_limit(-1, ObservationState::Failed, no_error);
    CK_CHECK(unlimited.state() == ObservationState::Unknown && unlimited.unavailable_detail()->error == no_error);
    for (const auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        const NativeError error{ErrorDomain::Posix, 13, "fpathconf.test"};
        const auto result = detail::decode_posix_limit(-1, state, error);
        CK_CHECK(result.state() == state && result.unavailable_detail()->error == error);
    }
    CK_CHECK(detail::decode_posix_limit(0, ObservationState::Failed, no_error).state() == ObservationState::Unknown);
    CK_CHECK(detail::decode_posix_limit(-2, ObservationState::Denied, no_error).state() == ObservationState::Failed);
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    CK_CHECK(*detail::decode_posix_limit(maximum, ObservationState::Denied, no_error).value() == static_cast<std::uint64_t>(maximum));
}
CK_TEST(file_limits_link_reports_concern_the_exact_subject_and_proposed_total_only) {
    const auto m = metadata();
    for (const auto kind : {ObjectKind::RegularFile, ObjectKind::Directory, ObjectKind::SymbolicLink}) {
        FileLinkLimits report{FileLinkLimitFamily::PosixFpathconf, *m.identity.value(), kind, {}, {}, Observation<std::uint64_t>::available(127)};
        for (const auto count : {std::uint64_t{0}, std::uint64_t{127}, std::uint64_t{128}, std::numeric_limits<std::uint64_t>::max()}) {
            const auto result = assess_hard_link_total(count, report); CK_CHECK(result.value());
            CK_CHECK(result.value()->kind == kind && result.value()->proposed_total == count && result.value()->exceeds_reported_maximum() == (count > 127));
        }
        report.maximum_links = Observation<std::uint64_t>::unavailable(ObservationState::Denied, {ErrorDomain::Posix, 13, "denied"});
        CK_CHECK(assess_hard_link_total(1, report).state() == ObservationState::Denied);
    }
    EmptyEntry entry; CK_CHECK(entry.inspect_link_limits().state() == ObservationState::Unsupported);
}
CK_TEST(file_limits_link_qualification_rejects_changed_or_unqualified_subjects_but_retains_racing_counts) {
    const auto before = metadata(); auto after = before; after.link_count = Observation<std::uint64_t>::available(100);
    const auto observe = [](BasicMetadata value) { return Observation<BasicMetadata>::available(std::move(value)); };
    CK_CHECK(detail::qualify_file_link_subject(observe(before), observe(after)).value());
    for (int mode = 0; mode < 7; ++mode) {
        after = before; auto id = *after.identity.value();
        if (mode == 0) id.object = NativeIdentifier::from_u64(99);
        if (mode == 1) id.volume = NativeIdentifier::from_u64(99);
        if (mode == 2) id.scope = AdapterScope{};
        if (mode == 3) id.object = {};
        if (mode == 4) id.scheme = IdentityScheme::WindowsFileId128;
        after.identity = Observation<ObjectIdentity>::available(id);
        if (mode == 5) after.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
        if (mode == 6) after.kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
        CK_CHECK(detail::qualify_file_link_subject(observe(before), observe(after)).state() ==
            (mode <= 1 || mode == 5 ? ObservationState::Changed : mode == 6 ? ObservationState::Failed : ObservationState::Unknown));
    }
}

CK_TEST(file_limits_subject_qualification_preserves_basic_kind_and_identity_failures) {
    const auto m = metadata();
    for (const auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed})
        for (bool second : {false, true}) for (int field = 0; field < 3; ++field) {
            const NativeError error{ErrorDomain::Posix, 13, "injected.subject"};
            auto a = Observation<BasicMetadata>::available(m), b = a; auto changed = m;
            if (field == 0) { if (second) b = Observation<BasicMetadata>::unavailable(state, error); else a = Observation<BasicMetadata>::unavailable(state, error); }
            if (field == 1) changed.kind = Observation<ObjectKind>::unavailable(state, error);
            if (field == 2) changed.identity = Observation<ObjectIdentity>::unavailable(state, error);
            if (field != 0) { if (second) b = Observation<BasicMetadata>::available(changed); else a = Observation<BasicMetadata>::available(changed); }
            const auto result = detail::qualify_file_link_subject(a, b);
            CK_CHECK(result.state() == state && result.unavailable_detail()->error == error);
        }
}
