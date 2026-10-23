// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_acquire_job.hpp"
#include "pane_refresh_job.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <new>
#include <stdexcept>
namespace {
using namespace ck::files;
using FileSystem = ck::testing::SimulatedFileSystem;
using Operation = FileSystem::Operation;
template<class T> T take(Observation<T> result) {
    if (!result.value()) throw std::runtime_error("required acquisition observation unavailable");
    return std::move(result).take_value();
}
NativeName name(const char* text) { return *NativeName::from_posix(text); }
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-pane-acquire";
#else
    return "/simulated-pane-acquire";
#endif
}
PaneListingJob::Result finish(auto& job) {
    for (int i = 0; i < 100; ++i) {
        if (job.advance() == PaneListingJobState::Complete) return take(job.completed());
        if (job.state() == PaneListingJobState::Failed) throw std::runtime_error("acquisition job failed");
    }
    throw std::runtime_error("acquisition step budget exceeded");
}
struct Fixture {
    FileSystem fs;
    FileSystem::Pointer root = fs.make_node(ObjectKind::Directory), file = fs.make_node(ObjectKind::RegularFile);
    PaneModel pane;
    Fixture() { fs.bind(path(), root); fs.put(root, name("a"), file); fs.metadata(file).logical_size = Observation<std::uint64_t>::available(3); }
    PaneListingJob::Result scan(ListingDirectoryRetention retention = ListingDirectoryRetention::RetainForRefresh) {
        PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), take(DirectoryListing::open(fs, path(), {}, fs.clock())),
            StorageModel::Unknown, {}, retention);
        return finish(job);
    }
};
std::size_t opens(const FileSystem& fs) { return static_cast<std::size_t>(std::count_if(fs.calls().begin(), fs.calls().end(), [](const auto& call) { return call.operation == Operation::OpenChild; })); }
PaneListingJob::Result refresh_input(Fixture& f, const std::vector<HeldChild::Pointer>& previous) {
    PaneListingJob scan(f.pane.begin_listing(PaneListingPurpose::Refresh), take(DirectoryListing::open(f.fs, path(), {}, f.fs.clock())),
        StorageModel::LinuxStatx, {}, ListingDirectoryRetention::RetainForRefresh);
    PaneRefreshJob refresh(finish(scan), previous, StorageModel::LinuxStatx, {}, f.fs.clock(), std::chrono::milliseconds(20), ListingDirectoryRetention::RetainForRefresh);
    return finish(refresh);
}
PaneRowId seed(Fixture& f) {
    PaneAcquireJob initial(f.scan(), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock());
    if (f.pane.publish(*finish(initial)) != PanePublish::Applied || !f.pane.focused()) throw std::runtime_error("cannot seed mixed fixture");
    const auto id = *f.pane.focused(); f.pane.set_mark(id, true); return id;
}
}
CK_TEST(pane_acquire_binds_requested_rows_with_fresh_metadata_and_sealed_refresh_continuity) {
    Fixture f; f.fs.put(f.root, name("b"), f.fs.make_node(ObjectKind::RegularFile)); auto raw = f.scan();
    f.fs.metadata(f.file).logical_size = Observation<std::uint64_t>::available(99);
    PaneAcquireJob job(std::move(raw), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock()); auto result = finish(job);
    CK_CHECK(!result->retains_scan_directory() && result->binding_issues().empty() && f.fs.live_entries() == 1);
    CK_CHECK(f.pane.publish(*result) == PanePublish::Applied && f.pane.held_entries().size() == 1 && f.pane.size() == 2);
    const auto id = f.pane.visible_row(0)->id; CK_CHECK(take(f.pane.row(id)->entry.metadata.logical_size) == 99);
    CK_CHECK(f.pane.set_mark(id, true)); auto held = f.pane.held_entries();
    PaneListingJob scan(f.pane.begin_listing(PaneListingPurpose::Refresh), take(DirectoryListing::open(f.fs, path(), {}, f.fs.clock())),
        StorageModel::LinuxStatx, {}, ListingDirectoryRetention::RetainForRefresh);
    PaneRefreshJob refresh(finish(scan), held, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(f.pane.publish(*finish(refresh)) == PanePublish::Applied && f.pane.marked(id) && f.pane.focused() == id);
}
CK_TEST(pane_acquire_opens_current_objects_without_promising_continuity_with_unheld_discovery_ids) {
    Fixture f; auto raw = f.scan(); auto replacement = f.fs.make_node(ObjectKind::RegularFile); f.fs.put(f.root, name("a"), replacement);
    PaneAcquireJob job(std::move(raw), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock()); auto result = finish(job);
    const auto held = result->entries()[0].held_child(); CK_CHECK(held && !held->previous_continuity().value());
    CK_CHECK(compare_identity(take(held->metadata().identity), take(f.fs.metadata(replacement).identity)) == IdentityComparison::Same);
    CK_CHECK(compare_identity(take(held->metadata().identity), take(f.fs.metadata(f.file).identity)) == IdentityComparison::Different);
}
CK_TEST(pane_acquire_uses_the_scanned_parent_after_original_path_rebinding_and_pins_independent_hard_link_names) {
    Fixture f; f.fs.put(f.root, name("b"), f.file); auto raw = f.scan(); auto other = f.fs.make_node(ObjectKind::Directory); f.fs.bind(path(), other);
    f.fs.put(other, name("a"), f.fs.make_node(ObjectKind::RegularFile));
    PaneAcquireJob job(std::move(raw), {name("a"), name("b")}, StorageModel::LinuxStatx, {}, f.fs.clock()); auto result = finish(job);
    CK_CHECK(result->binding_issues().empty() && f.fs.live_entries() == 2 && f.fs.live_cursors() == 1);
    const auto a = result->entries()[0].held_child(), b = result->entries()[1].held_child();
    CK_CHECK(a && b && a->name() != b->name() && !a->continues(*b));
    CK_CHECK(compare_identity(take(a->metadata().identity), take(b->metadata().identity)) == IdentityComparison::Same);
}
CK_TEST(pane_acquire_missing_discovery_rows_are_uncertain_and_never_probed_by_guessed_name) {
    Fixture f; auto raw = f.scan(); const auto count = f.fs.calls().size();
    PaneAcquireJob job(std::move(raw), {name("absent")}, StorageModel::LinuxStatx, {}, f.fs.clock()); auto result = finish(job);
    CK_CHECK(f.fs.calls().size() == count && f.fs.live_cursors() == 0 && !result->entries()[0].held_child());
    CK_CHECK(result->binding_issues().size() == 1 && result->binding_issues()[0].detail.state == ObservationState::Unknown &&
        result->binding_issues()[0].detail.error.operation == "pane_acquire.not_listed");
}
CK_TEST(pane_acquire_preserves_each_native_failure_without_removing_the_discovery_row) {
    for (auto state : {ObservationState::Unknown, ObservationState::Unsupported, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed})
        for (bool metadata : {false, true}) {
            Fixture f; auto raw = f.scan(); const NativeError error{ErrorDomain::Posix, 123, "acquire.native"};
            f.fs.enqueue(metadata ? Operation::InspectHandle : Operation::OpenChild, {UnavailableObservation{state, error}, {}, {}, false});
            PaneAcquireJob job(std::move(raw), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock()); auto result = finish(job);
            CK_CHECK(result->entries().size() == 1 && !result->entries()[0].held_child() && f.fs.live_entries() == 0);
            CK_CHECK(result->binding_issues().size() == 1 && result->binding_issues()[0].detail.state == state && result->binding_issues()[0].detail.error == error);
        }
}
CK_TEST(pane_acquire_cancellation_discards_incomplete_handles_and_retries_only_the_interrupted_binding) {
    for (bool metadata : {false, true}) {
        Fixture f; f.fs.put(f.root, name("b"), f.file); auto raw = f.scan(); const auto before = opens(f.fs); std::stop_source stop;
        f.fs.enqueue(metadata ? Operation::InspectHandle : Operation::OpenChild, {});
        f.fs.enqueue(metadata ? Operation::InspectHandle : Operation::OpenChild, {{}, {}, [&] { stop.request_stop(); }, false});
        PaneAcquireJob job(std::move(raw), {name("a"), name("b")}, StorageModel::LinuxStatx, {}, f.fs.clock());
        CK_CHECK(job.advance(stop.get_token()) == PaneListingJobState::Paused && !job.completed().value() && f.fs.live_entries() == 1);
        const auto count = f.fs.calls().size(); CK_CHECK(job.advance(stop.get_token()) == PaneListingJobState::Paused && f.fs.calls().size() == count);
        const auto result = finish(job); CK_CHECK(result->binding_issues().empty() && f.fs.live_entries() == 2 && opens(f.fs) == before + 3);
    }
}
CK_TEST(pane_acquire_slow_completed_bindings_survive_time_yields_without_reopening) {
    Fixture f; auto raw = f.scan(); f.fs.enqueue(Operation::OpenChild, {{}, std::chrono::milliseconds(20), {}, false});
    PaneAcquireJob job(std::move(raw), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && f.fs.live_entries() == 1 && !job.completed().value());
    const auto count = f.fs.calls().size(); CK_CHECK(finish(job)->entries()[0].held_child() && f.fs.calls().size() == count);
}
CK_TEST(pane_acquire_preparation_is_step_bounded_and_never_exposes_partial_results) {
    Fixture f; for (int i = 0; i < 299; ++i) f.fs.put(f.root, name(("z" + std::to_string(i)).c_str()), f.fs.make_node(ObjectKind::RegularFile));
    auto raw = f.scan(); const auto count = f.fs.calls().size(), before = opens(f.fs);
    PaneAcquireJob job(std::move(raw), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && !job.completed().value() && f.fs.calls().size() == count);
    CK_CHECK(finish(job)->entries().size() == 300 && opens(f.fs) == before + 1);
}
CK_TEST(pane_acquire_rejects_duplicate_requests_and_byte_limits_before_any_native_binding) {
    for (int mode = 0; mode < 4; ++mode) {
        Fixture f; auto raw = f.scan(); PaneLimits limits; std::vector<NativeName> requested{name("a")};
        if (mode == 0) requested.push_back(name("a"));
        if (mode == 1) { requested[0] = name("long"); limits.native_name_bytes = 1; }
        if (mode >= 2) { f.fs.put(f.root, name("bb"), f.file); raw = f.scan(); if (mode == 2) limits.native_name_bytes = 1; else limits.display_name_bytes = 1; }
        const auto count = f.fs.calls().size(); PaneAcquireJob job(std::move(raw), std::move(requested), StorageModel::LinuxStatx, limits, f.fs.clock());
        CK_CHECK(job.advance() == PaneListingJobState::Failed && !job.completed().value() && f.fs.calls().size() == count && f.fs.live_cursors() == 0);
    }
}
CK_TEST(pane_acquire_configuration_requires_a_retained_scan_and_valid_counts_clock_and_model) {
    for (int mode = 0; mode < 9; ++mode) {
        Fixture f; auto raw = f.scan(mode == 0 ? ListingDirectoryRetention::ReleaseAtEnd : ListingDirectoryRetention::RetainForRefresh);
        PaneLimits limits; if (mode == 5) limits.rows = 0; if (mode == 6) limits.held_entries = 1;
        std::vector<NativeName> requested{name("a")}; if (mode == 6) requested.push_back(name("b")); if (mode == 8) raw.reset();
        bool rejected = false;
        try { PaneAcquireJob job(std::move(raw), std::move(requested), mode == 1 ? static_cast<StorageModel>(999) : StorageModel::LinuxStatx,
            limits, mode == 2 ? std::shared_ptr<const MonotonicClock>{} : f.fs.clock(), mode == 3 ? MonotonicClock::Duration{} : mode == 4 ? std::chrono::seconds(2) : std::chrono::milliseconds(20));
            if (mode == 7) { CK_CHECK(job.advance(std::stop_source{}.get_token()) == PaneListingJobState::Complete); }
        } catch (const std::invalid_argument&) { rejected = true; } catch (const std::length_error&) { rejected = true; }
        CK_CHECK(rejected == (mode != 7));
    }
}
CK_TEST(pane_acquire_poisoning_on_exceptions_or_clock_reversal_releases_staging) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f; auto raw = f.scan(); PaneAcquireJob job(std::move(raw), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock());
        if (mode == 0) f.fs.enqueue(Operation::OpenChild, {{}, {}, {}, true});
        if (mode == 1) f.fs.enqueue(Operation::InspectHandle, {{}, {}, [] { throw std::bad_alloc{}; }, false});
        if (mode == 2) { f.fs.enqueue(Operation::OpenChild, {{}, std::chrono::milliseconds(20), {}, false}); CK_CHECK(job.advance() == PaneListingJobState::Collecting); f.fs.clock()->advance(-std::chrono::nanoseconds(1)); }
        bool threw = false; try { (void)job.advance(); } catch (const std::exception&) { threw = true; }
        CK_CHECK(threw == (mode != 2) && job.state() == PaneListingJobState::Failed && !job.abandon());
        CK_CHECK(f.fs.live_entries() == 0 && f.fs.live_cursors() == 0 && job.failure() && !job.completed().value());
    }
}
CK_TEST(pane_acquire_abandonment_and_superseding_generations_reject_late_publication) {
    Fixture f; PaneAcquireJob job(f.scan(), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock()); auto result = finish(job);
    const auto request = f.pane.begin_listing(PaneListingPurpose::Navigate);
    CK_CHECK(job.abandon() && !job.abandon() && f.pane.publish(*result) == PanePublish::Stale && f.pane.listing_pending());
    CK_CHECK(!f.pane.cancel_listing(job.request()) && f.pane.cancel_listing(request));
    CK_CHECK(f.fs.live_entries() == 1); result.reset(); CK_CHECK(f.fs.live_entries() == 0 && f.fs.live_cursors() == 0);
}
CK_TEST(pane_acquire_fresh_bindings_never_retain_previously_selected_unheld_row_ids) {
    Fixture f; const auto raw = f.scan(); CK_CHECK(f.pane.publish(*raw) == PanePublish::Applied); const auto id = *f.pane.focused(); f.pane.set_mark(id, true);
    PaneListingJob scan(f.pane.begin_listing(PaneListingPurpose::Refresh), take(DirectoryListing::open(f.fs, path(), {}, f.fs.clock())),
        StorageModel::LinuxStatx, {}, ListingDirectoryRetention::RetainForRefresh);
    PaneAcquireJob job(finish(scan), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(f.pane.publish(*finish(job)) == PanePublish::Applied && !f.pane.row(id) && !f.pane.focused() && f.pane.held_entries().size() == 1);
    CK_CHECK(f.pane.sources().state == PaneSourcesState::SelectionInvalidated && f.pane.invalidated_selection().marks == 1);
}
CK_TEST(pane_acquire_recomputes_hidden_state_and_preserves_unqualified_identity_without_certifying_it) {
    Fixture f; auto raw = f.scan(); f.fs.metadata(f.file).identity = {}; f.fs.metadata(f.file).bsd_flags = Observation<std::uint32_t>::available(0x8000);
    PaneAcquireJob job(std::move(raw), {name("a")}, StorageModel::DarwinStatFlags, {}, f.fs.clock()); auto result = finish(job);
    CK_CHECK(result->entries()[0].held_child() && !result->entries()[0].metadata.identity.value() && take(result->entries()[0].hidden));
    CK_CHECK(f.pane.publish(*result) == PanePublish::Applied && f.pane.visible_size() == 0 && f.pane.held_entries().size() == 1);
}
CK_TEST(pane_acquire_utf16_budget_is_in_bytes_and_exact_limits_allow_binding) {
    for (std::size_t bytes : {std::size_t{3}, std::size_t{4}}) {
        Fixture f; const auto wide = *NativeName::from_windows(u"xy"); f.fs.erase(f.root, name("a")); f.fs.put(f.root, wide, f.file);
        auto raw = f.scan(); PaneLimits limits; limits.native_name_bytes = bytes; limits.display_name_bytes = 2;
        const auto before = f.fs.calls().size(); PaneAcquireJob job(std::move(raw), {wide}, StorageModel::WindowsHandleAttributes, limits, f.fs.clock());
        if (bytes == 3) CK_CHECK(job.advance() == PaneListingJobState::Failed && f.fs.calls().size() == before);
        else CK_CHECK(finish(job)->entries()[0].held_child() && f.fs.live_entries() == 1);
    }
}
CK_TEST(pane_acquire_wrong_platform_names_fail_before_opening_and_empty_requests_release_the_scan) {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f; auto raw = f.scan(); const auto before = f.fs.calls().size();
        std::vector<NativeName> requested; if (mode == 1) requested.push_back(*NativeName::from_windows(u"a")); if (mode == 2) requested.push_back(name("a"));
        PaneAcquireJob job(std::move(raw), std::move(requested), mode == 2 ? StorageModel::WindowsHandleAttributes : StorageModel::LinuxStatx, {}, f.fs.clock());
        if (mode == 0) CK_CHECK(finish(job)->binding_issues().empty() && f.fs.live_cursors() == 0);
        else CK_CHECK(job.advance() == PaneListingJobState::Failed);
        CK_CHECK(f.fs.calls().size() == before && f.fs.live_entries() == 0);
    }
}
CK_TEST(pane_acquire_abandoning_a_paused_job_releases_its_retained_directory_without_native_io) {
    Fixture f; PaneAcquireJob job(f.scan(), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock());
    std::stop_source stop; stop.request_stop(); const auto before = f.fs.calls().size();
    CK_CHECK(job.advance(stop.get_token()) == PaneListingJobState::Paused && f.fs.live_cursors() == 1);
    CK_CHECK(job.abandon() && f.fs.live_cursors() == 0 && !job.completed().value() && !job.abandon());
    CK_CHECK(job.advance() == PaneListingJobState::Abandoned && f.fs.calls().size() == before && f.pane.cancel_listing(job.request()));
}
CK_TEST(pane_acquire_composes_refresh_and_new_binding_with_one_publication_and_no_duplicate_open) {
    Fixture f; const auto id = seed(f); f.fs.put(f.root, name("b"), f.fs.make_node(ObjectKind::RegularFile));
    auto refreshed = refresh_input(f, f.pane.held_entries()); CK_CHECK(refreshed->retains_scan_directory());
    const auto before = opens(f.fs); PaneAcquireJob job(std::move(refreshed), {name("a"), name("b")}, StorageModel::LinuxStatx, {}, f.fs.clock());
    const auto result = finish(job); CK_CHECK(opens(f.fs) == before + 1 && !result->retains_scan_directory() && result->binding_issues().empty());
    CK_CHECK(f.pane.publish(*result) == PanePublish::Applied && f.pane.marked(id) && f.pane.focused() == id && f.pane.held_entries().size() == 2);
    CK_CHECK(f.pane.visible_row(1)->id != id && !f.pane.row(f.pane.visible_row(1)->id)->entry.held_child()->previous_continuity().value());
}
CK_TEST(pane_acquire_union_handle_budget_counts_existing_and_new_rows_before_any_open) {
    for (std::size_t count : {std::size_t{1}, std::size_t{2}}) {
        Fixture f; const auto id = seed(f); f.fs.put(f.root, name("b"), f.file); auto refreshed = refresh_input(f, f.pane.held_entries());
        const auto before = opens(f.fs); PaneLimits limits; limits.held_entries = count;
        PaneAcquireJob job(std::move(refreshed), {name("b")}, StorageModel::LinuxStatx, limits, f.fs.clock());
        if (count == 1) CK_CHECK(job.advance() == PaneListingJobState::Failed && opens(f.fs) == before && f.pane.marked(id));
        else CK_CHECK(f.pane.publish(*finish(job)) == PanePublish::Applied && f.pane.marked(id) && opens(f.fs) == before + 1);
    }
    Fixture f; const auto id = seed(f); auto refreshed = refresh_input(f, f.pane.held_entries()); const auto before = opens(f.fs);
    PaneLimits limits; limits.held_entries = 1; PaneAcquireJob job(std::move(refreshed), {name("a")}, StorageModel::LinuxStatx, limits, f.fs.clock());
    CK_CHECK(f.pane.publish(*finish(job)) == PanePublish::Applied && f.pane.marked(id) && opens(f.fs) == before);
}
CK_TEST(pane_acquire_preserves_refresh_failures_even_when_a_later_initial_binding_succeeds) {
    Fixture f; const auto id = seed(f); auto previous = f.pane.held_entries();
    PaneListingJob scan(f.pane.begin_listing(PaneListingPurpose::Refresh), take(DirectoryListing::open(f.fs, path(), {}, f.fs.clock())), StorageModel::LinuxStatx, {}, ListingDirectoryRetention::RetainForRefresh);
    const auto raw = finish(scan); const NativeError denied{ErrorDomain::Posix, 13, "mixed.denied"};
    f.fs.enqueue(Operation::OpenChild, {UnavailableObservation{ObservationState::Denied, denied}, {}, {}, false});
    PaneRefreshJob refresh(raw, previous, StorageModel::LinuxStatx, {}, f.fs.clock(), std::chrono::milliseconds(20), ListingDirectoryRetention::RetainForRefresh);
    auto refreshed = finish(refresh); CK_CHECK(refreshed->binding_issues().size() == 1 && !refreshed->entries()[0].held_child());
    PaneAcquireJob job(std::move(refreshed), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock()); const auto result = finish(job);
    CK_CHECK(result->entries()[0].held_child() && result->binding_issues().size() == 1 && result->binding_issues()[0].detail.error == denied);
    CK_CHECK(f.pane.publish(*result) == PanePublish::Applied && !f.pane.row(id) && f.pane.sources().state == PaneSourcesState::SelectionInvalidated);
}
CK_TEST(pane_acquire_preserves_replacement_and_unlisted_issues_with_independent_new_failures) {
    Fixture f; const auto id = seed(f); auto previous = f.pane.held_entries(); f.fs.put(f.root, name("a"), f.fs.make_node(ObjectKind::RegularFile));
    auto refreshed = refresh_input(f, previous); const auto before = opens(f.fs);
    PaneAcquireJob job(std::move(refreshed), {name("a"), name("missing")}, StorageModel::LinuxStatx, {}, f.fs.clock()); const auto result = finish(job);
    CK_CHECK(opens(f.fs) == before && result->binding_issues().size() == 2 && result->binding_issues()[0].detail.state == ObservationState::Changed);
    CK_CHECK(result->binding_issues()[1].detail.error.operation == "pane_acquire.not_listed");
    CK_CHECK(f.pane.publish(*result) == PanePublish::Applied && !f.pane.row(id) && f.pane.invalidated_selection().marks == 1);
}
CK_TEST(pane_acquire_validates_inherited_issue_bytes_before_opening_new_children) {
    Fixture f; f.fs.put(f.root, name("long"), f.file); auto parent = std::shared_ptr<const DirectoryCursor>(take(f.fs.open_directory(path())));
    std::vector<HeldChild::Pointer> previous{take(HeldChild::observe(parent, name("long")))}; f.fs.erase(f.root, name("long"));
    auto refreshed = refresh_input(f, previous); const auto before = opens(f.fs); PaneLimits limits; limits.native_name_bytes = 1;
    PaneAcquireJob job(std::move(refreshed), {name("a")}, StorageModel::LinuxStatx, limits, f.fs.clock());
    CK_CHECK(job.advance() == PaneListingJobState::Failed && opens(f.fs) == before && job.failure()->error.operation == "pane_acquire.issue_bytes");
}
CK_TEST(pane_acquire_cancellation_keeps_sealed_successors_and_inherited_issues_until_resume) {
    Fixture f; const auto id = seed(f); auto previous = f.pane.held_entries(); f.fs.put(f.root, name("c"), f.file);
    auto parent = std::shared_ptr<const DirectoryCursor>(take(f.fs.open_directory(path()))); previous.push_back(take(HeldChild::observe(parent, name("c"))));
    f.fs.erase(f.root, name("c")); f.fs.put(f.root, name("b"), f.file); auto refreshed = refresh_input(f, previous); std::stop_source stop;
    f.fs.enqueue(Operation::OpenChild, {{}, {}, [&] { stop.request_stop(); }, false}); const auto before = opens(f.fs);
    PaneAcquireJob job(std::move(refreshed), {name("b")}, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(job.advance(stop.get_token()) == PaneListingJobState::Paused && f.pane.marked(id));
    const auto result = finish(job); CK_CHECK(result->binding_issues().size() == 1 && result->binding_issues()[0].name == name("c"));
    CK_CHECK(f.pane.publish(*result) == PanePublish::Applied && f.pane.marked(id) && opens(f.fs) == before + 2 && f.pane.held_entries().size() == 2);
}
CK_TEST(pane_refresh_rejects_unknown_result_retention_before_refreshing_children) {
    Fixture f; seed(f); const auto previous = f.pane.held_entries(); const auto raw = f.scan(); const auto before = opens(f.fs);
    bool rejected = false; try { PaneRefreshJob job(raw, previous, StorageModel::LinuxStatx, {}, f.fs.clock(), std::chrono::milliseconds(20), static_cast<ListingDirectoryRetention>(999)); }
    catch (const std::invalid_argument&) { rejected = true; }
    CK_CHECK(rejected && opens(f.fs) == before);
}
CK_TEST(pane_acquire_inherited_issue_copying_is_step_bounded_and_preserves_every_issue) {
    Fixture f; std::vector<HeldChild::Pointer> previous; auto parent = std::shared_ptr<const DirectoryCursor>(take(f.fs.open_directory(path())));
    for (int i = 0; i < 300; ++i) {
        const auto n = name(("old" + std::to_string(i)).c_str()); f.fs.put(f.root, n, f.file); previous.push_back(take(HeldChild::observe(parent, n))); f.fs.erase(f.root, n);
    }
    auto refreshed = refresh_input(f, previous); const auto before = opens(f.fs);
    PaneAcquireJob job(std::move(refreshed), {name("a")}, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && !job.completed().value() && opens(f.fs) == before);
    const auto result = finish(job); CK_CHECK(result->binding_issues().size() == 300 && opens(f.fs) == before + 1);
    for (std::size_t i = 0; i < previous.size(); ++i) CK_CHECK(result->binding_issues()[i].name == previous[i]->name());
}
CK_TEST(pane_acquire_recomputes_preserved_binding_visibility_for_the_requested_model) {
    Fixture f; const auto id = seed(f); f.fs.metadata(f.file).bsd_flags = Observation<std::uint32_t>::available(0x8000);
    auto refreshed = refresh_input(f, f.pane.held_entries()); CK_CHECK(!take(refreshed->entries()[0].hidden));
    const auto before = opens(f.fs); PaneAcquireJob job(std::move(refreshed), {}, StorageModel::DarwinStatFlags, {}, f.fs.clock());
    const auto result = finish(job); CK_CHECK(take(result->entries()[0].hidden) && opens(f.fs) == before);
    CK_CHECK(f.pane.publish(*result) == PanePublish::Applied && f.pane.marked(id) && f.pane.visible_size() == 0 && f.pane.sources().state == PaneSourcesState::HiddenMarksNeedReview);
}
