// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "name_mapping.hpp"
#include "windows_name.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace ck::files;
namespace fs=std::filesystem;
template<class T> const T& required(const Observation<T>& value) {
    if(!value.value()){
        const auto& e=*value.unavailable_detail();std::cerr<<"native name mapping observation: "<<static_cast<int>(e.state)<<" "<<e.error.operation<<" "<<e.error.code<<std::endl;
        throw std::runtime_error("native name mapping observation missing");
    }
    return *value.value();
}
constexpr NameEncoding native_encoding() {
#if defined(_WIN32)
    return NameEncoding::WindowsUtf16;
#else
    return NameEncoding::PosixBytes;
#endif
}
fs::path component(const NativeName& name) {
#if defined(_WIN32)
    std::wstring units;for(auto unit:*name.windows_units())units.push_back(static_cast<wchar_t>(unit));return fs::path(units);
#else
    return fs::path(*name.posix_bytes());
#endif
}
void write(const fs::path& path) {
    std::ofstream file(path,std::ios::binary);file<<"fixture";file.close();if(!file)throw std::runtime_error("cannot write native name fixture");
}
std::unique_ptr<DirectoryCursor> parent(const NativeFileSystem& filesystem,const fs::path& path) {
    auto observed=filesystem.open_directory(path);(void)required(observed);return std::move(observed).take_value();
}
void check_enumerated(const NativeFileSystem& filesystem,const fs::path& path,const NativeName& expected) {
    auto cursor=parent(filesystem,path);bool found=false,ended=false;
    for(int batch=0;batch<16;++batch) {
        const auto observed=cursor->read(16);
        if(observed.state==DirectoryReadState::Error||observed.state==DirectoryReadState::Cancelled)
            throw std::runtime_error("cannot enumerate private name fixture");
        for(const auto& name:observed.names)if(name==expected)found=true;
        if(observed.state==DirectoryReadState::End){ended=true;break;}
    }
    CK_CHECK(ended&&found); // compare native bytes/units, never escaped display
}

void check_child(const DirectoryCursor& directory,const NativeName& name) {
    const auto opened=directory.open_child(name);const auto metadata=required(opened)->inspect_basic();
    CK_CHECK(required(required(metadata).kind)==ObjectKind::RegularFile&&required(required(metadata).logical_size)==7);
}
}
CK_TEST(native_mapped_unicode_component_is_queried_without_locale_or_display_round_trip) {
    const ck::testing::ScratchDirectory scratch("files-name-unicode");
    const auto source=*NativeName::from_windows(u"portable-\u65e5\u672c-\U0001f642");
    const auto mapped=required(map_native_name(source,native_encoding()));write(scratch.path()/component(mapped));
    NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());check_child(*directory,mapped);check_enumerated(filesystem,scratch.path(),mapped);
    CK_CHECK(required(map_native_name(mapped,NameEncoding::WindowsUtf16))==source);
}
CK_TEST(native_same_encoding_preserves_unusual_file_components_without_rewriting) {
    const ck::testing::ScratchDirectory scratch("files-name-raw");
#if defined(_WIN32)
    const auto source=*NativeName::from_windows(std::u16string{u'r',u'a',u'w',u'-',0xd800});
#elif defined(__linux__)
    const auto source=*NativeName::from_posix("raw-\xff\\\n");
#else
    const auto source=*NativeName::from_posix("raw-\\\n"); // APFS does not accept arbitrary non-UTF8 entry names
#endif
    const auto mapped=required(map_native_name(source,native_encoding()));CK_CHECK(mapped==source);
    write(scratch.path()/component(mapped));NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());check_child(*directory,mapped);check_enumerated(filesystem,scratch.path(),mapped);
    auto moving=mapped;const auto owned=std::move(moving);CK_CHECK(owned.is_valid());
    if(!moving.is_valid())CK_CHECK(directory->open_child(moving).state()==ObservationState::Failed);
}
CK_TEST(native_stream_syntax_is_not_confused_with_an_ordinary_windows_file_component) {
    const ck::testing::ScratchDirectory scratch("files-name-colon");NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());
    const auto mapped=required(map_native_name(*NativeName::from_posix("owner:stream"),NameEncoding::WindowsUtf16));
    CK_CHECK(check_win32_component(*mapped.windows_units()).has(Win32NameIssue::ForbiddenCharacter));
#if defined(_WIN32)
    write(scratch.path()/"owner");const auto refused=directory->open_child(mapped);
    CK_CHECK(refused.state()==ObservationState::Failed&&refused.unavailable_detail()->error.code==static_cast<std::int64_t>(AdapterError::InvalidPath));
    check_child(*directory,*NativeName::from_windows(u"owner"));
#else
    const auto native=required(map_native_name(mapped,NameEncoding::PosixBytes));write(scratch.path()/component(native));check_child(*directory,native);check_enumerated(filesystem,scratch.path(),native);
#endif
}
CK_TEST(native_literal_names_remain_identifiable_even_when_win32_conventions_flag_them) {
    const ck::testing::ScratchDirectory scratch("files-name-literal");NativeFileSystem filesystem;auto directory=parent(filesystem,scratch.path());
    for(const auto units:{u"CON",u"trailing."}) {
        const auto windows=*NativeName::from_windows(units);CK_CHECK(!check_win32_component(*windows.windows_units()).ordinary_component());
        const auto native=required(map_native_name(windows,native_encoding()));
#if defined(_WIN32)
        auto path=scratch.path()/component(native);path.make_preferred();const auto extended=std::wstring(L"\\\\?\\")+path.native();
        struct Owner{HANDLE value;~Owner(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}} handle{
            CreateFileW(extended.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr)};
        if(handle.value==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot create private literal name: "+std::to_string(GetLastError()));
        DWORD written=0;if(!WriteFile(handle.value,"fixture",7,&written,nullptr)||written!=7)throw std::runtime_error("cannot write private literal name");
#else
        write(scratch.path()/component(native));
#endif
        // Windows uses the existing counted-relative NT inspection boundary,
        // never a Win32 DOS-device spelling. Mapping/diagnostics grant no write.
        check_child(*directory,native);check_enumerated(filesystem,scratch.path(),native);
#if defined(_WIN32)
        if(!DeleteFileW(extended.c_str()))throw std::runtime_error("cannot remove private literal name: "+std::to_string(GetLastError()));
#endif
    }
}
