// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "simulated_filesystem.hpp"
#include "pane_listing_job.hpp"
#include "cvision/testing/cktest.hpp"
#include <type_traits>
namespace {
using namespace ck::files;
using FileSystem = ck::testing::SimulatedFileSystem;
using Operation = FileSystem::Operation;
std::filesystem::path path() {
#ifdef _WIN32
    return L"V:/simulated-files";
#else
    return "/simulated-files";
#endif
}
NativeName name(const char* text) { return *NativeName::from_posix(text); }
template<class T> T take(Observation<T> result) {
    if (!result.value()) throw std::runtime_error("simulated fixture observation unavailable");
    return std::move(result).take_value();
}
UnavailableObservation denied() { return {ObservationState::Denied, {ErrorDomain::Adapter, 53, "injected.denied"}}; }
FileSystem::Pointer root(FileSystem& fs) { auto node = fs.make_node(ObjectKind::Directory); fs.bind(path(), node); return node; }
PaneListingJob::Result finish(PaneListingJob& job) {
    for (int i = 0; i < 64; ++i) {
        if (job.advance() == PaneListingJobState::Complete) return take(job.completed());
        if (job.state() == PaneListingJobState::Failed) throw std::runtime_error("simulated listing failed");
    }
    throw std::runtime_error("simulated listing exceeded step budget");
}
}
CK_TEST(simulated_filesystem_streams_raw_posix_and_windows_names_into_panes_without_host_io) {
    for (bool windows : {false, true}) {
        FileSystem fs; auto directory = root(fs);
        auto raw = windows ? *NativeName::from_windows(std::u16string{u'x', char16_t{0xd800}}) : *NativeName::from_posix(std::string{"x\xff", 2});
        fs.put(directory, raw, fs.make_node(ObjectKind::RegularFile));
        PaneModel pane;
        PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), take(DirectoryListing::open(fs, path(), {}, fs.clock())), StorageModel::Unknown);
        CK_CHECK(pane.publish(*finish(job)) == PanePublish::Applied && pane.size() == 1);
        CK_CHECK(pane.visible_row(0) && pane.visible_row(0)->entry.name == raw && pane.visible_row(0)->display_name == raw.display_name());
        CK_CHECK(fs.live_cursors() == 0 && fs.live_entries() == 0);
    }
}
CK_TEST(simulated_path_replacement_keeps_old_cursor_and_child_handles_on_original_objects) {
    FileSystem fs; auto old = root(fs); auto file = fs.make_node(ObjectKind::RegularFile); fs.put(old, name("a"), file);
    auto cursor = take(fs.open_directory(path())); auto held = take(cursor->open_child(name("a")));
    const auto original = take(take(held->inspect_basic()).identity);
    auto replacement = fs.make_node(ObjectKind::Directory); fs.bind(path(), replacement); fs.unbind(path());
    CK_CHECK(fs.open_directory(path()).state() == ObservationState::Gone);
    CK_CHECK(cursor->read(4).names == std::vector<NativeName>{name("a")});
    fs.erase(old, name("a")); CK_CHECK(cursor->open_child(name("a")).state() == ObservationState::Gone);
    CK_CHECK(compare_identity(take(take(held->inspect_basic()).identity), original) == IdentityComparison::Same);
    cursor.reset(); CK_CHECK(fs.live_cursors() == 0 && fs.live_entries() == 1);
    held.reset(); CK_CHECK(fs.live_entries() == 0);
}
CK_TEST(simulated_enumeration_names_and_live_membership_can_disagree_without_hiding_gone_rows) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("gone"), fs.make_node(ObjectKind::RegularFile));
    auto listing = take(DirectoryListing::open(fs, path(), {}, fs.clock()));
    fs.erase(directory, name("gone")); fs.put(directory, name("later"), fs.make_node(ObjectKind::RegularFile));
    const auto batch = listing->read();
    CK_CHECK(batch.state == DirectoryReadState::End && batch.entries.size() == 1);
    CK_CHECK(batch.entries.size() == 1 && batch.entries[0].name == name("gone") && batch.entries[0].metadata.state() == ObservationState::Gone);
}
CK_TEST(simulated_faults_retain_unknown_denied_and_native_error_details) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    fs.enqueue(Operation::OpenDirectory, {denied(), {}, {}, false});
    const auto refused = DirectoryListing::open(fs, path(), {}, fs.clock());
    CK_CHECK(refused.state() == ObservationState::Denied && refused.unavailable_detail()->error == denied().error);
    fs.enqueue(Operation::InspectHandle, {UnavailableObservation{ObservationState::Unknown, {ErrorDomain::Adapter, 54, "unknown.inspect"}}, {}, {}, false});
    auto listing = take(DirectoryListing::open(fs, path(), {}, fs.clock())); const auto batch = listing->read();
    CK_CHECK(batch.entries.size() == 1 && batch.entries[0].metadata.state() == ObservationState::Unknown);
    CK_CHECK(batch.entries.size() == 1 && batch.entries[0].metadata.unavailable_detail() && batch.entries[0].metadata.unavailable_detail()->error.operation == "unknown.inspect" && fs.live_entries() == 0);
}
CK_TEST(simulated_mid_scan_failure_poisoning_discards_private_pane_rows_and_closes_handles) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    fs.enqueue(Operation::ReadDirectory, {}); fs.enqueue(Operation::ReadDirectory, {denied(), {}, {}, false});
    PaneModel pane; PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate),
        take(DirectoryListing::open(fs, path(), {1,128,128}, fs.clock())), StorageModel::Unknown);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && job.advance() == PaneListingJobState::Failed);
    CK_CHECK(!job.completed().value() && job.failure() && job.failure()->error == denied().error && pane.size() == 0);
    const auto calls = fs.calls().size(); CK_CHECK(job.advance() == PaneListingJobState::Failed && fs.calls().size() == calls);
    CK_CHECK(fs.live_cursors() == 0 && fs.live_entries() == 0);
}
CK_TEST(simulated_side_effect_cancellation_closes_prepared_objects_and_can_resume_exact_name) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    std::stop_source stop; fs.enqueue(Operation::OpenChild, {{}, {}, [&] { stop.request_stop(); }, false});
    auto listing = take(DirectoryListing::open(fs, path(), {}, fs.clock()));
    CK_CHECK(listing->read(stop.get_token()).state == DirectoryReadState::Cancelled && fs.live_entries() == 0);
    const auto resumed = listing->read();
    CK_CHECK(resumed.state == DirectoryReadState::End && resumed.entries.size() == 1 && resumed.entries[0].name == name("a"));
}
CK_TEST(simulated_latency_time_slices_keep_prepared_handles_and_abandonment_releases_them) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    fs.enqueue(Operation::OpenChild, {{}, std::chrono::milliseconds(20), {}, false});
    PaneModel pane; PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate),
        take(DirectoryListing::open(fs, path(), {}, fs.clock())), StorageModel::Unknown);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && fs.live_entries() == 1 && fs.live_cursors() == 1);
    CK_CHECK(job.abandon() && fs.live_entries() == 0 && fs.live_cursors() == 0);
    CK_CHECK(pane.cancel_listing(job.request()));
}
CK_TEST(simulated_exception_poisoning_releases_private_discovery_and_prevents_retries) {
    FileSystem fs; auto directory = root(fs); fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    fs.enqueue(Operation::InspectHandle, {{}, {}, {}, true});
    PaneModel pane; PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate),
        take(DirectoryListing::open(fs, path(), {}, fs.clock())), StorageModel::Unknown);
    bool caught = false; try { (void)job.advance(); } catch (const std::runtime_error&) { caught = true; }
    CK_CHECK(caught && job.state() == PaneListingJobState::Failed && fs.live_entries() == 0 && fs.live_cursors() == 0);
    const auto calls = fs.calls().size(); CK_CHECK(job.advance() == PaneListingJobState::Failed && fs.calls().size() == calls);
}
CK_TEST(simulated_directory_children_own_independent_cursors_and_never_follow_link_nodes) {
    FileSystem fs; auto directory = root(fs); auto child = fs.make_node(ObjectKind::Directory);
    fs.put(directory, name("child"), child); fs.put(child, name("file"), fs.make_node(ObjectKind::RegularFile));
    fs.put(directory, name("link"), fs.make_node(ObjectKind::SymbolicLink));
    auto parent = take(fs.open_directory(path())); auto first = take(parent->open_directory_child(name("child")));
    auto second = take(parent->open_directory_child(name("child"))); parent.reset();
    CK_CHECK(first->read(8).names == std::vector<NativeName>{name("file")} && second->read(8).names == std::vector<NativeName>{name("file")});
    CK_CHECK(fs.live_cursors() == 2); auto fresh = take(fs.open_directory(path()));
    CK_CHECK(!fresh->open_directory_child(name("link")).value());
    CK_CHECK(take(take(take(fresh->open_child(name("link")))->inspect_basic()).kind) == ObjectKind::SymbolicLink);
}
CK_TEST(simulated_bindings_reject_foreign_nodes_relative_paths_and_directory_cycles) {
    FileSystem fs, foreign; auto directory = root(fs); auto child = fs.make_node(ObjectKind::Directory); fs.put(directory, name("child"), child);
    bool cycle = false, other = false, relative = false;
    try { fs.put(child, name("cycle"), directory); } catch (const std::invalid_argument&) { cycle = true; }
    try { fs.put(directory, name("foreign"), foreign.make_node(ObjectKind::RegularFile)); } catch (const std::invalid_argument&) { other = true; }
    try { fs.bind("relative", directory); } catch (const std::invalid_argument&) { relative = true; }
    CK_CHECK(cycle && other && relative && fs.calls().empty());
}
CK_TEST(simulated_held_refresh_qualifies_continuity_and_preserves_pane_selection) {
    FileSystem fs; auto directory = root(fs); auto object = fs.make_node(ObjectKind::RegularFile); fs.put(directory, name("a"), object);
    std::shared_ptr<const DirectoryCursor> parent(take(fs.open_directory(path())));
    auto held = take(HeldChild::observe(parent, name("a")));
    PaneModel pane; CK_CHECK(pane.publish(pane.begin_listing(PaneListingPurpose::Navigate),
        {PaneEntry::from_held(held, Observation<bool>::available(false))}) == PanePublish::Applied);
    const auto id = pane.focused(); CK_CHECK(id && pane.set_mark(*id, true));
    fs.metadata(object).logical_size = Observation<std::uint64_t>::available(77);
    auto refreshed = take(held->refresh()); CK_CHECK(refreshed->continues(*held));
    CK_CHECK(pane.publish(pane.begin_listing(PaneListingPurpose::Refresh),
        {PaneEntry::from_held(refreshed, Observation<bool>::available(false))}) == PanePublish::Applied);
    CK_CHECK(id && pane.focused() == id && pane.marked(*id) && pane.row(*id));
    if (!id || !pane.row(*id)) throw std::runtime_error("simulated refresh lost pane row");
    CK_CHECK(take(pane.row(*id)->entry.metadata.logical_size) == 77);
}
CK_TEST(simulated_held_refresh_detects_replacement_even_with_an_old_open_handle) {
    FileSystem fs; auto directory = root(fs); auto old = fs.make_node(ObjectKind::RegularFile); fs.put(directory, name("a"), old);
    std::shared_ptr<const DirectoryCursor> parent(take(fs.open_directory(path())));
    auto held = take(HeldChild::observe(parent, name("a")));
    fs.put(directory, name("a"), fs.make_node(ObjectKind::RegularFile));
    auto refreshed = take(held->refresh());
    CK_CHECK(!refreshed->continues(*held) && take(refreshed->previous_continuity()) == IdentityComparison::Different);
    CK_CHECK(fs.live_entries() == 2 && fs.live_cursors() == 1);
    held.reset(); refreshed.reset(); parent.reset(); CK_CHECK(fs.live_entries() == 0 && fs.live_cursors() == 0);
}
