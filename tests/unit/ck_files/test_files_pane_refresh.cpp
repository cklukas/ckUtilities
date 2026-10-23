// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_refresh_job.hpp"
#include "simulated_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include <stdexcept>
#include <new>
namespace {
using namespace ck::files;
using FileSystem = ck::testing::SimulatedFileSystem;
using Operation = FileSystem::Operation;
template<class T> T take(Observation<T> result) {
    if (!result.value()) throw std::runtime_error("required pane refresh fixture observation unavailable");
    return std::move(result).take_value();
}
NativeName name(const char* text) { return *NativeName::from_posix(text); }
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-pane-refresh";
#else
    return "/simulated-pane-refresh";
#endif
}
PaneListingJob::Result finish(auto& job) {
    for (int i = 0; i < 100; ++i) {
        if (job.advance() == PaneListingJobState::Complete) return take(job.completed());
        if (job.state() == PaneListingJobState::Failed) throw std::runtime_error("pane refresh fixture job failed");
    }
    throw std::runtime_error("pane refresh fixture exceeded step budget");
}
PaneListingJob::Result scan(FileSystem& fs, PaneListingRequest request, ListingDirectoryRetention retention = ListingDirectoryRetention::RetainForRefresh) {
    PaneListingJob job(request, take(DirectoryListing::open(fs, path(), {}, fs.clock())), StorageModel::Unknown, {}, retention);
    return finish(job);
}
struct Fixture {
    FileSystem fs;
    FileSystem::Pointer root = fs.make_node(ObjectKind::Directory), file = fs.make_node(ObjectKind::RegularFile);
    std::shared_ptr<const DirectoryCursor> parent;
    HeldChild::Pointer held;
    PaneModel pane;
    PaneRowId id;
    Fixture() {
        fs.bind(path(), root); fs.put(root, name("a"), file);
        fs.metadata(file).logical_size = Observation<std::uint64_t>::available(12);
        parent = std::shared_ptr<const DirectoryCursor>(take(fs.open_directory(path())));
        held = take(HeldChild::observe(parent, name("a")));
        if (pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), {PaneEntry::from_held(held, Observation<bool>::available(false))}) != PanePublish::Applied)
            throw std::runtime_error("cannot seed held pane");
        if (!pane.focused()) throw std::runtime_error("missing pane fixture focus");
        id = *pane.focused(); pane.set_mark(id, true);
    }
};
UnavailableObservation denied() { return {ObservationState::Denied, {ErrorDomain::Adapter, 13, "refresh.denied"}}; }
}
CK_TEST(pane_refresh_integrates_sealed_successors_and_releases_the_scan_pin_after_preparation) {
    Fixture f; auto previous = f.pane.held_entries(); f.fs.metadata(f.file).logical_size = Observation<std::uint64_t>::available(99);
    auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)); CK_CHECK(raw->retains_scan_directory() && f.fs.live_cursors() == 2);
    PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock()); auto prepared = finish(job);
    CK_CHECK(!prepared->retains_scan_directory() && f.fs.live_cursors() == 1 && prepared->binding_issues().empty());
    CK_CHECK(f.pane.publish(*prepared) == PanePublish::Applied && f.pane.focused() == f.id && f.pane.marked(f.id));
    CK_CHECK(f.pane.row(f.id) && take(f.pane.row(f.id)->entry.metadata.logical_size) == 99 && take(f.held->metadata().logical_size) == 12);
    CK_CHECK(f.pane.sources().state == PaneSourcesState::Ready && f.pane.binding_issues().empty());
}
CK_TEST(pane_refresh_replacement_publishes_new_metadata_but_invalidates_old_marks_and_focus) {
    Fixture f; auto previous = f.pane.held_entries(); auto replacement = f.fs.make_node(ObjectKind::RegularFile); f.fs.put(f.root, name("a"), replacement);
    PaneRefreshJob job(scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)), previous, StorageModel::LinuxStatx, {}, f.fs.clock());
    auto prepared = finish(job); CK_CHECK(prepared->binding_issues().size() == 1);
    CK_CHECK(f.pane.publish(*prepared) == PanePublish::Applied && !f.pane.row(f.id) && !f.pane.focused());
    CK_CHECK(f.pane.sources().state == PaneSourcesState::SelectionInvalidated && f.pane.invalidated_selection().marks == 1);
    CK_CHECK(f.pane.binding_issues().size() == 1 && f.pane.binding_issues()[0].detail.state == ObservationState::Changed);
    const auto pending = f.pane.begin_listing(PaneListingPurpose::Refresh); CK_CHECK(f.pane.cancel_listing(pending) && f.pane.binding_issues().size() == 1);
    f.pane.clear_marks(); CK_CHECK(f.pane.binding_issues().empty());
}
CK_TEST(pane_refresh_parent_mismatch_or_unknown_identity_never_queries_the_old_child) {
    for (bool unknown : {false, true}) {
        Fixture f; auto previous = f.pane.held_entries();
        if (unknown) f.fs.metadata(f.root).identity = {};
        else { auto root = f.fs.make_node(ObjectKind::Directory); f.fs.bind(path(), root); f.fs.put(root, name("a"), f.file); }
        auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)); const auto calls = f.fs.calls().size();
        PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock()); auto prepared = finish(job);
        CK_CHECK(f.fs.calls().size() == calls && prepared->binding_issues().size() == 1);
        CK_CHECK(f.pane.publish(*prepared) == PanePublish::Applied && f.pane.sources().state == PaneSourcesState::SelectionInvalidated);
        CK_CHECK(f.pane.binding_issues()[0].detail.state == (unknown ? ObservationState::Unknown : ObservationState::Changed));
    }
}
CK_TEST(pane_refresh_unlisted_bindings_report_uncertainty_without_claiming_native_absence) {
    Fixture f; auto previous = f.pane.held_entries(); f.fs.erase(f.root, name("a"));
    auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)); const auto calls = f.fs.calls().size();
    PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock()); auto prepared = finish(job);
    CK_CHECK(f.fs.calls().size() == calls && prepared->binding_issues().size() == 1);
    CK_CHECK(f.pane.publish(*prepared) == PanePublish::Applied && f.pane.size() == 0 && f.pane.invalidated_selection().marks == 1);
    CK_CHECK(f.pane.binding_issues()[0].detail.state == ObservationState::Unknown && f.pane.binding_issues()[0].detail.error.operation == "pane_refresh.not_listed");
}
CK_TEST(pane_refresh_denials_remain_named_diagnostics_and_never_retain_unproven_sources) {
    for (bool old_handle : {false, true}) {
        Fixture f; auto previous = f.pane.held_entries(); auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh));
        if (old_handle) { f.fs.enqueue(Operation::InspectHandle, {}); f.fs.enqueue(Operation::InspectHandle, {denied(), {}, {}, false}); }
        else f.fs.enqueue(Operation::OpenChild, {denied(), {}, {}, false});
        PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock()); auto prepared = finish(job);
        CK_CHECK(f.pane.publish(*prepared) == PanePublish::Applied && f.pane.sources().state == PaneSourcesState::SelectionInvalidated);
        CK_CHECK(f.pane.binding_issues().size() == 1 && f.pane.binding_issues()[0].detail.error == denied().error);
    }
}
CK_TEST(pane_refresh_independent_observations_cannot_forge_predecessor_families) {
    Fixture f; auto independent = take(HeldChild::observe(f.parent, name("a"))); std::vector<HeldChild::Pointer> previous{independent};
    PaneRefreshJob job(scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)), previous, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(f.pane.publish(*finish(job)) == PanePublish::Applied && !f.pane.row(f.id) && f.pane.sources().state == PaneSourcesState::SelectionInvalidated);
}
CK_TEST(pane_refresh_time_yield_retains_progress_without_repeating_a_slow_observation) {
    Fixture f; auto previous = f.pane.held_entries(); auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh));
    f.fs.enqueue(Operation::OpenChild, {{}, std::chrono::milliseconds(20), {}, false});
    PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && !job.completed().value() && f.pane.listing_pending());
    const auto calls = f.fs.calls().size(); auto prepared = finish(job);
    CK_CHECK(f.fs.calls().size() == calls && f.pane.publish(*prepared) == PanePublish::Applied && f.pane.marked(f.id));
}
CK_TEST(pane_refresh_staging_is_step_bounded_and_never_exposes_partial_rows) {
    Fixture f; for (int i = 0; i < 299; ++i) f.fs.put(f.root, name(("x" + std::to_string(i)).c_str()), f.fs.make_node(ObjectKind::RegularFile));
    auto previous = f.pane.held_entries(); auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)); const auto calls = f.fs.calls().size();
    PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock());
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && !job.completed().value() && f.fs.calls().size() == calls && f.pane.size() == 1);
    CK_CHECK(f.pane.publish(*finish(job)) == PanePublish::Applied && f.pane.size() == 300 && f.pane.marked(f.id));
}
CK_TEST(pane_refresh_cancellation_releases_newly_opened_children_and_resumes_without_binding_loss) {
    for (int stage = 0; stage < 3; ++stage) {
        Fixture f; auto previous = f.pane.held_entries(); auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh));
        std::stop_source stop;
        if (stage == 2) f.fs.enqueue(Operation::InspectHandle, {});
        f.fs.enqueue(stage == 0 ? Operation::OpenChild : Operation::InspectHandle, {{}, {}, [&] { stop.request_stop(); }, false});
        PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock());
        CK_CHECK(job.advance(stop.get_token()) == PaneListingJobState::Paused && f.fs.live_entries() == 1 && !job.completed().value());
        const auto calls = f.fs.calls().size(); CK_CHECK(job.advance(stop.get_token()) == PaneListingJobState::Paused && f.fs.calls().size() == calls);
        CK_CHECK(f.pane.publish(*finish(job)) == PanePublish::Applied && f.pane.marked(f.id) && f.pane.binding_issues().empty());
    }
}
CK_TEST(pane_refresh_abandonment_discards_private_successors_and_generation_gate_rejects_late_results) {
    Fixture f; auto previous = f.pane.held_entries(); auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh));
    PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock()); auto prepared = finish(job);
    CK_CHECK(job.abandon() && !job.abandon() && !job.completed().value());
    CK_CHECK(f.pane.cancel_listing(job.request()) && f.pane.publish(*prepared) == PanePublish::Stale && f.pane.marked(f.id));
    const auto calls = f.fs.calls().size(); CK_CHECK(job.advance() == PaneListingJobState::Abandoned && f.fs.calls().size() == calls);
}
CK_TEST(pane_refresh_clock_failure_and_exceptions_poison_staging_and_keep_old_pane) {
    for (bool exception : {false, true}) {
        Fixture f; auto previous = f.pane.held_entries(); auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh));
        f.fs.enqueue(Operation::OpenChild, {{}, std::chrono::milliseconds(20), {}, false});
        PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock());
        if (exception) {
            f.fs.enqueue(Operation::InspectHandle, {{}, {}, {}, true}); bool threw = false;
            try { (void)job.advance(); } catch (const std::runtime_error&) { threw = true; } CK_CHECK(threw);
        } else { CK_CHECK(job.advance() == PaneListingJobState::Collecting); f.fs.clock()->advance(-std::chrono::nanoseconds(1)); (void)job.advance(); }
        CK_CHECK(job.state() == PaneListingJobState::Failed && !job.abandon() && f.fs.live_entries() == 1 && f.fs.live_cursors() == 1);
        CK_CHECK(job.failure() && f.pane.fail_listing(job.request(), *job.failure()) && f.pane.marked(f.id) && f.pane.focused() == f.id);
    }
}
CK_TEST(pane_refresh_limits_and_invalid_previous_bindings_fail_before_native_revalidation) {
    for (int mode = 0; mode < 4; ++mode) {
        Fixture f; auto previous = f.pane.held_entries(); PaneLimits limits;
        if (mode == 0) previous.push_back(previous[0]);
        if (mode == 1) previous[0].reset();
        if (mode >= 2) { f.fs.put(f.root, name("bb"), f.fs.make_node(ObjectKind::RegularFile)); if (mode == 2) limits.native_name_bytes = 1; else limits.display_name_bytes = 1; }
        auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)); const auto calls = f.fs.calls().size();
        PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, limits, f.fs.clock());
        CK_CHECK(job.advance() == PaneListingJobState::Failed && !job.completed().value() && f.fs.calls().size() == calls && f.pane.marked(f.id));
    }
}
CK_TEST(pane_refresh_requires_explicit_scan_retention_and_valid_configuration) {
    Fixture f; auto previous = f.pane.held_entries(); auto request = f.pane.begin_listing(PaneListingPurpose::Refresh);
    for (int mode = 0; mode < 5; ++mode) {
        auto raw = scan(f.fs, request, mode == 0 ? ListingDirectoryRetention::ReleaseAtEnd : ListingDirectoryRetention::RetainForRefresh);
        bool rejected = false;
        try { PaneRefreshJob job(std::move(raw), previous, mode == 1 ? static_cast<StorageModel>(999) : StorageModel::LinuxStatx,
            {}, mode == 2 ? std::shared_ptr<const MonotonicClock>{} : f.fs.clock(), mode == 3 ? MonotonicClock::Duration{} : mode == 4 ? std::chrono::seconds(2) : std::chrono::milliseconds(20)); }
        catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected);
    }
}
CK_TEST(pane_refresh_hidden_flags_and_hard_link_names_preserve_exact_mark_semantics) {
    Fixture f; f.fs.put(f.root, name("b"), f.file); auto b = take(HeldChild::observe(f.parent, name("b")));
    CK_CHECK(f.pane.publish(f.pane.begin_listing(PaneListingPurpose::Navigate),
        {PaneEntry::from_held(f.held, Observation<bool>::available(false)), PaneEntry::from_held(b, Observation<bool>::available(false))}) == PanePublish::Applied);
    f.pane.mark_visible(true); auto previous = f.pane.held_entries();
    f.fs.metadata(f.file).bsd_flags = Observation<std::uint32_t>::available(0x8000);
    PaneRefreshJob job(scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)), previous, StorageModel::DarwinStatFlags, {}, f.fs.clock());
    CK_CHECK(f.pane.publish(*finish(job)) == PanePublish::Applied && f.pane.held_entries().size() == 2 && f.pane.visible_size() == 0);
    CK_CHECK(f.pane.sources().state == PaneSourcesState::HiddenMarksNeedReview && f.pane.sources().hidden_count == 2);
    CK_CHECK(pane_hidden_state(name("a"), {}, static_cast<StorageModel>(999)).state() == ObservationState::Failed);
}

