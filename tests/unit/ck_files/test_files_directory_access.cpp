// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_cursor.hpp"
#include "cvision/testing/cktest.hpp"
namespace {
using namespace ck::files;
class EmptyCursor final : public DirectoryCursor {
    Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
};
BasicMetadata directory() {
    BasicMetadata m;
    m.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
    m.identity = Observation<ObjectIdentity>::available({AdapterScope{}, IdentityScheme::PosixDeviceInode,
        NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}});
    m.bsd_flags = Observation<std::uint32_t>::available(0);
    return m;
}
auto qualify(const BasicMetadata& a, const BasicMetadata& b) {
    return detail::qualify_access_directory(Observation<BasicMetadata>::available(a), Observation<BasicMetadata>::available(b), StorageModel::DarwinStatFlags);
}
}
CK_TEST(directory_access_default_never_claims_native_rights_and_rejects_invalid_requests) {
    EmptyCursor cursor;
    for (auto access : {DirectoryAccess::List, DirectoryAccess::Search, DirectoryAccess::AddFile, DirectoryAccess::AddDirectory}) {
        const auto result = cursor.check_access(access);
        CK_CHECK(result.state() == ObservationState::Unsupported && !result.value());
    }
    CK_CHECK(cursor.check_access(static_cast<DirectoryAccess>(255)).state() == ObservationState::Failed);
}
CK_TEST(directory_access_qualification_preserves_metadata_kind_identity_and_provider_failures) {
    const auto before = directory();
    CK_CHECK(qualify(before, before).value());
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
        ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        const NativeError error{ErrorDomain::Posix, 13, "injected.access_metadata"};
        const auto failure = Observation<BasicMetadata>::unavailable(state, error);
        for (bool first : {false, true}) {
            const auto ok = Observation<BasicMetadata>::available(before);
            const auto result = detail::qualify_access_directory(first ? failure : ok, first ? ok : failure, StorageModel::DarwinStatFlags);
            CK_CHECK(result.state() == state && result.unavailable_detail()->error == error);
        }
        auto changed = before;
        changed.identity = Observation<ObjectIdentity>::unavailable(state, error);
        const auto identity = qualify(before, changed);
        CK_CHECK(identity.state() == state && identity.unavailable_detail()->error == error);
        changed = before; changed.kind = Observation<ObjectKind>::unavailable(state, error);
        CK_CHECK(qualify(before, changed).state() == state);
        changed = before; changed.bsd_flags = Observation<std::uint32_t>::unavailable(state, error);
        CK_CHECK(qualify(before, changed).state() == state);
    }
    auto changed = before; changed.kind = Observation<ObjectKind>::available(ObjectKind::SymbolicLink);
    CK_CHECK(qualify(before, changed).state() == ObservationState::Changed);
    changed.kind = Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
    CK_CHECK(qualify(before, changed).state() == ObservationState::Failed);
    changed = before; changed.bsd_flags = Observation<std::uint32_t>::available(0x40000000U); // UF_DATALESS
    CK_CHECK(qualify(before, changed).state() == ObservationState::Unknown);
}
CK_TEST(directory_access_qualification_never_equates_changed_or_unqualified_native_ids) {
    const auto before = directory();
    for (int variant = 0; variant < 6; ++variant) {
        auto after = before; auto id = *before.identity.value();
        if (variant == 0) id.object = NativeIdentifier::from_u64(3);
        if (variant == 1) id.volume = NativeIdentifier::from_u64(3);
        if (variant == 2) id.scope = AdapterScope{};
        if (variant == 3) id.object = {};
        if (variant == 4) id.scheme = IdentityScheme::Unknown;
        if (variant == 5) id.generation = 1;
        after.identity = Observation<ObjectIdentity>::available(id);
        CK_CHECK(qualify(before, after).state() == (variant < 2 ? ObservationState::Changed : ObservationState::Unknown));
    }
}

CK_TEST(directory_access_linux_check_does_not_claim_generic_provider_state) {
    const auto metadata = Observation<BasicMetadata>::available(directory());
    const auto result = detail::qualify_access_directory(metadata, metadata, StorageModel::LinuxStatx);
    CK_CHECK(result.value());
    CK_CHECK(detail::qualify_access_directory(metadata, metadata, StorageModel::Unknown).state() == ObservationState::Failed);
}
