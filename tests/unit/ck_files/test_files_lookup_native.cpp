// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "name_mapping.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include <iostream>
#include <cerrno>
#include <utility>
#include <stdexcept>
#if defined(_WIN32)
#include "windows_junction.hpp"
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
using namespace ck::files;namespace fs=std::filesystem;
template<class T>const T& required(const Observation<T>& value){
    if(!value.value()){const auto& e=*value.unavailable_detail();std::cerr<<"lookup observation: "<<static_cast<int>(e.state)<<" "<<e.error.operation<<" "<<e.error.code<<std::endl;throw std::runtime_error("native lookup observation missing");}return *value.value();
}
NativeName name(std::u16string units){
    auto windows=NativeName::from_windows(std::move(units));if(!windows)throw std::runtime_error("bad native lookup fixture name");
#if defined(_WIN32)
    return *windows;
#else
    return required(map_native_name(*windows,NameEncoding::PosixBytes));
#endif
}
fs::path path_component(const NativeName& component){
#if defined(_WIN32)
    std::wstring text;for(auto unit:*component.windows_units())text.push_back(static_cast<wchar_t>(unit));return fs::path(text);
#else
    return fs::path(*component.posix_bytes());
#endif
}
void create(const fs::path& path,const std::string& bytes="fixture"){
#if defined(_WIN32)
    struct Owner{HANDLE handle;~Owner(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}} owner{CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if(owner.handle==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot exclusively create lookup fixture: "+std::to_string(GetLastError()));
    DWORD written=0;if(!WriteFile(owner.handle,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size())throw std::runtime_error("cannot write lookup fixture");
#else
    struct Owner{int fd;~Owner(){if(fd>=0)close(fd);}} owner{open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600)};
    if(owner.fd<0)throw std::runtime_error("cannot exclusively create lookup fixture");
    std::size_t at=0;while(at<bytes.size()){const auto count=write(owner.fd,bytes.data()+at,bytes.size()-at);if(count<0&&errno==EINTR)continue;if(count<=0)throw std::runtime_error("cannot write lookup fixture");at+=static_cast<std::size_t>(count);}
#endif
}
std::unique_ptr<DirectoryCursor> parent(const NativeFileSystem& filesystem,const fs::path& path){auto result=filesystem.open_directory(path);(void)required(result);return std::move(result).take_value();}
ChildLookup query(const DirectoryCursor& directory,const NativeName& component,ChildLookupMode mode=ChildLookupMode::NativeDefault){auto result=directory.lookup_child(component,mode);(void)required(result);return std::move(result).take_value();}
const OpenedEntry& occupant(const ChildLookup& lookup){if(lookup.state()!=ChildLookupState::Found||!lookup.entry())throw std::runtime_error("expected native lookup occupant");return *lookup.entry();}
BasicMetadata metadata(const ChildLookup& lookup){return required(occupant(lookup).inspect_basic());}
}
CK_TEST(native_lookup_distinguishes_held_occupants_and_evidenced_absence_without_reserving_names){
    const ck::testing::ScratchDirectory scratch("files-lookup-basic");NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());const auto component=name(u"slot");
    const auto absent=query(*directory,component);CK_CHECK(absent.state()==ChildLookupState::Missing&&absent.missing_witness());
    if(!absent.context()||!absent.missing_witness())throw std::runtime_error("missing lookup context/evidence");
    CK_CHECK(absent.context()->requested==component&&absent.context()->mode==ChildLookupMode::NativeDefault);
    CK_CHECK(compare_identity(required(absent.context()->directory),required(directory->identity()))==IdentityComparison::Same);
#if defined(_WIN32)
    CK_CHECK(absent.missing_witness()->domain==ErrorDomain::NtStatus);
#else
    CK_CHECK(absent.missing_witness()->domain==ErrorDomain::Posix&&absent.missing_witness()->code==ENOENT);
#endif
    CK_CHECK(!fs::exists(scratch.path()/path_component(component)));
    create(scratch.path()/path_component(component));const auto present=query(*directory,component);
    CK_CHECK(required(metadata(present).kind)==ObjectKind::RegularFile&&required(metadata(present).logical_size)==7);
    CK_CHECK(absent.state()==ChildLookupState::Missing); // an observation, never a reservation
}
CK_TEST(native_lookup_stays_with_the_held_parent_after_its_path_is_replaced){
    const ck::testing::ScratchDirectory scratch("files-lookup-parent");const auto path=scratch.path()/"parent",retired=scratch.path()/"retired";
    fs::create_directory(path);create(path/"file");NativeFileSystem filesystem;auto directory=parent(filesystem,path);
    fs::rename(path,retired);fs::create_directory(path);create(path/"file","replacement");create(path/"only-new");
    const auto found=query(*directory,name(u"file"));CK_CHECK(required(metadata(found).logical_size)==7);
    CK_CHECK(query(*directory,name(u"only-new")).state()==ChildLookupState::Missing);
    CK_CHECK(compare_identity(required(metadata(found).identity),required(required(filesystem.inspect_basic(path/"file")).identity))==IdentityComparison::Different);
}
CK_TEST(native_lookup_occupant_survives_parent_destruction_rename_replacement_and_unlink){
    const ck::testing::ScratchDirectory scratch("files-lookup-owner");create(scratch.path()/"file");NativeFileSystem filesystem;ChildLookup found;
    {auto directory=parent(filesystem,scratch.path());found=query(*directory,name(u"file"));}
    const auto before=metadata(found);fs::rename(scratch.path()/"file",scratch.path()/"retired");create(scratch.path()/"file","replacement");fs::remove(scratch.path()/"retired");
    CK_CHECK(required(metadata(found).logical_size)==7&&compare_identity(required(before.identity),required(metadata(found).identity))==IdentityComparison::Same);
    CK_CHECK(compare_identity(required(before.identity),required(required(filesystem.inspect_basic(scratch.path()/"file")).identity))==IdentityComparison::Different);
}
CK_TEST(native_lookup_does_not_follow_final_links_or_treat_dangling_links_as_missing){
    const ck::testing::ScratchDirectory scratch("files-lookup-link");
#if defined(_WIN32)
    fs::create_directory(scratch.path()/"link");ck::testing::make_junction(scratch.path()/"link",scratch.path()/"absent");constexpr auto kind=ObjectKind::Junction;
#else
    fs::create_symlink("absent",scratch.path()/"link");constexpr auto kind=ObjectKind::SymbolicLink;
#endif
    NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());const auto found=query(*directory,name(u"link"));
    CK_CHECK(required(metadata(found).kind)==kind&&query(*directory,name(u"absent")).state()==ChildLookupState::Missing);
}
CK_TEST(native_lookup_same_object_does_not_conflate_hard_link_names_with_namespace_aliases){
    const ck::testing::ScratchDirectory scratch("files-lookup-hardlinks");create(scratch.path()/"file");fs::create_hard_link(scratch.path()/"file",scratch.path()/"alias");
    NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());const auto first=query(*directory,name(u"file")),second=query(*directory,name(u"alias"));
    CK_CHECK(compare_identity(required(metadata(first).identity),required(metadata(second).identity))==IdentityComparison::Same);
    if(!first.context()||!second.context())throw std::runtime_error("missing hard-link contexts");
    CK_CHECK(first.context()->requested!=second.context()->requested);
}
CK_TEST(native_lookup_case_and_normalization_resolution_matches_an_independent_native_query){
    const ck::testing::ScratchDirectory scratch("files-lookup-comparison");NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());
    for(const auto& pair:{std::pair<std::u16string,std::u16string>{u"CaseProbe",u"caseprobe"},{u"norm-\u00e9",u"norm-e\u0301"}}){
        const auto original=name(pair.first),alternate=name(pair.second);create(scratch.path()/path_component(original));
#if defined(_WIN32)
        constexpr auto mode=ChildLookupMode::WindowsIgnoreCase;
#else
        constexpr auto mode=ChildLookupMode::NativeDefault;
#endif
        const auto looked=query(*directory,alternate,mode);std::error_code error;const auto independent=fs::symlink_status(scratch.path()/path_component(alternate),error);
        if(independent.type()==fs::file_type::not_found){CK_CHECK(looked.state()==ChildLookupState::Missing);}
        else{if(error)throw std::system_error(error,"independent lookup fixture query");CK_CHECK(looked.state()==ChildLookupState::Found);CK_CHECK(compare_identity(required(metadata(looked).identity),required(required(filesystem.inspect_basic(scratch.path()/path_component(original))).identity))==IdentityComparison::Same);}
        if(!looked.context())throw std::runtime_error("missing comparison context");
        CK_CHECK(looked.context()->requested==alternate&&looked.context()->mode==mode);
        std::cout<<"native lookup alternate "<<alternate.display_name()<<" mode "<<static_cast<int>(mode)<<" state "<<static_cast<int>(looked.state())<<std::endl;
    }
}
CK_TEST(native_lookup_checks_explicit_policy_encoding_and_stream_boundaries){
    const ck::testing::ScratchDirectory scratch("files-lookup-policy");create(scratch.path()/"CaseProbe");NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());
    CK_CHECK(directory->lookup_child(name(u"CaseProbe"),static_cast<ChildLookupMode>(999)).state()==ObservationState::Failed);
#if defined(_WIN32)
    const auto ignored=query(*directory,name(u"caseprobe"),ChildLookupMode::WindowsIgnoreCase);CK_CHECK(ignored.state()==ChildLookupState::Found);
    CK_CHECK(directory->lookup_child(name(u"CaseProbe:stream"),ChildLookupMode::WindowsIgnoreCase).state()==ObservationState::Failed);
    CK_CHECK(directory->lookup_child(*NativeName::from_posix("CaseProbe"),ChildLookupMode::NativeDefault).state()==ObservationState::Failed);
#else
    CK_CHECK(directory->lookup_child(name(u"caseprobe"),ChildLookupMode::WindowsIgnoreCase).state()==ObservationState::Unsupported);
    CK_CHECK(directory->lookup_child(*NativeName::from_windows(u"CaseProbe"),ChildLookupMode::NativeDefault).state()==ObservationState::Failed);
#endif
}
