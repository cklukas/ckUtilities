// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_cursor.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace {
using namespace ck::files;
class EmptyCursor final : public DirectoryCursor {
    Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
};
FscryptPolicyData policy(unsigned version, unsigned marker = 0xa0) {
    std::array<std::byte, 24> bytes{}; bytes[0] = static_cast<std::byte>(version);
    for (std::size_t i = version == 0 ? 4 : 8; i < bytes.size(); ++i) bytes[i] = static_cast<std::byte>(marker + i);
    auto result = FscryptPolicyData::decode(std::span(bytes).first(version == 0 ? 12U : 24U));
    if (!result.value()) throw std::runtime_error("invalid key-status policy fixture");
    return std::move(result).take_value();
}
ObjectIdentity identity() { return {AdapterScope{}, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(7), NativeIdentifier::from_u64(8), {}}; }
auto report(unsigned version = 2) { return DirectoryFscryptKeyStatus{identity(), policy(version), {}}; }
auto observed(const DirectoryFscryptKeyStatus& value) {
    return Observation<DirectoryFscryptPolicy>::available({value.directory, value.policy});
}
}
CK_TEST(fscrypt_key_default_is_unsupported_and_does_not_imply_a_missing_key) {
    EmptyCursor cursor; const auto result = cursor.inspect_fscrypt_key_status();
    CK_CHECK(result.state() == ObservationState::Unsupported && result.unavailable_detail()->error.operation == "directory.fscrypt_key_status");
}
CK_TEST(fscrypt_key_states_report_only_filesystem_keyring_codes_and_retain_unknown_raw_values) {
    for (const auto version : {0U, 2U}) {
        auto value = report(version);
        for (std::uint32_t status = 1; status <= 3; ++status) {
            value.key.status = status; const auto state = fscrypt_filesystem_key_state(value);
            CK_CHECK(state.value() && static_cast<unsigned>(*state.value()) == status - 1);
        }
        for (const auto status : {0U, 4U, std::numeric_limits<std::uint32_t>::max()}) {
            value.key.status = status;
            CK_CHECK(fscrypt_filesystem_key_state(value).state() == ObservationState::Unsupported && value.key.status == status);
            CK_CHECK(fscrypt_key_added_by_self(value).state() == ObservationState::Unsupported &&
                fscrypt_key_user_count(value).state() == ObservationState::Unsupported);
        }
    }
}
CK_TEST(fscrypt_key_v2_flags_and_counts_preserve_all_native_bits_and_zero_values) {
    auto value = report(); value.key.status = 2;
    for (const auto flags : {0U, 1U, 0x80000000U, 0xffffffffU}) for (const auto count : {0U, 1U, 0xffffffffU}) {
        value.key.flags = flags; value.key.user_count = count;
        CK_CHECK(fscrypt_key_added_by_self(value).value() && *fscrypt_key_added_by_self(value).value() == ((flags & 1U) != 0));
        CK_CHECK(fscrypt_key_user_count(value).value() && *fscrypt_key_user_count(value).value() == count);
        CK_CHECK(value.key.flags == flags && value.key.user_count == count);
    }
    for (std::size_t i = 0; i < value.key.reserved.size(); ++i) value.key.reserved[i] = static_cast<std::uint32_t>(0xdead0000U + i);
    const auto copy = value;
    CK_CHECK(copy.key.reserved == value.key.reserved && copy.key.reserved.back() == 0xdead000cU);
}
CK_TEST(fscrypt_key_v1_user_fields_remain_uninterpreted_without_a_legacy_keyring_claim) {
    auto value = report(0); value.key.status = 1; value.key.flags = 1; value.key.user_count = 42;
    CK_CHECK(fscrypt_key_added_by_self(value).state() == ObservationState::Unsupported);
    CK_CHECK(fscrypt_key_user_count(value).state() == ObservationState::Unsupported);
    CK_CHECK(*fscrypt_filesystem_key_state(value).value() == FscryptFilesystemKeyState::Absent);
    CK_CHECK(value.key.flags == 1 && value.key.user_count == 42);
}
CK_TEST(fscrypt_key_qualification_requires_unchanged_policy_bytes_and_present_policy) {
    const auto value = report(); const auto before = observed(value);
    CK_CHECK(detail::qualify_fscrypt_key_policy(before, before).value());
    for (std::size_t byte = 0; byte < value.policy.raw().size(); ++byte) {
        auto after = value;
        if (byte == 0) after.policy = policy(0);
        else { auto raw = std::vector<std::byte>(value.policy.raw().begin(), value.policy.raw().end()); raw[byte] ^= std::byte{1}; after.policy = std::move(FscryptPolicyData::decode(raw)).take_value(); }
        CK_CHECK(detail::qualify_fscrypt_key_policy(before, observed(after)).state() == ObservationState::Changed);
    }
    auto absent = Observation<DirectoryFscryptPolicy>::available({value.directory, FscryptPolicyAbsent{{ErrorDomain::Posix, 61, "policy.absent"}}});
    CK_CHECK(detail::qualify_fscrypt_key_policy(before, absent).state() == ObservationState::Changed);
    CK_CHECK(detail::qualify_fscrypt_key_policy(absent, absent).state() == ObservationState::Unsupported);
}
CK_TEST(fscrypt_key_qualification_rejects_changed_unqualified_and_foreign_directory_ids) {
    const auto value = report();
    for (int mode = 0; mode < 7; ++mode) {
        auto after = value;
        if (mode == 0) after.directory.object = NativeIdentifier::from_u64(9);
        if (mode == 1) after.directory.volume = NativeIdentifier::from_u64(9);
        if (mode == 2) after.directory.scope = AdapterScope{};
        if (mode == 3) after.directory.object = {};
        if (mode == 4) after.directory.generation = 1;
        if (mode == 5) after.directory.scheme = IdentityScheme::WindowsFileId128;
        if (mode == 6) after.directory.scheme = IdentityScheme::Unknown;
        CK_CHECK(detail::qualify_fscrypt_key_policy(observed(value), observed(after)).state() ==
            (mode <= 1 ? ObservationState::Changed : ObservationState::Unknown));
    }
}
CK_TEST(fscrypt_key_qualification_preserves_each_unavailable_native_policy_state) {
    const auto before = observed(report());
    for (const auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed})
        for (const bool second : {false, true}) {
            const NativeError error{ErrorDomain::Posix, 13, "injected.key_policy"};
            const auto failed = Observation<DirectoryFscryptPolicy>::unavailable(state, error);
            const auto result = detail::qualify_fscrypt_key_policy(second ? before : failed, second ? failed : before);
            CK_CHECK(result.state() == state && result.unavailable_detail()->error == error);
        }
}
