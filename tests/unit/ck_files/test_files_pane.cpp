// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_model.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <stdexcept>

namespace {
using namespace ck::files;
PaneEntry entry(std::string name, std::optional<std::uint64_t> size = {}, bool hidden = false) {
    BasicMetadata metadata;
    metadata.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
    if (size) metadata.logical_size = Observation<std::uint64_t>::available(*size);
    return {*NativeName::from_posix(std::move(name)), std::move(metadata), Observation<bool>::available(hidden)};
}
void load(PaneModel& pane, std::vector<PaneEntry> entries) {
    if (pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), entries) != PanePublish::Applied) throw std::runtime_error("pane fixture publication failed");
}
PaneRowId id(const PaneModel& pane, std::size_t index) {
    const auto* row = pane.visible_row(index);
    if (!row) throw std::runtime_error("missing pane fixture row");
    return row->id;
}
std::vector<std::string> names(const PaneModel& pane) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < pane.visible_size(); ++i) result.push_back(pane.visible_row(i)->display_name);
    return result;
}
}
CK_TEST(pane_rejects_stale_and_foreign_publications_without_changing_the_listing) {
    PaneModel pane, other;
    load(pane, {entry("old")}); const auto old = id(pane, 0);
    CK_CHECK(pane.set_mark(old, true));
    const auto stale = pane.begin_listing(PaneListingPurpose::Navigate); const auto latest = pane.begin_listing(PaneListingPurpose::Navigate);
    CK_CHECK(pane.publish(stale, {entry("stale")}) == PanePublish::Stale);
    CK_CHECK(pane.publish(other.begin_listing(PaneListingPurpose::Navigate), {entry("foreign")}) == PanePublish::Stale);
    CK_CHECK(pane.row(old) && pane.marked(old) && pane.listing_pending());
    CK_CHECK(pane.sources().state == PaneSourcesState::ListingPending);
    CK_CHECK(pane.publish(latest, {entry("new")}) == PanePublish::Applied);
    CK_CHECK(!pane.row(old) && !pane.marked(old) && !pane.focus(old) && !pane.set_mark(old, true));
    CK_CHECK(pane.publish(latest, {entry("duplicate completion")}) == PanePublish::Stale);
}
CK_TEST(pane_scan_failure_preserves_focus_marks_and_native_error) {
    PaneModel pane; load(pane, {entry("retained")}); const auto row = id(pane, 0); pane.set_mark(row, true);
    const auto request = pane.begin_listing(PaneListingPurpose::Navigate);
    const UnavailableObservation failure{ObservationState::Denied, {ErrorDomain::Posix, 13, "injected.scan"}};
    CK_CHECK(pane.fail_listing(request, failure));
    CK_CHECK(pane.listing_error() && pane.listing_error()->error == failure.error);
    CK_CHECK(!pane.listing_pending() && pane.focused() == row && pane.marked(row));
    CK_CHECK(pane.sources().state == PaneSourcesState::Ready);
    CK_CHECK(!pane.fail_listing(request, failure));
    const auto next = pane.begin_listing(PaneListingPurpose::Navigate);
    bool rejected = false;
    try { pane.fail_listing(next, {ObservationState::Available, {}}); } catch (const std::invalid_argument&) { rejected = true; }
    CK_CHECK(rejected && pane.listing_pending() && !pane.listing_error());
}
CK_TEST(pane_name_order_is_unsigned_lossless_and_independent_of_arrival_order) {
    PaneModel pane;
    load(pane, {entry(std::string(1, '\xff')), entry("z"), entry("A"), entry("a"), entry(std::string(1, '\x80'))});
    CK_CHECK((names(pane) == std::vector<std::string>{"A", "a", "z", "\\x80", "\\xFF"}));
    auto view = pane.view(); view.descending = true; pane.set_view(view);
    CK_CHECK((names(pane) == std::vector<std::string>{"\\xFF", "\\x80", "z", "a", "A"}));
}
CK_TEST(pane_windows_names_keep_surrogates_and_do_not_normalize_or_fold_native_names) {
    PaneModel pane;
    auto a = entry("placeholder"), b = a, c = a;
    a.name = *NativeName::from_windows(std::u16string(1, static_cast<char16_t>(0xd800)));
    b.name = *NativeName::from_windows(u"a"); c.name = *NativeName::from_windows(u"A");
    load(pane, {a,b,c});
    CK_CHECK(*pane.visible_row(0)->entry.name.windows_units() == u"A");
    CK_CHECK(*pane.visible_row(1)->entry.name.windows_units() == u"a");
    CK_CHECK(*pane.visible_row(2)->entry.name.windows_units() == *a.name.windows_units());
}
CK_TEST(pane_folders_first_does_not_follow_junctions_or_symlinks) {
    PaneModel pane;
    auto folder = entry("z-directory", 100), link = entry("a-link", 0), junction = entry("b-junction", 0);
    folder.metadata.kind = Observation<ObjectKind>::available(ObjectKind::Directory);
    link.metadata.kind = Observation<ObjectKind>::available(ObjectKind::SymbolicLink);
    junction.metadata.kind = Observation<ObjectKind>::available(ObjectKind::Junction);
    load(pane, {link,junction,folder});
    CK_CHECK(pane.visible_row(0)->entry.name == folder.name);
    auto view = pane.view(); view.descending = true; pane.set_view(view);
    CK_CHECK(pane.visible_row(0)->entry.name == folder.name);
    view.folders_first = false; view.descending = false; pane.set_view(view);
    CK_CHECK(pane.visible_row(0)->entry.name == link.name);
}
CK_TEST(pane_numeric_sort_is_numeric_and_unavailable_values_stay_last_both_directions) {
    PaneModel pane;
    auto missing = entry("unknown");
    for (auto state : {ObservationState::Unknown, ObservationState::Denied, ObservationState::Failed, ObservationState::Unsupported, ObservationState::Gone, ObservationState::Changed}) {
        missing.metadata.logical_size = Observation<std::uint64_t>::unavailable(state);
        load(pane, {entry("ten",10),missing,entry("two",2),entry("zero",0)});
        auto view = pane.view(); view.sort = PaneSortKey::Size; view.folders_first = false; view.descending = false; pane.set_view(view);
        CK_CHECK((names(pane) == std::vector<std::string>{"zero","two","ten","unknown"}));
        view.descending = true; pane.set_view(view);
        CK_CHECK((names(pane) == std::vector<std::string>{"ten","two","zero","unknown"}));
    }
}
CK_TEST(pane_time_sort_handles_pre_epoch_nanoseconds_and_extreme_seconds_without_subtraction) {
    PaneModel pane;
    auto a = entry("a"), b = entry("b"), c = entry("c"), d = entry("d");
    a.metadata.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(-1,999999999));
    b.metadata.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(-1,0));
    c.metadata.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(std::numeric_limits<std::int64_t>::max(),0));
    d.metadata.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(std::numeric_limits<std::int64_t>::min(),0));
    load(pane,{a,b,c,d,entry("missing")}); auto view = pane.view(); view.sort = PaneSortKey::Modified; pane.set_view(view);
    CK_CHECK((names(pane) == std::vector<std::string>{"d","b","a","c","missing"}));
}
CK_TEST(pane_kind_sort_uses_observed_kinds_and_lossless_name_ties) {
    PaneModel pane; auto a=entry("a"), b=entry("b"), c=entry("c"), d=entry("d");
    b.metadata.kind=Observation<ObjectKind>::available(ObjectKind::Socket);
    c.metadata.kind=Observation<ObjectKind>::available(ObjectKind::Directory);
    d.metadata.kind={}; load(pane,{d,c,b,a}); auto view=pane.view();view.sort=PaneSortKey::Kind;view.folders_first=false;pane.set_view(view);
    CK_CHECK((names(pane)==std::vector<std::string>{"a","c","b","d"}));
    view.descending=true;pane.set_view(view);CK_CHECK((names(pane)==std::vector<std::string>{"b","c","a","d"}));
}
CK_TEST(pane_marks_and_focus_survive_sorting_and_filtering_without_index_retargeting) {
    PaneModel pane; load(pane,{entry("alpha"),entry("beta"),entry("gamma")});
    const auto alpha=id(pane,0), beta=id(pane,1); pane.set_mark(alpha,true);pane.focus(beta);
    auto view=pane.view();view.descending=true;pane.set_view(view);
    CK_CHECK(pane.visible_index(alpha)==2 && pane.visible_index(beta)==1 && pane.focused()==beta && pane.marked(alpha));
    view.display_substring="beta";pane.set_view(view);
    auto sources=pane.sources();CK_CHECK(sources.state==PaneSourcesState::HiddenMarksNeedReview && sources.hidden_count==1 && sources.visible_count==0);
    CK_CHECK(sources.origin==PaneSourcesOrigin::Marks && sources.candidates.size()==1 && sources.candidates[0].row==alpha);
    CK_CHECK(!pane.set_mark(alpha,false)); // hidden marks require explicit clear/review
    pane.clear_marks();sources=pane.sources();CK_CHECK(sources.state==PaneSourcesState::Ready && sources.candidates[0].row==beta);
    view.display_substring="absent";pane.set_view(view);CK_CHECK(pane.sources().state==PaneSourcesState::Empty && pane.focused()==beta);
}
CK_TEST(pane_hidden_flags_do_not_hide_entries_with_unknown_or_denied_visibility) {
    PaneModel pane;auto unknown=entry("unknown"),denied=entry("denied");unknown.hidden={};denied.hidden=Observation<bool>::unavailable(ObservationState::Denied);
    load(pane,{entry("hidden",{},true),unknown,denied});CK_CHECK(pane.size()==3 && pane.visible_size()==2);
    auto view=pane.view();view.show_hidden=true;pane.set_view(view);CK_CHECK(pane.visible_size()==3);
    const auto hidden=id(pane,1);pane.set_mark(hidden,true);view.show_hidden=false;pane.set_view(view);
    CK_CHECK(pane.sources().state==PaneSourcesState::HiddenMarksNeedReview && pane.sources().hidden_count==1);
}
CK_TEST(pane_range_all_and_invert_only_change_visible_marks_and_invalid_ranges_are_atomic) {
    PaneModel pane;load(pane,{entry("a"),entry("b"),entry("c"),entry("d")});
    const auto a=id(pane,0),b=id(pane,1),c=id(pane,2),d=id(pane,3);
    CK_CHECK(pane.mark_range(2,0,true));CK_CHECK(pane.marked(a)&&pane.marked(b)&&pane.marked(c)&&!pane.marked(d));
    CK_CHECK(!pane.mark_range(0,4,false)&&pane.marked(a));
    auto view=pane.view();view.display_substring="b";pane.set_view(view);pane.invert_visible_marks();
    CK_CHECK(pane.marked(a)&&!pane.marked(b)&&pane.marked(c));pane.mark_visible(true);CK_CHECK(pane.marked(b));
    pane.mark_visible(false);CK_CHECK(!pane.marked(b)&&pane.marked(a));pane.clear_marks();CK_CHECK(!pane.marked(a)&&!pane.marked(c));
}
CK_TEST(pane_filters_prepared_safe_text_without_parsing_display_back_into_native_names) {
    PaneModel pane; load(pane,{entry("A\nB"),entry("a\\nB"),entry("ALPHA"),entry("alpha")});
    auto view=pane.view();view.display_substring="alpha";pane.set_view(view);CK_CHECK(pane.visible_size()==2);
    view.ascii_case_sensitive=true;pane.set_view(view);CK_CHECK(pane.visible_size()==1 && pane.visible_row(0)->display_name=="alpha");
    view.display_substring="A\\u{000A}";pane.set_view(view);CK_CHECK(pane.visible_size()==1);
    CK_CHECK(*pane.visible_row(0)->entry.name.posix_bytes()=="A\nB");
}
CK_TEST(pane_duplicate_and_mixed_names_and_resource_limits_do_not_replace_existing_state) {
    PaneModel pane({2,4,16});load(pane,{entry("old")});const auto old=id(pane,0);pane.set_mark(old,true);const auto request=pane.begin_listing(PaneListingPurpose::Navigate);
    auto moving=entry("moving");const auto owned=std::move(moving.name);CK_CHECK(owned.is_valid());
    if(!moving.name.is_valid())CK_CHECK(pane.publish(request,{moving})==PanePublish::InvalidListing);
    CK_CHECK(pane.publish(request,{entry("a"),entry("a")})==PanePublish::InvalidListing);
    auto windows=entry("b");windows.name=*NativeName::from_windows(u"b");
    CK_CHECK(pane.publish(request,{entry("a"),windows})==PanePublish::InvalidListing);
    CK_CHECK(pane.publish(request,{entry("a"),entry("b"),entry("c")})==PanePublish::LimitExceeded);
    CK_CHECK(pane.publish(request,{entry("abcde")})==PanePublish::LimitExceeded);
    CK_CHECK(pane.row(old)&&pane.marked(old)&&pane.listing_pending());
    CK_CHECK(pane.publish(request,{entry("new")})==PanePublish::Applied && !pane.row(old));
    PaneModel display_limit({1,4,3});CK_CHECK(display_limit.publish(display_limit.begin_listing(PaneListingPurpose::Navigate),{entry(std::string(1,'\xff'))})==PanePublish::LimitExceeded);
}
CK_TEST(pane_row_handles_never_recycle_or_cross_panes_and_hard_links_remain_distinct_names) {
    PaneModel pane,other;AdapterScope adapter;
    auto a=entry("a"),b=entry("b");ObjectIdentity identity{adapter,IdentityScheme::PosixDeviceInode,NativeIdentifier::from_u64(1),NativeIdentifier::from_u64(2),{}};
    a.metadata.identity=Observation<ObjectIdentity>::available(identity);b.metadata.identity=a.metadata.identity;
    load(pane,{a,b});const auto old=id(pane,0);pane.mark_visible(true);CK_CHECK(pane.sources().candidates.size()==2);
    load(other,{a,b});CK_CHECK(!other.row(old)&&!other.focus(old)&&!other.set_mark(old,true));
    load(pane,{});CK_CHECK(!pane.row(old)&&!pane.focused()&&pane.sources().state==PaneSourcesState::Empty);
    load(pane,{a,b});CK_CHECK(id(pane,0).number>old.number && !pane.marked(id(pane,0)) && !pane.row(old));
}
CK_TEST(pane_invalid_settings_preserve_prepared_view_and_selection) {
    PaneModel pane;load(pane,{entry("a")});const auto row=id(pane,0);pane.set_mark(row,true);
    for (bool invalid_enum : {false,true}) {
        auto view=pane.view();if(invalid_enum)view.sort=static_cast<PaneSortKey>(999);else view.display_substring=std::string(4097,'x');
        bool rejected=false;try{pane.set_view(view);}catch(const std::invalid_argument&){rejected=true;}
        CK_CHECK(rejected&&pane.visible_size()==1&&pane.marked(row)&&pane.view().display_substring.empty());
    }
    bool rejected=false;try{PaneModel invalid({0,1,1});}catch(const std::invalid_argument&){rejected=true;}CK_CHECK(rejected);
}
CK_TEST(pane_large_projection_keeps_every_row_handle_and_mark_attached_to_its_native_name) {
    PaneModel pane;std::vector<PaneEntry> entries;
    for(std::size_t i=0;i<5000;++i)entries.push_back(entry("name-"+std::to_string(4999-i),i%17));
    load(pane,std::move(entries));std::vector<std::pair<PaneRowId,NativeName>> selected;
    for(std::size_t i=0;i<pane.visible_size();i+=97){const auto row=id(pane,i);pane.set_mark(row,true);selected.emplace_back(row,pane.row(row)->entry.name);}
    for(auto key:{PaneSortKey::Size,PaneSortKey::Name,PaneSortKey::Kind,PaneSortKey::Modified})for(bool descending:{false,true}){
        auto view=pane.view();view.sort=key;view.descending=descending;pane.set_view(view);
        std::vector<std::uint64_t> seen;
        for(std::size_t i=0;i<pane.visible_size();++i){const auto row=id(pane,i);seen.push_back(row.number);CK_CHECK(pane.visible_index(row)==i);}
        std::sort(seen.begin(),seen.end());CK_CHECK(std::adjacent_find(seen.begin(),seen.end())==seen.end()&&seen.size()==5000);
        for(const auto& [row,name]:selected)CK_CHECK(pane.marked(row)&&pane.row(row)&&pane.row(row)->entry.name==name);
        CK_CHECK(pane.sources().candidates.size()==selected.size());
    }
}
CK_TEST(pane_refresh_invalidates_sources_explicitly_and_never_falls_back_to_a_replacement_cursor) {
    PaneModel pane;load(pane,{entry("same-name"),entry("second")});const auto old=id(pane,0);pane.mark_visible(true);
    const auto request=pane.begin_listing(PaneListingPurpose::Refresh);
    CK_CHECK(pane.publish(request,{entry("same-name"),entry("second")})==PanePublish::Applied);
    CK_CHECK(!pane.row(old)&&!pane.focused()&&pane.invalidated_selection().marks==2&&pane.invalidated_selection().cursor);
    CK_CHECK(pane.sources().state==PaneSourcesState::SelectionInvalidated&&pane.sources().candidates.empty());
    auto view=pane.view();view.descending=true;pane.set_view(view);CK_CHECK(pane.sources().state==PaneSourcesState::SelectionInvalidated);
    CK_CHECK(pane.publish(pane.begin_listing(PaneListingPurpose::Refresh),{entry("same-name")})==PanePublish::Applied);
    CK_CHECK(pane.invalidated_selection().marks==2&&pane.sources().state==PaneSourcesState::SelectionInvalidated);
    CK_CHECK(!pane.focus(old)&&pane.sources().state==PaneSourcesState::SelectionInvalidated);
    CK_CHECK(pane.focus(id(pane,0))&&pane.sources().state==PaneSourcesState::Ready&&pane.invalidated_selection().marks==0);
}
CK_TEST(pane_failed_refresh_preserves_sources_and_explicit_navigation_starts_a_new_context) {
    PaneModel pane;load(pane,{entry("old")});const auto old=id(pane,0);pane.set_mark(old,true);
    const auto refresh=pane.begin_listing(PaneListingPurpose::Refresh);
    CK_CHECK(pane.fail_listing(refresh,{ObservationState::Failed,{ErrorDomain::Posix,5,"scan.io"}}));
    CK_CHECK(pane.focused()==old&&pane.marked(old)&&pane.sources().state==PaneSourcesState::Ready);
    CK_CHECK(pane.publish(pane.begin_listing(PaneListingPurpose::Refresh),{})==PanePublish::Applied);
    CK_CHECK(pane.sources().state==PaneSourcesState::SelectionInvalidated);
    pane.clear_marks();CK_CHECK(pane.sources().state==PaneSourcesState::Empty);
    load(pane,{entry("new-location")});CK_CHECK(pane.focused()==id(pane,0)&&pane.sources().state==PaneSourcesState::Ready);
}
