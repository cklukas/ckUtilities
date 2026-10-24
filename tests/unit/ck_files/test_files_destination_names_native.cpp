// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_names_job.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
using namespace ck::files;
namespace fs=std::filesystem;
template<class T> T take(Observation<T> value) {
    if(!value.value())throw std::runtime_error("required native destination-name observation unavailable");
    return std::move(value).take_value();
}
NativeName name(const char* text) {
    auto raw=*NativeName::from_posix(text);
#ifdef _WIN32
    return take(map_native_name(raw,NameEncoding::WindowsUtf16));
#else
    return raw;
#endif
}
constexpr auto encoding=
#ifdef _WIN32
    NameEncoding::WindowsUtf16;
#else
    NameEncoding::PosixBytes;
#endif
DestinationBatchJob::Result batch(std::shared_ptr<const DirectoryCursor> cursor,std::vector<NativeName> names) {
    DestinationBatchJob job(std::move(cursor),std::move(names),encoding);
    for(int i=0;i<1000;++i){if(job.advance()==DestinationBatchState::Complete)return take(job.completed());}
    throw std::runtime_error("native destination batch failed");
}
DestinationNamesJob::Result finish(DestinationNamesJob& job) {
    for(int i=0;i<1000;++i){if(job.advance()==DestinationNamesState::Complete)return take(job.completed());}
    throw std::runtime_error("native destination-name assessment failed");
}
}
CK_TEST(native_destination_names_groups_missing_candidates_without_treating_unqualified_rules_as_collision_freedom) {
    const ck::testing::ScratchDirectory scratch("files-destination-names");NativeFileSystem filesystem;
    auto candidates=batch(std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(scratch.path()))),{name("A"),name("a"),name("A")});
    CK_CHECK(candidates->probe(0)->lookup().value()->state()==ChildLookupState::Missing && candidates->probe(1)->lookup().value()->state()==ChildLookupState::Missing);
    DestinationNamesJob job(candidates);const auto result=finish(job);
    CK_CHECK(result->summary().groups==2 && result->summary().duplicate_groups==1 && result->summary().rows_in_duplicate_groups==2);
    CK_CHECK(take(result->compare(0,2)).relation==ComponentNameRelation::IdenticalNativeUnits);
    if(result->rule().value()){
        CK_CHECK(take(result->compare(0,1)).relation==ComponentNameRelation::DistinctUnderReportedRules);
        std::cout<<"native-destination-names: qualified distinct missing groups\n";
    }else CK_CHECK(!result->compare(0,1).value() && !take(result->row(0)).against_other_groups.value());
    CK_CHECK(fs::is_empty(scratch.path()));
}
CK_TEST(native_destination_names_keeps_hard_link_object_aliases_separate_from_component_groups) {
    const ck::testing::ScratchDirectory scratch("files-destination-name-links");NativeFileSystem filesystem;
    {std::ofstream output(scratch.path()/"file");output<<"fixture";output.close();if(!output)throw std::runtime_error("cannot create native hard-link fixture");}
    fs::create_hard_link(scratch.path()/"file",scratch.path()/"alias");
    auto candidates=batch(std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(scratch.path()))),{name("file"),name("alias")});
    CK_CHECK(candidates->reported_object_leader(0)==0 && candidates->reported_object_leader(1)==0);
    DestinationNamesJob job(candidates);const auto result=finish(job);
    CK_CHECK(result->summary().groups==2 && result->summary().duplicate_groups==0);
    if(result->rule().value())CK_CHECK(take(result->compare(0,1)).relation==ComponentNameRelation::DistinctUnderReportedRules);
    else CK_CHECK(!result->compare(0,1).value());
    CK_CHECK(fs::file_size(scratch.path()/"file")==7 && fs::file_size(scratch.path()/"alias")==7);
}
CK_TEST(native_destination_names_uses_the_batches_retained_parent_after_path_replacement_and_owns_its_lifetime) {
    const ck::testing::ScratchDirectory scratch("files-destination-names-parent");NativeFileSystem filesystem;
    const auto original=scratch.path()/"original",moved=scratch.path()/"moved";fs::create_directory(original);
    auto cursor=std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(original)));std::weak_ptr<const DirectoryCursor> weak=cursor;
    const auto id=take(cursor->identity());auto candidates=batch(cursor,{name("x"),name("y")});cursor.reset();
    fs::rename(original,moved);fs::create_directory(original);
    DestinationNamesJob job(std::move(candidates));auto result=finish(job);
    CK_CHECK(compare_identity(take(take(result->before()).directory),id)==IdentityComparison::Same);
    auto replacement=take(filesystem.open_directory(original));
    CK_CHECK(compare_identity(take(take(result->before()).directory),take(replacement->identity()))==IdentityComparison::Different);
    CK_CHECK(job.abandon() && !weak.expired());result.reset();CK_CHECK(weak.expired());
    CK_CHECK(fs::is_empty(original) && fs::is_empty(moved));
}
