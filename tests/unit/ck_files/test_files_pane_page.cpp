// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_page.hpp"
#include "directory_listing.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace ck::files;
PaneEntry entry(std::string name) { return {*NativeName::from_posix(std::move(name)), {}, {}}; }
void load(PaneModel& pane, std::vector<PaneEntry> entries) {
    if (pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), entries) != PanePublish::Applied)
        throw std::runtime_error("page fixture publication failed");
}
std::shared_ptr<const PanePage> page(const PaneModel& pane, std::size_t first = 0, PanePageLimits limits = {}) {
    auto result = capture_pane_page(pane, first, limits);
    if (result.state != PanePageState::Ready || !result.page) throw std::runtime_error("page fixture capture failed");
    return result.page;
}
}
CK_TEST(pane_page_copies_display_values_and_preserves_old_pages_after_view_and_selection_changes) {
    PaneModel pane; load(pane, {entry("z"), entry(std::string(1, '\xff')), entry("a")});
    const auto a = pane.visible_row(0)->id; pane.set_mark(a, true);
    const auto old = page(pane, 0, {2, 1024});
    CK_CHECK(old->first() == 0 && old->total_rows() == 3 && old->visible_rows() == 3 && old->rows().size() == 2);
    CK_CHECK(old->rows()[0].display_name == "a" && old->rows()[0].marked && old->rows()[0].focused);
    CK_CHECK(old->rows()[1].display_name == "z" && !old->rows()[1].marked && old->text_bytes() == 2);
    CK_CHECK(pane.accepts(old->stamp()));
    auto view = pane.view(); view.descending = true; pane.set_view(view); pane.clear_marks();
    CK_CHECK(!old->stamp().current() && !pane.accepts(old->stamp()));
    CK_CHECK(old->rows()[0].marked && !old->view().descending && old->rows()[0].id == a);
    const auto fresh = page(pane);
    CK_CHECK(fresh->rows()[0].display_name == "\\xFF" && fresh->view().descending && fresh->stamp().current());
}
CK_TEST(pane_page_bounds_the_window_and_rejects_overflowing_offsets_and_invalid_limits) {
    PaneModel pane; std::vector<PaneEntry> entries;
    for (int i = 0; i < 5000; ++i) entries.push_back(entry("item" + std::to_string(i)));
    load(pane, std::move(entries));
    CK_CHECK(page(pane)->rows().size() == 256);
    CK_CHECK(page(pane, 4999)->rows().size() == 1 && page(pane, 5000)->rows().empty());
    for (auto offset : {std::size_t{5001}, std::numeric_limits<std::size_t>::max()}) {
        const auto result = capture_pane_page(pane, offset);
        CK_CHECK(result.state == PanePageState::OutOfRange && !result.page);
    }
    CK_CHECK(page(pane, 0, {4096, 16 * 1024 * 1024})->rows().size() == 4096);
    for (auto limits : {PanePageLimits{0, 1}, PanePageLimits{4097, 1}, PanePageLimits{1, 0}, PanePageLimits{1, 16 * 1024 * 1024 + 1}}) {
        bool rejected = false;
        try { (void)capture_pane_page(pane, 0, limits); } catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected);
    }
}
CK_TEST(pane_page_text_budget_includes_every_metadata_diagnostic_without_truncation) {
    // Each field is charged independently, including identity and platform-only
    // fields. No unavailable state is flattened into an available zero value.
    for (int field = 0; field < 17; ++field) {
        auto row = entry("a"); const NativeError error{ErrorDomain::NtStatus, -123, "diagnostic"};
        const auto set = [&]<class T>(Observation<T>& value) { value = Observation<T>::unavailable(ObservationState::Denied, error); };
        auto& m = row.metadata;
        switch (field) {
        case 0: set(m.identity); break; case 1: set(m.kind); break; case 2: set(m.logical_size); break;
        case 3: set(m.allocated_size); break; case 4: set(m.link_count); break; case 5: set(m.posix_uid); break;
        case 6: set(m.posix_gid); break; case 7: set(m.posix_permissions); break; case 8: set(m.bsd_flags); break;
        case 9: set(m.windows_attributes); break; case 10: set(m.reparse_tag); break; case 11: set(m.linux_attributes); break;
        case 12: set(m.mount_id); break; case 13: set(m.accessed); break; case 14: set(m.modified); break;
        case 15: set(m.changed); break; case 16: set(m.created); break;
        }
        PaneModel pane; load(pane, {row});
        const auto result = capture_pane_page(pane, 0, {1, 10});
        CK_CHECK(result.state == PanePageState::LimitExceeded && !result.page);
        CK_CHECK(page(pane, 0, {1, 11})->text_bytes() == 11);
    }
}
CK_TEST(pane_page_charges_hidden_inspection_filter_and_listing_diagnostics_and_aggregates_rows) {
    PaneModel pane; auto row = entry("a");
    row.hidden = Observation<bool>::unavailable(ObservationState::Unsupported, {ErrorDomain::Adapter, 4, "hidden"});
    load(pane, {row, entry("ab")});
    auto view = pane.view(); view.display_substring = "a"; pane.set_view(view);
    auto request = pane.begin_listing(PaneListingPurpose::Refresh);
    CK_CHECK(pane.fail_listing(request, {ObservationState::Failed, {ErrorDomain::Win32, 5, "listing"}}));
    const auto captured = page(pane, 0, {2, 17});
    CK_CHECK(captured->text_bytes() == 17 && captured->rows()[0].hidden.state() == ObservationState::Unsupported);
    CK_CHECK(captured->listing_error()->error.operation == "listing" && !captured->listing_pending());
    CK_CHECK(capture_pane_page(pane, 0, {2, 16}).state == PanePageState::LimitExceeded);
    auto failed = PaneEntry::from_listed({*NativeName::from_posix("b"), Observation<BasicMetadata>::unavailable(
        ObservationState::Gone, {ErrorDomain::Posix, 2, "inspection"})}, {});
    load(pane, {failed}); view.display_substring.clear(); pane.set_view(view);
    CK_CHECK(capture_pane_page(pane, 0, {1, 10}).state == PanePageState::LimitExceeded);
    const auto inspected = page(pane, 0, {1, 11});
    CK_CHECK(inspected->rows()[0].inspection_error->state == ObservationState::Gone);
}
CK_TEST(pane_state_stamps_revoke_on_every_successful_state_mutator_and_reject_foreign_tokens) {
    PaneModel other; PaneStateStamp empty;
    CK_CHECK(!empty.current() && !other.accepts(empty));
    for (int action = 0; action < 12; ++action) {
        PaneModel pane; load(pane, {entry("a"), entry("b")});
        auto request = pane.begin_listing(PaneListingPurpose::Refresh);
        const auto stamp = pane.state_stamp(); const auto same = pane.state_stamp();
        CK_CHECK(stamp.current() && pane.accepts(same) && !other.accepts(stamp));
        const auto id = pane.visible_row(1)->id;
        switch (action) {
        case 0: (void)pane.begin_listing(PaneListingPurpose::Navigate); break;
        case 1: CK_CHECK(pane.publish(request, {entry("c")}) == PanePublish::Applied); break;
        case 2: CK_CHECK(pane.fail_listing(request, {ObservationState::Denied, {}})); break;
        case 3: CK_CHECK(pane.cancel_listing(request)); break;
        case 4: pane.set_view(pane.view()); break;
        case 5: CK_CHECK(pane.focus(id)); break;
        case 6: CK_CHECK(pane.set_mark(id, true)); break;
        case 7: pane.mark_visible(true); break;
        case 8: pane.invert_visible_marks(); break;
        case 9: CK_CHECK(pane.mark_range(1, 0, true)); break;
        case 10: pane.clear_marks(); break;
        case 11: CK_CHECK(pane.set_mark(id, false)); break; // conservative no-op revocation
        }
        CK_CHECK(!stamp.current() && !same.current() && !pane.accepts(stamp));
        const auto fresh = pane.state_stamp(); CK_CHECK(fresh.current() && pane.accepts(fresh) && !pane.accepts(stamp));
    }
}
CK_TEST(pane_state_stamps_survive_rejected_requests_and_failed_validation) {
    PaneModel pane, other; load(pane, {entry("a")});
    const auto request = pane.begin_listing(PaneListingPurpose::Refresh), foreign = other.begin_listing(PaneListingPurpose::Navigate);
    const auto stamp = pane.state_stamp();
    CK_CHECK(!pane.cancel_listing(foreign) && !pane.fail_listing(foreign, {ObservationState::Denied, {}}));
    CK_CHECK(pane.publish(foreign, {}) == PanePublish::Stale && !pane.focus(PaneRowId{}));
    CK_CHECK(!pane.set_mark(PaneRowId{}, true) && !pane.mark_range(1, 0, true));
    CK_CHECK(pane.publish(request, {entry("a"), entry("a")}) == PanePublish::InvalidListing);
    bool rejected = false;
    try { auto view = pane.view(); view.display_substring.resize(4097); pane.set_view(std::move(view)); }
    catch (const std::invalid_argument&) { rejected = true; }
    CK_CHECK(rejected && stamp.current() && pane.accepts(stamp));
}
CK_TEST(pane_page_preserves_pending_and_invalidated_selection_without_asserting_operation_readiness) {
    PaneModel pane; auto row = entry("a");
    row.metadata.logical_size = Observation<std::uint64_t>::available(0);
    row.metadata.modified = Observation<FileTimestamp>::available(*FileTimestamp::from_unix(-1, 999999999));
    load(pane, {row}); pane.mark_visible(true);
    const auto request = pane.begin_listing(PaneListingPurpose::Refresh);
    const auto pending = page(pane);
    CK_CHECK(pending->listing_pending() && pending->rows()[0].marked);
    CK_CHECK(*pending->rows()[0].metadata.logical_size.value() == 0);
    CK_CHECK(pending->rows()[0].metadata.modified.value()->seconds() == -1 &&
        pending->rows()[0].metadata.modified.value()->nanoseconds() == 999999999);
    CK_CHECK(pane.publish(request, {row}) == PanePublish::Applied);
    const auto fresh = page(pane);
    CK_CHECK(!pending->stamp().current() && !fresh->listing_pending());
    CK_CHECK(fresh->invalidated_selection().marks == 1 && fresh->invalidated_selection().cursor);
}
CK_TEST(pane_page_does_not_retain_native_owners_and_can_outlive_worker_model) {
    ck::testing::SimulatedFileSystem fs;
    const auto root = fs.make_node(ObjectKind::Directory), file = fs.make_node(ObjectKind::RegularFile);
#ifdef _WIN32
    const std::filesystem::path path = L"V:/simulated-pane-page";
#else
    const std::filesystem::path path = "/simulated-pane-page";
#endif
    const auto name = *NativeName::from_posix("held"); fs.bind(path, root); fs.put(root, name, file);
    std::shared_ptr<const PanePage> captured;
    {
        auto opened = fs.open_directory(path);
        CK_CHECK(opened.value());
        auto parent = std::shared_ptr<const DirectoryCursor>(std::move(opened).take_value());
        auto observed = HeldChild::observe(parent, name); CK_CHECK(observed.value());
        PaneModel pane; load(pane, {PaneEntry::from_held(std::move(observed).take_value(), {})});
        const auto calls = fs.calls().size(); captured = page(pane);
        CK_CHECK(captured->stamp().current() && fs.calls().size() == calls && fs.live_entries() == 1 && fs.live_cursors() == 1);
    }
    CK_CHECK(fs.live_entries() == 0 && fs.live_cursors() == 0 && !captured->stamp().current());
    bool read = false;
    std::thread ui([&] { read = captured->rows()[0].display_name == "held" && !captured->stamp().current(); });
    ui.join(); CK_CHECK(read);
}
