// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_operation_recovery_evidence.hpp"

#include <cvision/testing/cktest.hpp>

using namespace ck::files;

namespace {
ObjectIdentity live_identity(const AdapterScope& scope, IdentityScheme scheme, std::uint64_t object,
                             std::optional<std::uint64_t> generation = {}) {
    return {scope, scheme, NativeIdentifier::from_u64(7), NativeIdentifier::from_u64(object), generation};
}
NativeOperationRecoveryEvidence evidence(const AdapterScope& scope) {
    return {
        *NativeName::from_posix(".ck-stage-9"),
        *NativeName::from_windows(u"report-copy.txt"),
        *NativeDurableIdentity::from_live(live_identity(scope, IdentityScheme::PosixDeviceInode, 11, 2)),
        *NativeDurableIdentity::from_live(live_identity(scope, IdentityScheme::PosixDeviceInode, 12)),
        *NativeDurableIdentity::from_live(live_identity(scope, IdentityScheme::PosixDeviceInode, 13, 4)), {}, {}, {}};
}
}

CK_TEST(native_recovery_evidence_roundtrips_native_names_and_durable_identities) {
    AdapterScope scope;
    auto original = evidence(scope);
    original.staging_identity = *NativeDurableIdentity::from_live(
        live_identity(scope, IdentityScheme::PosixDeviceInode, 14, 5));
    original.destination_address = std::move(NativeAbsolutePath::from_posix("/var/lib/ck/destination")).take_value();
    auto encoded = encode_native_operation_recovery_evidence(original);
    CK_CHECK(encoded.value() && encoded.value()->size() < 4096);
    if (!encoded.value()) return;
    auto decoded = decode_native_operation_recovery_evidence(*encoded.value());
    CK_CHECK(decoded.value());
    if (decoded.value()) {
        CK_CHECK(decoded.value()->staging_name == original.staging_name);
        CK_CHECK(decoded.value()->destination_name == original.destination_name);
        CK_CHECK(decoded.value()->source_parent_identity == original.source_parent_identity);
        CK_CHECK(decoded.value()->source_identity == original.source_identity);
        CK_CHECK(decoded.value()->destination_identity == original.destination_identity);
        CK_CHECK(decoded.value()->staging_identity == original.staging_identity);
        CK_CHECK(decoded.value()->destination_address == original.destination_address);
        CK_CHECK(decoded.value()->content_digest == original.content_digest);
        CK_CHECK(decoded.value()->destination_identity.matches_live(
            live_identity(scope, IdentityScheme::PosixDeviceInode, 13, 4)));
    }
}

CK_TEST(native_recovery_evidence_roundtrips_verified_content_digest_without_destination_address) {
    AdapterScope scope;
    auto original = evidence(scope);
    std::array<std::byte, 32> digest{};
    digest[0] = std::byte{0x5a};
    original.content_digest = digest;
    auto encoded = encode_native_operation_recovery_evidence(original);
    CK_CHECK(encoded.value());
    if (!encoded.value()) return;
    auto decoded = decode_native_operation_recovery_evidence(*encoded.value());
    CK_CHECK(decoded.value());
    if (decoded.value()) {
        CK_CHECK(!decoded.value()->destination_address);
        CK_CHECK(decoded.value()->content_digest == original.content_digest);
    }
}

CK_TEST(native_recovery_evidence_does_not_treat_scope_or_names_as_authority) {
    AdapterScope first;
    AdapterScope second;
    const auto durable = *NativeDurableIdentity::from_live(
        live_identity(first, IdentityScheme::PosixDeviceInode, 99));
    CK_CHECK(durable.matches_live(live_identity(first, IdentityScheme::PosixDeviceInode, 99)));
    // The durable form deliberately omits AdapterScope so a restarted adapter
    // can compare native IDs after binding the journal to a fresh root handle.
    CK_CHECK(durable.matches_live(live_identity(second, IdentityScheme::PosixDeviceInode, 99)));
    CK_CHECK(!durable.matches_live(live_identity(first, IdentityScheme::PosixDeviceInode, 100)));
    CK_CHECK(!NativeDurableIdentity::from_live(
        live_identity(first, IdentityScheme::Unknown, 99)));
}

CK_TEST(native_recovery_evidence_rejects_corruption_truncation_and_budget_overruns) {
    AdapterScope scope;
    auto encoded = encode_native_operation_recovery_evidence(evidence(scope));
    CK_CHECK(encoded.value());
    if (!encoded.value()) return;
    auto truncated = *encoded.value();
    truncated.pop_back();
    CK_CHECK(!decode_native_operation_recovery_evidence(truncated).value());
    auto corrupt = *encoded.value();
    corrupt[0] ^= std::byte{1};
    CK_CHECK(!decode_native_operation_recovery_evidence(corrupt).value());
    CK_CHECK(!encode_native_operation_recovery_evidence(evidence(scope), {16, 32 * 1024}).value());
    CK_CHECK(!decode_native_operation_recovery_evidence(*encoded.value(), {16, 32 * 1024}).value());
}
