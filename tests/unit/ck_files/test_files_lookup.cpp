// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_cursor.hpp"
#include "cvision/testing/cktest.hpp"
#include <cerrno>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace ck::files;
static_assert(!std::is_copy_constructible_v<ChildLookup>&&std::is_nothrow_move_constructible_v<ChildLookup>&&std::is_nothrow_move_assignable_v<ChildLookup>);
NativeName name(std::string value="component") {return *NativeName::from_posix(std::move(value));}
ChildLookupContext context(){return {name(),ChildLookupMode::NativeDefault,{}};}
class Entry final : public OpenedEntry {
public:
    explicit Entry(int& live):live_(live){++live_;}~Entry()override{--live_;}
    Observation<BasicMetadata> inspect_basic()const override{return Observation<BasicMetadata>::unavailable(ObservationState::Denied,{ErrorDomain::Posix,EACCES,"fake.metadata"});}
private:int& live_;
};
class LegacyCursor final : public DirectoryCursor {
public:
    mutable int opens=0;
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName&,EntryAccess)const override {
        ++opens;return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Gone,{ErrorDomain::Posix,ENOENT,"legacy.open"});
    }
protected:Observation<DirectoryStep> next_step()override{return Observation<DirectoryStep>::available(DirectoryEnd{});}
};
}
CK_TEST(child_lookup_defaults_do_not_fabricate_absence) {
    const ChildLookup result;CK_CHECK(result.state()==ChildLookupState::Unknown&&!result.context()&&!result.entry()&&!result.missing_witness());
    const Observation<ChildLookup> observed;CK_CHECK(observed.state()==ObservationState::Unknown);
}
CK_TEST(found_lookup_owns_its_entry_even_when_metadata_is_denied) {
    int live=0;
    {
        auto result=ChildLookup::found(context(),std::make_unique<Entry>(live));
        CK_CHECK(result.state()==ChildLookupState::Found&&result.entry()&&live==1&&!result.missing_witness());
        CK_CHECK(result.entry()->inspect_basic().state()==ObservationState::Denied);
        ChildLookup moved(std::move(result));CK_CHECK(moved.state()==ChildLookupState::Found&&live==1);
        CK_CHECK(result.state()==ChildLookupState::Unknown&&!result.context()&&!result.entry());
        ChildLookup destination=ChildLookup::found(context(),std::make_unique<Entry>(live));CK_CHECK(live==2);destination=std::move(moved);
        CK_CHECK(destination.entry()&&moved.state()==ChildLookupState::Unknown&&!moved.context()&&live==1);
    }
    CK_CHECK(live==0);
}
CK_TEST(missing_lookup_retains_context_and_evidence_and_moving_clears_the_source) {
    auto ctx=context();ctx.requested=name("raw-\xff");ctx.mode=ChildLookupMode::WindowsIgnoreCase;
    ctx.directory=Observation<ObjectIdentity>::unavailable(ObservationState::Denied,{ErrorDomain::Adapter,12,"fake.parent_identity"});
    const NativeError witness{ErrorDomain::Posix,ENOENT,"fake.lookup"};
    auto result=ChildLookup::missing(ctx,witness);auto moved=std::move(result);
    CK_CHECK(result.state()==ChildLookupState::Unknown&&!result.missing_witness()&&!result.context());
    CK_CHECK(moved.state()==ChildLookupState::Missing&&moved.missing_witness()&&*moved.missing_witness()==witness&&!moved.entry());
    CK_CHECK(moved.context()&&moved.context()->requested==ctx.requested&&moved.context()->mode==ctx.mode);
    CK_CHECK(moved.context()->directory.state()==ObservationState::Denied);
}
CK_TEST(lookup_factories_reject_null_found_entries_and_missing_evidence) {
    bool rejected=false;try{(void)ChildLookup::found(context(),nullptr);}catch(const std::invalid_argument&){rejected=true;}CK_CHECK(rejected);
    for(const NativeError& witness:{NativeError{},NativeError{ErrorDomain::Posix,0,"zero"},NativeError{ErrorDomain::Posix,ENOENT,{}},NativeError{static_cast<ErrorDomain>(999),1,"invalid"}}) {
        rejected=false;try{(void)ChildLookup::missing(context(),witness);}catch(const std::invalid_argument&){rejected=true;}CK_CHECK(rejected);
    }
    auto ctx=context();ctx.mode=static_cast<ChildLookupMode>(999);rejected=false;
    try{(void)ChildLookup::missing(ctx,{ErrorDomain::Posix,ENOENT,"fake"});}catch(const std::invalid_argument&){rejected=true;}CK_CHECK(rejected);
}
CK_TEST(generic_directory_lookup_never_infers_absence_from_legacy_open_errors) {
    LegacyCursor cursor;const auto result=cursor.lookup_child(name(),ChildLookupMode::NativeDefault);
    CK_CHECK(result.state()==ObservationState::Unsupported&&cursor.opens==0);
    CK_CHECK(cursor.lookup_child(name(),ChildLookupMode::WindowsIgnoreCase).state()==ObservationState::Unsupported&&cursor.opens==0);
}
CK_TEST(invalid_lookup_modes_and_moved_components_fail_without_invoking_legacy_lookup) {
    LegacyCursor cursor;CK_CHECK(cursor.lookup_child(name(),static_cast<ChildLookupMode>(999)).state()==ObservationState::Failed&&cursor.opens==0);
    auto moving=name();const auto owned=std::move(moving);CK_CHECK(owned.is_valid());
    if(!moving.is_valid())CK_CHECK(cursor.lookup_child(moving,ChildLookupMode::NativeDefault).state()==ObservationState::Failed&&cursor.opens==0);
}
CK_TEST(native_absence_classifier_requires_the_exact_native_family_domain_state_and_code) {
    using detail::ChildLookupFamily;
    const auto test=[](ObservationState state,ErrorDomain domain,std::int64_t code,ChildLookupFamily family){return detail::definite_child_absence({state,{domain,code,"lookup"}},family);};
    CK_CHECK(test(ObservationState::Gone,ErrorDomain::Posix,ENOENT,ChildLookupFamily::PosixAt));
    for(auto code:{0xc0000034LL,0xc000000fLL})CK_CHECK(test(ObservationState::Gone,ErrorDomain::NtStatus,code,ChildLookupFamily::WindowsNt));
    for(auto state:{ObservationState::Available,ObservationState::Unknown,ObservationState::Denied,ObservationState::Failed,ObservationState::Changed,ObservationState::Unsupported}){
        CK_CHECK(!test(state,ErrorDomain::Posix,ENOENT,ChildLookupFamily::PosixAt));
        CK_CHECK(!test(state,ErrorDomain::NtStatus,0xc0000034LL,ChildLookupFamily::WindowsNt));
    }
    for(auto code:{ENOTDIR,EACCES,EINVAL,EIO})CK_CHECK(!test(ObservationState::Gone,ErrorDomain::Posix,code,ChildLookupFamily::PosixAt));
    for(auto code:{0xc000003aLL,0xc0000022LL,0xc0000056LL,0x103LL,0LL,-1073741772LL})CK_CHECK(!test(ObservationState::Gone,ErrorDomain::NtStatus,code,ChildLookupFamily::WindowsNt));
    for(auto domain:{ErrorDomain::None,ErrorDomain::Adapter,ErrorDomain::Win32}){
        CK_CHECK(!test(ObservationState::Gone,domain,ENOENT,ChildLookupFamily::PosixAt));
        CK_CHECK(!test(ObservationState::Gone,domain,0xc0000034LL,ChildLookupFamily::WindowsNt));
    }
    CK_CHECK(!test(ObservationState::Gone,ErrorDomain::Posix,ENOENT,static_cast<ChildLookupFamily>(999)));
}
CK_TEST(consuming_a_lookup_observation_does_not_leave_an_available_or_missing_shell) {
    auto observed=Observation<ChildLookup>::available(ChildLookup::missing(context(),{ErrorDomain::Posix,ENOENT,"fake"}));
    const auto consumed=std::move(observed).take_value();CK_CHECK(consumed.state()==ChildLookupState::Missing&&observed.state()==ObservationState::Unknown&&!observed.value());
}
