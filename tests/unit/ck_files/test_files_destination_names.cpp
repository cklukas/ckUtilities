// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_names_job.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
namespace {
using namespace ck::files;
using FS = ck::testing::SimulatedFileSystem;
using Op = FS::Operation;
using State = DestinationNamesState;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required destination-name fixture observation unavailable");
    return std::move(value).take_value();
}
NativeName name(std::string bytes) { return *NativeName::from_posix(std::move(bytes)); }
struct Fixture {
    FS fs; FS::Pointer root = fs.make_node(ObjectKind::Directory);
    Fixture() {
#ifdef _WIN32
        fs.bind(L"V:/simulated-destination-names", root);
#else
        fs.bind("/simulated-destination-names", root);
#endif
        set_plain();
    }
    LocationInfo plain() {
        LocationInfo info; info.family = LocationQueryFamily::LinuxDescriptor; info.directory = fs.metadata(root).identity;
        info.filesystem_type_code = Observation<std::uint64_t>::available(0xef53);
        info.linux_directory_inode_flags = Observation<std::uint32_t>::available(0); return info;
    }
    void set_plain() { fs.set_location(root, Observation<LocationInfo>::available(plain())); }
    std::size_t calls(Op op) const { return static_cast<std::size_t>(std::count_if(fs.calls().begin(), fs.calls().end(), [=](const auto& c){return c.operation == op;})); }
    DestinationBatchJob::Result batch(std::vector<NativeName> names, NameEncoding encoding = NameEncoding::PosixBytes) {
#ifdef _WIN32
        auto cursor = take(fs.open_directory(L"V:/simulated-destination-names"));
#else
        auto cursor = take(fs.open_directory("/simulated-destination-names"));
#endif
        DestinationBatchLimits limits; limits.rows = 4096;
        DestinationBatchJob job(std::shared_ptr<const DirectoryCursor>(std::move(cursor)), std::move(names), encoding, limits, fs.clock());
        for (int i=0;i<1000;++i) { if (job.advance()==DestinationBatchState::Complete) return take(job.completed()); }
        throw std::runtime_error("batch fixture failed");
    }
};
DestinationNamesJob::Result finish(DestinationNamesJob& job) {
    for (int i=0;i<1000;++i) {
        const auto state=job.advance(); if(state==State::Complete)return take(job.completed());
        if(state==State::Failed)throw std::runtime_error("name assessment fixture failed");
    }
    throw std::runtime_error("assessment fixture exceeded step bound");
}
}
CK_TEST(destination_names_assesses_exact_groups_and_distinct_rules_without_new_child_lookups) {
    Fixture f; auto batch=f.batch({name("A"),name("A"),name("a"),name("\xc3\xa9"),name("e\xcc\x81")});
    const auto locations=f.calls(Op::InspectLocation), lookups=f.calls(Op::LookupChild);
    DestinationNamesJob job(batch,{},f.fs.clock()); const auto result=finish(job);
    CK_CHECK(result->summary().mapped_rows==5 && result->summary().unmapped_rows==0 && result->summary().groups==4);
    CK_CHECK(result->summary().duplicate_groups==1 && result->summary().rows_in_duplicate_groups==2);
    CK_CHECK(f.calls(Op::InspectLocation)==locations+2 && f.calls(Op::LookupChild)==lookups);
    const auto row=take(result->row(1)); CK_CHECK(row.exact_leader==0 && row.exact_count==2 && row.other_groups==3);
    CK_CHECK(take(row.against_other_groups)==OtherNameGroups::DistinctUnderReportedRules);
    CK_CHECK(take(result->compare(0,1)).relation==ComponentNameRelation::IdenticalNativeUnits);
    CK_CHECK(take(result->compare(0,2)).relation==ComponentNameRelation::DistinctUnderReportedRules);
    CK_CHECK(take(result->compare(3,4)).relation==ComponentNameRelation::DistinctUnderReportedRules);
    CK_CHECK(result->batch()==batch && result->rule().value() && result->before().value() && result->after().value());
}
CK_TEST(destination_names_exact_duplicates_survive_unknown_rules_and_same_object_is_not_name_equivalence) {
    Fixture f; auto object=f.fs.make_node(ObjectKind::RegularFile); f.fs.put(f.root,name("a"),object); f.fs.put(f.root,name("b"),object);
    auto batch=f.batch({name("a"),name("a"),name("b")}); auto location=f.plain(); location.family=LocationQueryFamily::WindowsHandle;
    f.fs.set_location(f.root,Observation<LocationInfo>::available(location)); const auto locations=f.calls(Op::InspectLocation);
    DestinationNamesJob job(batch,{},f.fs.clock());const auto result=finish(job);
    CK_CHECK(result->summary().duplicate_groups==1 && result->other_groups().state()==ObservationState::Unsupported);
    CK_CHECK(take(result->compare(0,1)).relation==ComponentNameRelation::IdenticalNativeUnits);
    CK_CHECK(result->compare(0,2).state()==ObservationState::Unsupported && batch->reported_object_leader(2)==0);
    CK_CHECK(f.calls(Op::InspectLocation)==locations+1 && !result->after().value());
}
CK_TEST(destination_names_zero_or_one_mapped_group_needs_no_native_policy_and_does_not_clear_mapping_failures) {
    for(int mode=0;mode<4;++mode) {
        Fixture f;const auto raw=*NativeName::from_windows(std::u16string{char16_t{0xd800}});
        auto batch=f.batch(mode==0?std::vector<NativeName>{}:mode==1?std::vector<NativeName>{name("a"),name("a")}:mode==2?std::vector<NativeName>{name("a"),raw}:std::vector<NativeName>{raw,raw});
        const auto locations=f.calls(Op::InspectLocation);DestinationNamesJob job(batch,{},f.fs.clock());const auto result=finish(job);
        CK_CHECK(take(result->other_groups())==OtherNameGroups::None && f.calls(Op::InspectLocation)==locations);
        if(mode==1) { CK_CHECK(result->summary().duplicate_groups==1); CK_CHECK(take(result->row(0)).other_groups==0); }
        if(mode==2) {
            const auto row=take(result->row(1)); CK_CHECK(!row.exact_leader && row.exact_count==0 && row.against_other_groups.state()==batch->entries()[1].mapped.state());
            CK_CHECK(result->summary().unmapped_rows==1 && result->compare(0,1).state()==batch->entries()[1].mapped.state());
        }
        if(mode==3) { CK_CHECK(result->summary().unmapped_rows==2 && result->summary().mapped_rows==0 && result->summary().groups==0); CK_CHECK(!take(result->row(0)).against_other_groups.value() && !result->compare(0,1).value()); }
        CK_CHECK(result->row(batch->entries().size()).state()==ObservationState::Failed);
        CK_CHECK(result->compare(batch->entries().size(),0).state()==ObservationState::Failed);
    }
}
CK_TEST(destination_names_preserves_every_native_failure_state_in_either_location_observation) {
    for(auto state:{ObservationState::Unsupported,ObservationState::Unknown,ObservationState::Denied,ObservationState::Gone,ObservationState::Changed,ObservationState::Failed})for(bool second:{false,true}) {
        Fixture f;auto batch=f.batch({name("x"),name("y")});const auto before=f.calls(Op::InspectLocation);
        if(second)f.fs.enqueue(Op::InspectLocation,{});
        const NativeError error{ErrorDomain::Posix,13,"injected.destination_names"};f.fs.enqueue(Op::InspectLocation,{{UnavailableObservation{state,error}},{},{},false});
        DestinationNamesJob job(batch,{},f.fs.clock());const auto result=finish(job);
        CK_CHECK(result->other_groups().state()==state && result->other_groups().unavailable_detail()->error==error);
        CK_CHECK(result->compare(0,1).state()==state && take(result->row(0)).against_other_groups.state()==state);
        CK_CHECK(f.calls(Op::InspectLocation)==before+(second?2:1));
    }
}
CK_TEST(destination_names_changed_identity_and_flags_do_not_turn_into_distinct_name_evidence) {
    for(bool flags:{false,true}) {
        Fixture f;auto batch=f.batch({name("x"),name("y")});f.fs.enqueue(Op::InspectLocation,{});
        f.fs.enqueue(Op::InspectLocation,{{},{},[&]{auto location=f.plain();if(flags)location.linux_directory_inode_flags=Observation<std::uint32_t>::available(0x40000000);else{auto id=take(location.directory);id.object=NativeIdentifier::from_u64(999);location.directory=Observation<ObjectIdentity>::available(id);}f.fs.set_location(f.root,Observation<LocationInfo>::available(location));},false});
        DestinationNamesJob job(batch,{},f.fs.clock());const auto result=finish(job);
        CK_CHECK(result->rule().state()==ObservationState::Changed && result->compare(0,1).state()==ObservationState::Changed);
    }
}
CK_TEST(destination_names_rule_encoding_applicability_is_independent_of_native_policy_qualification) {
    Fixture f;auto batch=f.batch({*NativeName::from_windows(u"a"),*NativeName::from_windows(u"b")},NameEncoding::WindowsUtf16);
    DestinationNamesJob job(batch,{},f.fs.clock());const auto result=finish(job);
    CK_CHECK(result->rule().value() && result->other_groups().state()==ObservationState::Unsupported);
    CK_CHECK(result->compare(0,1).state()==ObservationState::Unsupported);
}
CK_TEST(destination_names_completed_queries_survive_cancellation_and_time_yields_without_repeating_io) {
    for(bool second:{false,true})for(bool slow:{false,true}) {
        Fixture f;auto batch=f.batch({name("a"),name("b")});const auto initial=f.calls(Op::InspectLocation);std::stop_source stop;
        if(second)f.fs.enqueue(Op::InspectLocation,{});
        f.fs.enqueue(Op::InspectLocation,{{},slow?std::chrono::milliseconds(25):MonotonicClock::Duration{},slow?std::function<void()>{}:std::function<void()>{[&]{stop.request_stop();}},false});
        DestinationNamesJob job(batch,{},f.fs.clock());const auto state=job.advance(stop.get_token());
        CK_CHECK(state==(slow?State::Collecting:State::Paused) && !job.completed().value());
        const auto result=finish(job);CK_CHECK(result->rule().value() && f.calls(Op::InspectLocation)==initial+2);
    }
}
CK_TEST(destination_names_large_batches_yield_during_linear_validation_and_keep_source_rows) {
    Fixture f;std::vector<NativeName> names;for(int i=0;i<600;++i)names.push_back(name(std::to_string(i)));
    auto batch=f.batch(std::move(names));const auto initial=f.calls(Op::InspectLocation);DestinationNamesJob job(batch,{},f.fs.clock());
    CK_CHECK(job.advance()==State::Collecting && f.calls(Op::InspectLocation)==initial);
    const auto result=finish(job);CK_CHECK(result->summary().groups==600 && result->summary().mapped_rows==600);
    CK_CHECK(take(result->row(599)).other_groups==599 && f.calls(Op::InspectLocation)==initial+2);
}
CK_TEST(destination_names_limits_fail_before_new_io_and_configuration_is_validated) {
    for(int mode=0;mode<8;++mode) {
        Fixture f;auto batch=f.batch({name("aa"),name("b")});const auto calls=f.calls(Op::InspectLocation);DestinationNamesLimits limits;
        if(mode==0)limits.rows=1;
        if(mode==1)limits.component_bytes=1;
        if(mode==2)limits.rows=0;
        if(mode==3)limits.rows=4097;
        if(mode==4)limits.component_bytes=0;
        if(mode==5)limits.component_bytes=NameMappingLimits::hard_native_bytes+1;
        if(mode==6)limits.time_slice={};
        if(mode==7)limits.time_slice=std::chrono::seconds(2);
        bool threw=false;try{DestinationNamesJob job(batch,limits,f.fs.clock());
        if(mode<2)CK_CHECK(job.advance()==State::Failed && !job.completed().value());}catch(const std::invalid_argument&){threw=true;}
        CK_CHECK(threw==(mode>=2) && f.calls(Op::InspectLocation)==calls);
    }
    bool threw=false;try{DestinationNamesJob job({});}catch(const std::invalid_argument&){threw=true;}CK_CHECK(threw);
    Fixture f;auto batch=f.batch({});threw=false;try{DestinationNamesJob job(batch,{},nullptr);}catch(const std::invalid_argument&){threw=true;}CK_CHECK(threw);
}
CK_TEST(destination_names_exceptions_and_clock_reversal_poison_staging_and_prevent_retries) {
    for(bool allocation:{false,true})for(bool second:{false,true}) {
        Fixture f;auto batch=f.batch({name("a"),name("b")});if(second)f.fs.enqueue(Op::InspectLocation,{});
        f.fs.enqueue(Op::InspectLocation,{{},{},[=]{if(allocation)throw std::bad_alloc{};throw std::runtime_error("injected");},false});
        DestinationNamesJob job(std::move(batch),{},f.fs.clock());bool caught=false;try{(void)job.advance();}catch(const std::exception&){caught=true;}
        CK_CHECK(caught && job.state()==State::Failed && f.fs.live_cursors()==0);
        const auto calls=f.calls(Op::InspectLocation);CK_CHECK(job.advance()==State::Failed && !job.abandon() && f.calls(Op::InspectLocation)==calls);
    }
    Fixture f;auto batch=f.batch({name("a"),name("b")});f.fs.enqueue(Op::InspectLocation,{{},std::chrono::milliseconds(25),{},false});DestinationNamesJob job(std::move(batch),{},f.fs.clock());
    CK_CHECK(job.advance()==State::Collecting);f.fs.clock()->value=MonotonicClock::TimePoint{}-std::chrono::seconds(1);
    CK_CHECK(job.advance()==State::Failed && !job.completed().value() && f.fs.live_cursors()==0);
}
CK_TEST(destination_names_issued_results_retain_batch_owners_and_abandonment_retires_only_private_state) {
    Fixture f;auto object=f.fs.make_node(ObjectKind::RegularFile);f.fs.put(f.root,name("a"),object);auto batch=f.batch({name("a"),name("b")});
    DestinationNamesJob job(std::move(batch),{},f.fs.clock());auto result=finish(job);CK_CHECK(job.abandon() && f.fs.live_cursors()==1 && f.fs.live_entries()==1);
    result.reset();CK_CHECK(f.fs.live_cursors()==0 && f.fs.live_entries()==0 && !job.abandon());
    auto empty=f.batch({name("a"),name("b")});DestinationNamesJob paused(std::move(empty),{},f.fs.clock());std::stop_source stop;stop.request_stop();
    CK_CHECK(paused.advance(stop.get_token())==State::Paused && paused.abandon() && f.fs.live_cursors()==0 && paused.advance()==State::Abandoned);
}