CK_TEST(pane_refresh_superseded_generations_do_not_publish_or_revoke_a_newer_request) {
    Fixture f; auto previous = f.pane.held_entries();
    PaneRefreshJob job(scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh)), previous, StorageModel::LinuxStatx, {}, f.fs.clock());
    const auto current = f.pane.begin_listing(PaneListingPurpose::Refresh); const auto result = finish(job);
    CK_CHECK(f.pane.publish(*result) == PanePublish::Stale && f.pane.listing_pending() && f.pane.marked(f.id));
    CK_CHECK(!f.pane.cancel_listing(job.request()) && f.pane.cancel_listing(current));
}
CK_TEST(pane_listing_rejects_unknown_directory_retention_without_reading_or_leaking_the_cursor) {
    Fixture f; auto listing = take(DirectoryListing::open(f.fs, path(), {}, f.fs.clock())); const auto calls = f.fs.calls().size();
    bool rejected = false;
    try { PaneListingJob job(f.pane.begin_listing(PaneListingPurpose::Refresh), std::move(listing), StorageModel::LinuxStatx, {}, static_cast<ListingDirectoryRetention>(999)); }
    catch (const std::invalid_argument&) { rejected = true; }
    CK_CHECK(rejected && f.fs.live_cursors() == 1 && f.fs.calls().size() == calls);
}

CK_TEST(pane_refresh_allocation_failure_uses_a_nonallocating_poison_record_and_releases_staging) {
    Fixture f; auto previous = f.pane.held_entries(); auto raw = scan(f.fs, f.pane.begin_listing(PaneListingPurpose::Refresh));
    f.fs.enqueue(Operation::InspectHandle, {{}, {}, [] { throw std::bad_alloc{}; }, false});
    PaneRefreshJob job(std::move(raw), previous, StorageModel::LinuxStatx, {}, f.fs.clock());
    bool threw = false; try { (void)job.advance(); } catch (const std::bad_alloc&) { threw = true; }
    CK_CHECK(threw && job.state() == PaneListingJobState::Failed && f.fs.live_entries() == 1 && f.fs.live_cursors() == 1);
    CK_CHECK(job.failure() && job.failure()->error.operation.empty() && job.failure()->error.code == static_cast<std::int64_t>(AdapterError::InterruptedRead));
    const auto calls = f.fs.calls().size(); CK_CHECK(job.advance() == PaneListingJobState::Failed && f.fs.calls().size() == calls && f.pane.marked(f.id));
}
