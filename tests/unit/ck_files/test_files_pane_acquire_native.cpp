// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_acquire_job.hpp"
#include "pane_refresh_job.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <stdexcept>
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native acquisition observation unavailable");
    return std::move(value).take_value();
}
NativeName name(const char* text) {
#ifdef _WIN32
    std::u16string units; while (*text) units.push_back(static_cast<char16_t>(static_cast<unsigned char>(*text++)));
    return *NativeName::from_windows(std::move(units));
#else
    return *NativeName::from_posix(text);
#endif
}
constexpr StorageModel model() {
#ifdef _WIN32
    return StorageModel::WindowsHandleAttributes;
#elif defined(__APPLE__)
    return StorageModel::DarwinStatFlags;
#else
    return StorageModel::LinuxStatx;
#endif
}
void write(const fs::path& file, const char* text) { std::ofstream out(file, std::ios::binary); out << text; out.close(); if (!out) throw std::runtime_error("cannot write native acquisition fixture"); }
PaneListingJob::Result finish(auto& job) {
    for (int i = 0; i < 100; ++i) {
        if (job.advance() == PaneListingJobState::Complete) return take(job.completed());
        if (job.state() == PaneListingJobState::Failed) throw std::runtime_error("native acquisition job failed");
    }
    throw std::runtime_error("native acquisition fixture exceeded step budget");
}
PaneListingJob::Result scan(const NativeFileSystem& filesystem, const fs::path& path, PaneListingRequest request) {
    PaneListingJob job(request, take(DirectoryListing::open(filesystem, path)), model(), {}, ListingDirectoryRetention::RetainForRefresh);
    return finish(job);
}
}
CK_TEST(native_pane_acquire_binds_initial_selection_then_refreshes_the_same_live_family) {
    const ck::testing::ScratchDirectory scratch("files-pane-acquire-native"); NativeFileSystem filesystem; PaneModel pane;
    write(scratch.path() / "a", "old"); auto raw = scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Navigate));
    write(scratch.path() / "a", "fresh"); PaneAcquireJob job(std::move(raw), {name("a")}, model());
    const auto result = finish(job); CK_CHECK(pane.publish(*result) == PanePublish::Applied && pane.held_entries().size() == 1 && result->binding_issues().empty());
    const auto id = *pane.focused(); CK_CHECK(take(pane.row(id)->entry.metadata.logical_size) == 5 && pane.set_mark(id, true));
    PaneRefreshJob refresh(scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Refresh)), pane.held_entries(), model());
    CK_CHECK(pane.publish(*finish(refresh)) == PanePublish::Applied && pane.marked(id) && pane.focused() == id);
}
CK_TEST(native_pane_acquire_uses_the_scanned_directory_after_its_path_is_replaced) {
    const ck::testing::ScratchDirectory scratch("files-pane-acquire-parent"); NativeFileSystem filesystem; PaneModel pane;
    const auto path = scratch.path() / "directory", retired = scratch.path() / "retired"; fs::create_directory(path); write(path / "a", "old");
    auto raw = scan(filesystem, path, pane.begin_listing(PaneListingPurpose::Navigate)); fs::rename(path, retired); fs::create_directory(path); write(path / "a", "replacement");
    PaneAcquireJob job(std::move(raw), {name("a")}, model()); const auto result = finish(job);
    CK_CHECK(result->binding_issues().empty() && result->entries()[0].held_child() && take(result->entries()[0].metadata.logical_size) == 3);
    CK_CHECK(pane.publish(*result) == PanePublish::Applied && pane.held_entries().size() == 1);
}
CK_TEST(native_pane_acquire_reports_removed_entries_without_substituting_another_row) {
    const ck::testing::ScratchDirectory scratch("files-pane-acquire-gone"); NativeFileSystem filesystem; PaneModel pane;
    write(scratch.path() / "a", "old"); write(scratch.path() / "b", "retained");
    auto raw = scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Navigate)); fs::remove(scratch.path() / "a");
    PaneAcquireJob job(std::move(raw), {name("a"), name("b")}, model()); const auto result = finish(job);
    CK_CHECK(result->entries().size() == 2 && result->binding_issues().size() == 1 && result->binding_issues()[0].name == name("a"));
    CK_CHECK(result->binding_issues()[0].detail.state == ObservationState::Gone && pane.publish(*result) == PanePublish::Applied);
    CK_CHECK(pane.held_entries().size() == 1 && pane.held_entries()[0]->name() == name("b"));
}
CK_TEST(native_pane_acquire_keeps_separate_hard_link_names_and_refuses_cached_selection_continuity) {
    const ck::testing::ScratchDirectory scratch("files-pane-acquire-links"); NativeFileSystem filesystem; PaneModel pane;
    write(scratch.path() / "a", "old"); fs::create_hard_link(scratch.path() / "a", scratch.path() / "b");
    auto raw = scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Navigate)); CK_CHECK(pane.publish(*raw) == PanePublish::Applied);
    const auto old = *pane.focused(); pane.mark_visible(true);
    PaneAcquireJob job(scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Refresh)), {name("a"), name("b")}, model());
    const auto result = finish(job); CK_CHECK(pane.publish(*result) == PanePublish::Applied && pane.held_entries().size() == 2 && !pane.row(old));
    const auto held = pane.held_entries(); CK_CHECK(compare_identity(take(held[0]->metadata().identity), take(held[1]->metadata().identity)) == IdentityComparison::Same);
    CK_CHECK(held[0]->name() != held[1]->name() && pane.sources().state == PaneSourcesState::SelectionInvalidated && pane.invalidated_selection().marks == 2);
}
CK_TEST(native_pane_preparation_refreshes_marked_objects_and_acquires_new_rows_in_one_publication) {
    const ck::testing::ScratchDirectory scratch("files-pane-prepare-mixed"); NativeFileSystem filesystem; PaneModel pane;
    write(scratch.path() / "a", "old"); PaneAcquireJob initial(scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Navigate)), {name("a")}, model());
    CK_CHECK(pane.publish(*finish(initial)) == PanePublish::Applied); const auto id = *pane.focused(); pane.set_mark(id, true);
    write(scratch.path() / "b", "new"); const auto previous = pane.held_entries();
    PaneRefreshJob refresh(scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Refresh)), previous, model(), {}, system_monotonic_clock(),
        std::chrono::milliseconds(20), ListingDirectoryRetention::RetainForRefresh);
    PaneAcquireJob acquire(finish(refresh), {name("a"), name("b")}, model()); const auto result = finish(acquire);
    CK_CHECK(!result->retains_scan_directory() && result->binding_issues().empty() && pane.publish(*result) == PanePublish::Applied);
    CK_CHECK(pane.marked(id) && pane.focused() == id && pane.held_entries().size() == 2 && pane.invalidated_selection().marks == 0);
}
CK_TEST(native_pane_preparation_preserves_replacement_diagnostics_while_acquiring_another_file) {
    const ck::testing::ScratchDirectory scratch("files-pane-prepare-replace"); NativeFileSystem filesystem; PaneModel pane;
    write(scratch.path() / "a", "old"); PaneAcquireJob initial(scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Navigate)), {name("a")}, model());
    CK_CHECK(pane.publish(*finish(initial)) == PanePublish::Applied); const auto id = *pane.focused(); pane.set_mark(id, true); const auto previous = pane.held_entries();
    fs::rename(scratch.path() / "a", scratch.path() / "retired"); write(scratch.path() / "a", "replacement"); write(scratch.path() / "b", "new");
    PaneRefreshJob refresh(scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Refresh)), previous, model(), {}, system_monotonic_clock(),
        std::chrono::milliseconds(20), ListingDirectoryRetention::RetainForRefresh);
    PaneAcquireJob acquire(finish(refresh), {name("b")}, model()); const auto result = finish(acquire);
    CK_CHECK(result->binding_issues().size() == 1 && result->binding_issues()[0].detail.state == ObservationState::Changed);
    CK_CHECK(pane.publish(*result) == PanePublish::Applied && !pane.row(id) && pane.invalidated_selection().marks == 1 && pane.held_entries().size() == 2);
}