CK_TEST(destination_names_abandonment_after_a_completed_query_releases_staged_batch_without_another_query) {
    Fixture f;auto batch=f.batch({name("a"),name("b")});const auto initial=f.calls(Op::InspectLocation);
    f.fs.enqueue(Op::InspectLocation,{{},std::chrono::milliseconds(25),{},false});
    DestinationNamesJob job(std::move(batch),{},f.fs.clock());CK_CHECK(job.advance()==State::Collecting);
    CK_CHECK(f.calls(Op::InspectLocation)==initial+1 && job.abandon() && f.fs.live_cursors()==0);
    CK_CHECK(job.advance()==State::Abandoned && f.calls(Op::InspectLocation)==initial+1 && !job.completed().value());
}
CK_TEST(destination_names_utf16_late_component_limit_is_checked_before_any_new_native_query) {
    Fixture f;auto batch=f.batch({*NativeName::from_windows(u"a"),*NativeName::from_windows(u"bb")},NameEncoding::WindowsUtf16);
    const auto initial=f.calls(Op::InspectLocation);DestinationNamesLimits limits;limits.component_bytes=3;
    DestinationNamesJob job(batch,limits,f.fs.clock());CK_CHECK(job.advance()==State::Failed && !job.completed().value());
    CK_CHECK(f.calls(Op::InspectLocation)==initial && job.failure()->error.operation=="destination_names.component_bytes");
}
