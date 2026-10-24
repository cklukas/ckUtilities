// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "directory_name_rules.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#ifdef __linux__
#include <cerrno>
#include <unistd.h>
#endif
namespace {
using namespace ck::files;
namespace fs=std::filesystem;
template<class T>T take(Observation<T> value){if(!value.value())throw std::runtime_error("required native fscrypt observation unavailable");return std::move(value).take_value();}
}
CK_TEST(native_fscrypt_query_preserves_absence_or_unavailability_without_mutation_or_enumeration_changes) {
    const ck::testing::ScratchDirectory scratch("files-fscrypt");NativeFileSystem filesystem;
    fs::create_directory(scratch.path()/"a");fs::create_directory(scratch.path()/"b");auto cursor=take(filesystem.open_directory(scratch.path()));
    const auto first=cursor->read(1);CK_CHECK(first.names.size()==1);const auto report=cursor->inspect_fscrypt_policy();
#ifdef __linux__
    if(report.value()) {
        const auto* absent=std::get_if<FscryptPolicyAbsent>(&report.value()->policy);CK_CHECK(absent && absent->witness.domain==ErrorDomain::Posix && absent->witness.code==ENODATA);
        CK_CHECK(compare_identity(report.value()->directory,take(cursor->identity()))==IdentityComparison::Same);
        std::cout<<"native-fscrypt: ENODATA absence qualified\n";
    } else CK_CHECK(report.state()==ObservationState::Unsupported && report.unavailable_detail()->error.domain==ErrorDomain::Posix);
#else
    CK_CHECK(report.state()==ObservationState::Unsupported && report.unavailable_detail()->error.operation=="directory.fscrypt_policy");
#endif
    const auto rest=cursor->read(10);CK_CHECK(rest.state==DirectoryReadState::End && rest.names.size()==1 && rest.names[0]!=first.names[0]);
    CK_CHECK(std::distance(fs::directory_iterator(scratch.path()),fs::directory_iterator{})==2);
}
CK_TEST(native_fscrypt_query_uses_the_held_directory_after_original_path_replacement) {
    const ck::testing::ScratchDirectory scratch("files-fscrypt-held");NativeFileSystem filesystem;
    const auto original=scratch.path()/"original",moved=scratch.path()/"moved";fs::create_directory(original);auto cursor=take(filesystem.open_directory(original));
    const auto id=take(cursor->identity());fs::rename(original,moved);fs::create_directory(original);
    const auto report=cursor->inspect_fscrypt_policy();if(report.value())CK_CHECK(compare_identity(report.value()->directory,id)==IdentityComparison::Same);
    else CK_CHECK(report.state()==ObservationState::Unsupported);
    CK_CHECK(fs::is_empty(original) && fs::is_empty(moved));
}
#ifdef __linux__
CK_TEST(native_fscrypt_locked_v2_policy_fixture_preserves_bytes_identity_and_no_key_requirement) {
    const char* configured=std::getenv("CKTOOLS_FSCRYPT_FIXTURE");
    if(!configured){std::cout<<"native-fscrypt: locked-policy fixture not configured for this run\n";return;}
    CK_CHECK(geteuid()!=0);const fs::path root(configured);
    const auto expected=ck::testing::scratch_root().parent_path().parent_path()/"fscrypt-fixture-20261024/mount";
    if(!root.is_absolute() || root.lexically_normal()!=expected || fs::is_symlink(fs::symlink_status(root)))
        throw std::runtime_error("fscrypt fixture must be the private task mount");
    NativeFileSystem filesystem;const auto encrypted=root/"encrypted",moved=root/"moved";
    auto cursor=std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(encrypted)));const auto id=take(cursor->identity());
    const auto report=take(cursor->inspect_fscrypt_policy());const auto* policy=std::get_if<FscryptPolicyData>(&report.policy);
    CK_CHECK(policy && policy->version()==2 && policy->contents_mode()==1 && policy->filenames_mode()==4 && policy->flags()==0 && policy->raw().size()==24);
    if(!policy)throw std::runtime_error("locked fixture policy missing");
    for(std::size_t i=0;i<16;++i)CK_CHECK(policy->key_reference()[i]==static_cast<std::byte>(0xa0+i));
    CK_CHECK(compare_identity(report.directory,id)==IdentityComparison::Same);
    const auto names=take(DirectoryNameRulesProbe::inspect(cursor));CK_CHECK(names->rule().state()==ObservationState::Unsupported);
    struct Restore {
        fs::path original, moved; bool renamed=false, replacement=false;
        ~Restore(){if(!renamed)return;std::error_code error;if(replacement)fs::remove(original,error);if(!fs::exists(original,error))fs::rename(moved,original,error);}
    } restore{encrypted,moved};
    if(fs::exists(moved))throw std::runtime_error("fscrypt fixture move target already exists");
    fs::rename(encrypted,moved);restore.renamed=true;fs::create_directory(encrypted);restore.replacement=true;
    const auto held=take(cursor->inspect_fscrypt_policy());const auto& held_policy=std::get<FscryptPolicyData>(held.policy);
    CK_CHECK(std::equal(held_policy.raw().begin(),held_policy.raw().end(),policy->raw().begin(),policy->raw().end()));
    auto replacement=take(filesystem.open_directory(encrypted));const auto plain=take(replacement->inspect_fscrypt_policy());
    CK_CHECK(std::holds_alternative<FscryptPolicyAbsent>(plain.policy) && compare_identity(plain.directory,id)==IdentityComparison::Different);
    CK_CHECK(fs::remove(encrypted));restore.replacement=false;fs::rename(moved,encrypted);restore.renamed=false;
    std::cout<<"native-fscrypt: locked v2 policy and replacement isolation qualified\n";
}
#endif
