// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_model.hpp"
#include "cvision/testing/cktest.hpp"
#include <map>
#include <stdexcept>

namespace {
using namespace ck::files;
static_assert(!std::is_copy_constructible_v<HeldChild> && !std::is_copy_assignable_v<HeldChild>);
static_assert(!std::is_move_constructible_v<HeldChild> && !std::is_move_assignable_v<HeldChild>);
template<class T> const T& required(const Observation<T>& observed) {
    if (!observed.value()) throw std::runtime_error("required continuity observation missing");
    return *observed.value();
}
NativeName name(std::string value) { return *NativeName::from_posix(std::move(value)); }
struct Object {
    BasicMetadata metadata;
    std::optional<UnavailableObservation> failure;
    int reads = 0;
    int fail_at = -1;
    int live_handles = 0;
};
class Handle final : public OpenedEntry {
public:
    explicit Handle(std::shared_ptr<Object> object) : object_(std::move(object)) { ++object_->live_handles; }
    ~Handle() override { --object_->live_handles; }
    Observation<BasicMetadata> inspect_basic() const override {
        ++object_->reads;
        if (object_->failure && (object_->fail_at < 0 || object_->reads == object_->fail_at))
            return Observation<BasicMetadata>::unavailable(object_->failure->state, object_->failure->error);
        return Observation<BasicMetadata>::available(object_->metadata);
    }
private: std::shared_ptr<Object> object_;
};
class Parent final : public DirectoryCursor {
public:
    AdapterScope adapter;
    std::map<std::string,std::shared_ptr<Object>> entries;
    bool null_child = false;
    mutable int opens = 0;
    std::shared_ptr<Object> add(const std::string& basename, std::uint64_t id) {
        auto object = std::make_shared<Object>();
        object->metadata.identity = Observation<ObjectIdentity>::available({adapter,IdentityScheme::PosixDeviceInode,NativeIdentifier::from_u64(7),NativeIdentifier::from_u64(id),{}});
        object->metadata.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        object->metadata.logical_size = Observation<std::uint64_t>::available(12);
        entries[basename] = object;
        return object;
    }
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& basename, EntryAccess access) const override {
        using Result = Observation<std::unique_ptr<OpenedEntry>>;
        ++opens;
        if (access != EntryAccess::MetadataOnly) throw std::runtime_error("continuity requested excessive access");
        if (null_child) return Result::available(nullptr);
        const auto found = entries.find(*basename.posix_bytes());
        if (found == entries.end()) return Result::unavailable(ObservationState::Gone,{ErrorDomain::Posix,2,"script.open_child"});
        return Result::available(std::make_unique<Handle>(found->second));
    }
protected:
    Observation<DirectoryStep> next_step() override { return Observation<DirectoryStep>::available(DirectoryEnd{}); }
};
PaneEntry row(HeldChild::Pointer child, bool hidden = false) { return PaneEntry::from_held(std::move(child),Observation<bool>::available(hidden)); }
HeldChild::Pointer observe(const std::shared_ptr<Parent>& parent, const std::string& basename) { return required(HeldChild::observe(parent,name(basename))); }
PaneRowId first(const PaneModel& pane) {
    if (!pane.visible_row(0)) throw std::runtime_error("missing continuity pane row");
    return pane.visible_row(0)->id;
}
void publish(PaneModel& pane, PaneListingPurpose purpose, std::vector<PaneEntry> entries) {
    if (pane.publish(pane.begin_listing(purpose),entries) != PanePublish::Applied) throw std::runtime_error("continuity publication failed");
}
}
CK_TEST(held_child_rejects_null_and_missing_objects_and_preserves_native_failures) {
    auto parent=std::make_shared<Parent>();
    CK_CHECK(HeldChild::observe(nullptr,name("a")).state()==ObservationState::Failed);
    const auto missing=HeldChild::observe(parent,name("a"));CK_CHECK(missing.state()==ObservationState::Gone&&missing.unavailable_detail()->error.code==2);
    auto object=parent->add("a",1);object->failure=UnavailableObservation{ObservationState::Denied,{ErrorDomain::Posix,13,"script.inspect"}};
    const auto denied=HeldChild::observe(parent,name("a"));CK_CHECK(denied.state()==ObservationState::Denied&&denied.unavailable_detail()->error==object->failure->error&&object->live_handles==0);
    parent->null_child=true;CK_CHECK(HeldChild::observe(parent,name("a")).state()==ObservationState::Failed);
}
CK_TEST(held_child_continuity_requires_a_live_successor_not_cached_or_independent_id_matches) {
    auto parent=std::make_shared<Parent>();auto object=parent->add("a",1);
    auto original=observe(parent,"a");auto independent=observe(parent,"a");
    CK_CHECK(!original->continues(*original)&&!independent->continues(*original));
    auto next=required(original->refresh());CK_CHECK(next->continues(*original)&&!original->continues(*next));
    CK_CHECK(required(next->previous_continuity())==IdentityComparison::Same&&object->live_handles==3);
    auto later=required(next->refresh());CK_CHECK(later->continues(*next)&&later->continues(*original));
    original.reset();next.reset();independent.reset();CK_CHECK(object->live_handles==1);
}
CK_TEST(held_child_replacement_keeps_old_object_alive_and_starts_a_distinct_family) {
    auto parent=std::make_shared<Parent>();auto old=parent->add("a",1);auto before=observe(parent,"a");
    auto replacement=parent->add("a",2);auto after=required(before->refresh());
    CK_CHECK(!after->continues(*before)&&required(after->previous_continuity())==IdentityComparison::Different);
    CK_CHECK(old->live_handles==1&&replacement->live_handles==1);
    parent->entries.erase("a");CK_CHECK(before->refresh().state()==ObservationState::Gone&&old->live_handles==1);
    before.reset();CK_CHECK(old->live_handles==0);
}
CK_TEST(held_child_unknown_identity_and_changed_observation_never_claim_continuity) {
    auto parent=std::make_shared<Parent>();auto object=parent->add("a",1);
    object->metadata.identity={};auto old=observe(parent,"a");auto next=required(old->refresh());
    CK_CHECK(!next->continues(*old)&&next->previous_continuity().state()==ObservationState::Unknown);
    object->metadata.identity=Observation<ObjectIdentity>::available({parent->adapter,IdentityScheme::PosixDeviceInode,NativeIdentifier::from_u64(7),NativeIdentifier::from_u64(1),{}});
    next=required(old->refresh());CK_CHECK(!next->continues(*old));
    old=observe(parent,"a");object->metadata.kind=Observation<ObjectKind>::available(ObjectKind::Directory);
    next=required(old->refresh());CK_CHECK(!next->continues(*old)&&next->previous_continuity().state()==ObservationState::Changed);
}
CK_TEST(held_child_old_handle_query_failure_is_reported_without_discarding_the_fresh_observation) {
    auto parent=std::make_shared<Parent>();auto object=parent->add("a",1);auto old=observe(parent,"a");
    object->failure=UnavailableObservation{ObservationState::Denied,{ErrorDomain::Posix,13,"old.inspect"}};object->fail_at=3;
    const auto next=required(old->refresh());
    CK_CHECK(!next->continues(*old)&&next->previous_continuity().state()==ObservationState::Denied);
    CK_CHECK(next->previous_continuity().unavailable_detail()->error==object->failure->error&&required(next->metadata().logical_size)==12);
}
CK_TEST(pane_refresh_retains_qualified_row_handles_marks_focus_and_current_metadata) {
    auto parent=std::make_shared<Parent>();auto object=parent->add("a",1);auto before=observe(parent,"a");
    PaneModel pane;publish(pane,PaneListingPurpose::Navigate,{row(before)});const auto handle=first(pane);pane.set_mark(handle,true);
    object->metadata.logical_size=Observation<std::uint64_t>::available(44);auto after=required(before->refresh());
    publish(pane,PaneListingPurpose::Refresh,{row(after)});
    CK_CHECK(first(pane)==handle&&pane.marked(handle)&&pane.focused()==handle&&pane.sources().state==PaneSourcesState::Ready);
    CK_CHECK(required(pane.row(handle)->entry.metadata.logical_size)==44&&pane.invalidated_selection().marks==0);
    CK_CHECK(parent->opens==2); // publication itself performs no native I/O
}
CK_TEST(pane_cached_or_independent_held_snapshots_cannot_retain_stale_sources) {
    auto parent=std::make_shared<Parent>();parent->add("a",1);auto before=observe(parent,"a");PaneModel pane;
    for(bool cached:{true,false}){
        publish(pane,PaneListingPurpose::Navigate,{row(before)});const auto old=first(pane);pane.set_mark(old,true);
        publish(pane,PaneListingPurpose::Refresh,{row(cached?before:observe(parent,"a"))});
        CK_CHECK(!pane.row(old)&&pane.sources().state==PaneSourcesState::SelectionInvalidated&&pane.invalidated_selection().marks==1);
    }
}
CK_TEST(pane_same_name_replacement_invalidates_old_sources_even_while_other_rows_continue) {
    auto parent=std::make_shared<Parent>();parent->add("a",1);parent->add("b",2);
    auto a=observe(parent,"a"),b=observe(parent,"b");PaneModel pane;publish(pane,PaneListingPurpose::Navigate,{row(a),row(b)});
    const auto old_a=first(pane),old_b=pane.visible_row(1)->id;pane.mark_visible(true);parent->add("a",3);
    publish(pane,PaneListingPurpose::Refresh,{row(required(a->refresh())),row(required(b->refresh()))});
    CK_CHECK(!pane.row(old_a)&&pane.row(old_b)&&pane.marked(old_b)&&pane.invalidated_selection().marks==1);
    CK_CHECK(pane.sources().state==PaneSourcesState::SelectionInvalidated&&!pane.focused());
}
CK_TEST(pane_continued_hidden_marks_still_require_review_and_hard_link_names_stay_distinct) {
    auto parent=std::make_shared<Parent>();auto object=parent->add("a",1);parent->entries["b"]=object;
    auto a=observe(parent,"a"),b=observe(parent,"b");PaneModel pane;publish(pane,PaneListingPurpose::Navigate,{row(a),row(b)});
    const auto old_a=first(pane),old_b=pane.visible_row(1)->id;pane.mark_visible(true);
    publish(pane,PaneListingPurpose::Refresh,{row(required(b->refresh())),row(required(a->refresh()),true)});
    const auto sources=pane.sources();CK_CHECK(pane.row(old_a)&&pane.row(old_b)&&sources.candidates.size()==2);
    CK_CHECK(sources.state==PaneSourcesState::HiddenMarksNeedReview&&sources.hidden_count==1&&sources.visible_count==1);
}
CK_TEST(pane_held_binding_canonicalizes_metadata_rejects_name_substitution_and_checks_held_limit) {
    auto parent=std::make_shared<Parent>();parent->add("a",1);parent->add("b",2);
    auto a=observe(parent,"a"),b=observe(parent,"b");PaneModel pane({10,100,100,1});
    const auto request=pane.begin_listing(PaneListingPurpose::Navigate);
    CK_CHECK(pane.publish(request,{row(a),row(b)})==PanePublish::LimitExceeded&&pane.size()==0);
    auto forged=row(a);forged.name=name("other");CK_CHECK(pane.publish(request,{forged})==PanePublish::InvalidListing);
    auto changed=row(a);changed.metadata.logical_size=Observation<std::uint64_t>::available(9999);
    CK_CHECK(pane.publish(request,{changed})==PanePublish::Applied&&required(pane.visible_row(0)->entry.metadata.logical_size)==12);
}
CK_TEST(all_zero_native_object_ids_are_unqualified_even_when_the_query_reports_success) {
    AdapterScope scope;
    for(auto scheme:{IdentityScheme::PosixDeviceInode,IdentityScheme::WindowsFileId128,IdentityScheme::WindowsFileIndex64}){
        ObjectIdentity zero{scope,scheme,NativeIdentifier::from_u64(7),{},std::nullopt};
        CK_CHECK(compare_identity(zero,zero)==IdentityComparison::Unknown);
        auto nonzero=zero;nonzero.object=NativeIdentifier::from_u64(1);CK_CHECK(compare_identity(zero,nonzero)==IdentityComparison::Unknown);
    }
}
