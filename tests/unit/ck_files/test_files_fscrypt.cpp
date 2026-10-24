// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_cursor.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
namespace {
using namespace ck::files;
class EmptyCursor final : public DirectoryCursor {
    Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
};
BasicMetadata directory() {
    BasicMetadata m; m.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
    m.identity = Observation<ObjectIdentity>::available({AdapterScope{}, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(1), NativeIdentifier::from_u64(2), {}});
    return m;
}
auto qualify(const BasicMetadata& a, const BasicMetadata& b, std::optional<bool> present = {}) {
    return detail::qualify_fscrypt_directory(Observation<BasicMetadata>::available(a), Observation<BasicMetadata>::available(b), present);
}
}
CK_TEST(fscrypt_default_query_is_unsupported_not_a_report_of_no_encryption) {
    EmptyCursor cursor;const auto result=cursor.inspect_fscrypt_policy();
    CK_CHECK(result.state()==ObservationState::Unsupported && !result.value());
    CK_CHECK(result.unavailable_detail()->error.operation=="directory.fscrypt_policy");
}
CK_TEST(fscrypt_decoder_preserves_exact_v1_and_v2_bytes_and_exposes_only_structural_fields) {
    for(const auto version:{0U,2U}) {
        std::array<std::byte,24> bytes{};for(std::size_t i=0;i<bytes.size();++i)bytes[i]=static_cast<std::byte>(0xa0U+i);
        bytes[0]=static_cast<std::byte>(version);const auto size=version==0?12U:24U;
        const auto result=FscryptPolicyData::decode(std::span(bytes).first(size));CK_CHECK(result.value());
        const auto& data=*result.value();CK_CHECK(data.version()==version && data.contents_mode()==0xa1 && data.filenames_mode()==0xa2 && data.flags()==0xa3);
        CK_CHECK(std::equal(data.raw().begin(),data.raw().end(),bytes.begin(),bytes.begin()+size));
        CK_CHECK(data.key_reference().size()==(version==0?8U:16U) && data.key_reference()[0]==bytes[version==0?4:8]);
    }
}
CK_TEST(fscrypt_decoder_rejects_wrong_lengths_and_unknown_versions_without_returning_a_prefix) {
    std::array<std::byte,32> bytes{};
    for(const auto version:{0U,2U})for(std::size_t size=0;size<=bytes.size();++size) {
        bytes[0]=static_cast<std::byte>(version);const auto result=FscryptPolicyData::decode(std::span(bytes).first(size));
        CK_CHECK(static_cast<bool>(result.value())==(size==(version==0?12U:24U)));
    }
    for(const auto version:{1U,3U,255U}) {
        bytes[0]=static_cast<std::byte>(version);const auto result=FscryptPolicyData::decode(std::span(bytes).first(24));
        CK_CHECK(result.state()==ObservationState::Unsupported && !result.value());
    }
}
CK_TEST(fscrypt_directory_qualification_rejects_changed_or_unqualified_ids_and_non_directory_objects) {
    const auto before=directory();CK_CHECK(qualify(before,before).value());
    for(int variant=0;variant<8;++variant) {
        auto after=before;auto id=*before.identity.value();
        if(variant==0)id.object=NativeIdentifier::from_u64(3);
        if(variant==1)id.volume=NativeIdentifier::from_u64(3);
        if(variant==2)id.scope=AdapterScope{};
        if(variant==3)id.object={};
        if(variant==4)id.generation=1;
        if(variant==5)id.scheme=IdentityScheme::WindowsFileId128;
        after.identity=Observation<ObjectIdentity>::available(id);
        if(variant==6)after.kind=Observation<ObjectKind>::available(ObjectKind::SymbolicLink);
        if(variant==7)after.kind=Observation<ObjectKind>::available(static_cast<ObjectKind>(255));
        CK_CHECK(qualify(before,after).state()==(variant<2||variant==6?ObservationState::Changed:variant==7?ObservationState::Failed:ObservationState::Unknown));
    }
}
CK_TEST(fscrypt_policy_presence_is_corroborated_only_by_known_encryption_bits_and_conflicts_are_explicit) {
    for(bool present:{false,true})for(bool set:{false,true})for(bool known:{false,true}) {
        auto before=directory(),after=before;before.linux_attributes=Observation<LinuxFileAttributes>::available({set?0x800U:0U,known?0x800U:0U});after.linux_attributes=before.linux_attributes;
        const auto result=qualify(before,after,present);CK_CHECK(static_cast<bool>(result.value())==(!known||set==present));
        if(!result.value())CK_CHECK(result.state()==ObservationState::Changed);
    }
    auto before=directory(),after=before;before.linux_attributes=Observation<LinuxFileAttributes>::available({0,0x800});after.linux_attributes=Observation<LinuxFileAttributes>::available({0x800,0x800});
    CK_CHECK(qualify(before,after,false).state()==ObservationState::Changed && qualify(before,after,true).state()==ObservationState::Changed);
}
CK_TEST(fscrypt_qualification_preserves_native_query_kind_and_identity_field_failures) {
    const auto before=directory();
    for(auto state:{ObservationState::Unsupported,ObservationState::Unknown,ObservationState::Denied,ObservationState::Gone,ObservationState::Changed,ObservationState::Failed})for(bool second:{false,true})for(int field=0;field<3;++field) {
        const NativeError error{ErrorDomain::Posix,13,"injected.fscrypt"};auto a=Observation<BasicMetadata>::available(before),b=a;auto changed=before;
        if(field==0){if(second)b=Observation<BasicMetadata>::unavailable(state,error);else a=Observation<BasicMetadata>::unavailable(state,error);}
        if(field==1)changed.kind=Observation<ObjectKind>::unavailable(state,error);
        if(field==2)changed.identity=Observation<ObjectIdentity>::unavailable(state,error);
        if(field!=0){if(second)b=Observation<BasicMetadata>::available(changed);else a=Observation<BasicMetadata>::available(changed);}
        const auto result=detail::qualify_fscrypt_directory(a,b);CK_CHECK(result.state()==state && result.unavailable_detail()->error==error);
    }
}
