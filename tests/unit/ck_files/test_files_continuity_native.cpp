// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "pane_model.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#include "windows_junction.hpp"
#endif

namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> const T& required(const Observation<T>& observed) {
    if (!observed.value()) {
        const auto& e=*observed.unavailable_detail();
        std::cerr<<"continuity observation missing: "<<static_cast<int>(e.state)<<" "<<e.error.operation<<" "<<e.error.code<<std::endl;
        throw std::runtime_error("native continuity observation missing");
    }
    return *observed.value();
}
NativeName native_name(const fs::path& basename) {
#if defined(_WIN32)
    std::u16string units;for(const auto unit:basename.native())units.push_back(static_cast<char16_t>(unit));
    const auto name=NativeName::from_windows(std::move(units));
#else
    const auto name=NativeName::from_posix(basename.native());
#endif
    if(!name)throw std::runtime_error("invalid native continuity fixture name");
    return *name;
}
void write(const fs::path& path, const std::string& data="original") {
    std::ofstream stream(path,std::ios::binary);stream<<data;stream.close();
    if(!stream)throw std::runtime_error("cannot write continuity fixture");
}
std::shared_ptr<const DirectoryCursor> open_parent(const NativeFileSystem& filesystem,const fs::path& path) {
    auto opened=filesystem.open_directory(path);(void)required(opened);
    return std::shared_ptr<const DirectoryCursor>(std::move(opened).take_value());
}
PaneEntry row(HeldChild::Pointer child) { return PaneEntry::from_held(std::move(child),Observation<bool>::available(false)); }
void publish(PaneModel& pane,PaneListingPurpose purpose,std::vector<PaneEntry> entries) {
    if(pane.publish(pane.begin_listing(purpose),entries)!=PanePublish::Applied)throw std::runtime_error("native continuity publication failed");
}
PaneRowId first(const PaneModel& pane) {
    if(!pane.visible_row(0))throw std::runtime_error("missing native continuity row");
    return pane.visible_row(0)->id;
}
}
CK_TEST(native_continuity_refresh_preserves_selection_and_updates_metadata_through_held_objects) {
    const ck::testing::ScratchDirectory scratch("files-continuity-stable");write(scratch.path()/"file");
    NativeFileSystem filesystem;auto parent=open_parent(filesystem,scratch.path());
    auto before=required(HeldChild::observe(parent,native_name("file")));PaneModel pane;publish(pane,PaneListingPurpose::Navigate,{row(before)});
    const auto handle=first(pane);pane.set_mark(handle,true);write(scratch.path()/"file","same object has a new length");
    auto after=required(before->refresh());CK_CHECK(after->continues(*before));publish(pane,PaneListingPurpose::Refresh,{row(after)});
    CK_CHECK(first(pane)==handle&&pane.focused()==handle&&pane.marked(handle)&&pane.sources().state==PaneSourcesState::Ready);
    CK_CHECK(required(pane.row(handle)->entry.metadata.logical_size)==28);
}
CK_TEST(native_continuity_same_name_replacement_invalidates_marks_while_old_object_is_held) {
    const ck::testing::ScratchDirectory scratch("files-continuity-replace");write(scratch.path()/"file");
    NativeFileSystem filesystem;auto before=required(HeldChild::observe(open_parent(filesystem,scratch.path()),native_name("file")));
    PaneModel pane;publish(pane,PaneListingPurpose::Navigate,{row(before)});const auto old=first(pane);pane.set_mark(old,true);
    fs::rename(scratch.path()/"file",scratch.path()/"retired");write(scratch.path()/"file","replacement");fs::remove(scratch.path()/"retired");
    auto after=required(before->refresh());CK_CHECK(!after->continues(*before)&&required(after->previous_continuity())==IdentityComparison::Different);
    publish(pane,PaneListingPurpose::Refresh,{row(after)});
    CK_CHECK(!pane.row(old)&&pane.sources().state==PaneSourcesState::SelectionInvalidated&&pane.invalidated_selection().marks==1);
    CK_CHECK(compare_identity(required(before->metadata().identity),required(after->metadata().identity))==IdentityComparison::Different);
}
#if !defined(_WIN32)
CK_TEST(native_continuity_parent_path_replacement_does_not_retarget_refresh) {
    const ck::testing::ScratchDirectory scratch("files-continuity-parent");const auto parent_path=scratch.path()/"parent",retired=scratch.path()/"retired";
    fs::create_directory(parent_path);write(parent_path/"file");NativeFileSystem filesystem;
    auto parent=open_parent(filesystem,parent_path);auto before=required(HeldChild::observe(parent,native_name("file")));parent.reset();
    std::error_code rename_error;
    fs::rename(parent_path,retired,rename_error);
    if(rename_error){std::cerr<<"continuity parent rename: "<<rename_error.value()<<" "<<rename_error.message()<<std::endl;throw std::system_error(rename_error,"parent rename fixture");}
    fs::create_directory(parent_path);write(parent_path/"file","replacement in different parent");
    const auto after=required(before->refresh());CK_CHECK(after->continues(*before)&&required(after->metadata().logical_size)==8);
    const auto replacement=required(filesystem.inspect_basic(parent_path/"file"));
    CK_CHECK(compare_identity(required(after->metadata().identity),required(replacement.identity))==IdentityComparison::Different);
}
#else
CK_TEST(native_continuity_windows_open_child_blocks_parent_rename_without_losing_the_hold) {
    const ck::testing::ScratchDirectory scratch("files-continuity-parent-limit");
    const auto path=scratch.path()/"parent",retired=scratch.path()/"retired";
    fs::create_directory(path);write(path/"file");NativeFileSystem filesystem;auto parent=open_parent(filesystem,path);
    {
        const auto before=required(HeldChild::observe(parent,native_name("file")));
        std::error_code error;fs::rename(path,retired,error);
        CK_CHECK(error.value()==ERROR_ACCESS_DENIED&&fs::exists(path)&&!fs::exists(retired));
        std::cout<<"Windows held-child parent rename refused: "<<error.value()<<" "<<error.message()<<std::endl;
        const auto after=required(before->refresh());CK_CHECK(after->continues(*before));
    }
    // This controlled fixture verifies the cause after both child observations
    // leave scope. Production code must not release a hold to retry a mutation.
    fs::rename(path,retired);CK_CHECK(fs::exists(retired/"file"));
}
#endif
CK_TEST(native_continuity_refresh_uses_a_held_parent_whose_path_was_replaced_before_child_observation) {
    const ck::testing::ScratchDirectory scratch("files-continuity-retired-parent");
    const auto path=scratch.path()/"parent",retired=scratch.path()/"retired";
    fs::create_directory(path);write(path/"file");NativeFileSystem filesystem;auto parent=open_parent(filesystem,path);
    fs::rename(path,retired);fs::create_directory(path);write(path/"file","replacement in different parent");
    const auto before=required(HeldChild::observe(parent,native_name("file")));const auto after=required(before->refresh());
    CK_CHECK(after->continues(*before)&&required(after->metadata().logical_size)==8);
    const auto replacement=required(filesystem.inspect_basic(path/"file"));
    CK_CHECK(compare_identity(required(after->metadata().identity),required(replacement.identity))==IdentityComparison::Different);
}
CK_TEST(native_continuity_keeps_hard_link_names_separate_and_observes_final_links_themselves) {
    const ck::testing::ScratchDirectory scratch("files-continuity-links");write(scratch.path()/"file");fs::create_hard_link(scratch.path()/"file",scratch.path()/"alias");
    NativeFileSystem filesystem;auto parent=open_parent(filesystem,scratch.path());
    const auto file=required(HeldChild::observe(parent,native_name("file"))),alias=required(HeldChild::observe(parent,native_name("alias")));
    CK_CHECK(compare_identity(required(file->metadata().identity),required(alias->metadata().identity))==IdentityComparison::Same);
    CK_CHECK(!alias->continues(*file));
#if defined(_WIN32)
    fs::create_directory(scratch.path()/"target");fs::create_directory(scratch.path()/"link");ck::testing::make_junction(scratch.path()/"link",scratch.path()/"target");
    constexpr auto expected=ObjectKind::Junction;
#else
    fs::create_symlink("absent",scratch.path()/"link");constexpr auto expected=ObjectKind::SymbolicLink;
#endif
    const auto link=required(HeldChild::observe(parent,native_name("link")));const auto refreshed=required(link->refresh());
    CK_CHECK(required(link->metadata().kind)==expected&&required(refreshed->metadata().kind)==expected&&refreshed->continues(*link));
}
