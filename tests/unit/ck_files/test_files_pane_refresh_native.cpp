// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_refresh_job.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <stdexcept>
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native pane-refresh observation unavailable");
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
void write(const fs::path& file, const char* text) { std::ofstream out(file, std::ios::binary); out << text; out.close(); if (!out) throw std::runtime_error("cannot write native refresh fixture"); }
PaneListingJob::Result finish(auto& job) {
    for (int i = 0; i < 100; ++i) {
        if (job.advance() == PaneListingJobState::Complete) return take(job.completed());
        if (job.state() == PaneListingJobState::Failed) throw std::runtime_error("native refresh job failed");
    }
    throw std::runtime_error("native refresh fixture exceeded step budget");
}
PaneListingJob::Result scan(const NativeFileSystem& filesystem, const fs::path& path, PaneListingRequest request) {
    PaneListingJob job(request, take(DirectoryListing::open(filesystem, path)), model(), {}, ListingDirectoryRetention::RetainForRefresh);
    return finish(job);
}
HeldChild::Pointer seed(PaneModel& pane, const NativeFileSystem& filesystem, const fs::path& path) {
    std::shared_ptr<const DirectoryCursor> parent(take(filesystem.open_directory(path)));
    auto held = take(HeldChild::observe(std::move(parent), name("a")));
    if (pane.publish(pane.begin_listing(PaneListingPurpose::Navigate), {PaneEntry::from_held(held, Observation<bool>::available(false))}) != PanePublish::Applied)
        throw std::runtime_error("cannot seed native held pane");
    pane.mark_visible(true); return held;
}
}
CK_TEST(native_pane_refresh_preserves_bound_focus_and_marks_with_live_current_metadata) {
    const ck::testing::ScratchDirectory scratch("files-pane-refresh-native"); NativeFileSystem filesystem;
    write(scratch.path() / "a", "old"); PaneModel pane; const auto held = seed(pane, filesystem, scratch.path());
    const auto id = pane.focused(); auto previous = pane.held_entries(); write(scratch.path() / "a", "changed!");
    PaneRefreshJob job(scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Refresh)), previous, model());
    const auto prepared = finish(job); CK_CHECK(!prepared->retains_scan_directory() && prepared->binding_issues().empty());
    CK_CHECK(pane.publish(*prepared) == PanePublish::Applied && id && pane.focused() == id && pane.marked(*id));
    if (!id || !pane.row(*id)) throw std::runtime_error("native refresh lost bound row");
    CK_CHECK(take(pane.row(*id)->entry.metadata.logical_size) == 8 && take(held->metadata().logical_size) == 3);
    CK_CHECK(pane.sources().state == PaneSourcesState::Ready);
}
CK_TEST(native_pane_refresh_detects_replacement_while_the_previous_object_remains_held) {
    const ck::testing::ScratchDirectory scratch("files-pane-refresh-replace"); NativeFileSystem filesystem;
    write(scratch.path() / "a", "old"); PaneModel pane; const auto held = seed(pane, filesystem, scratch.path()); auto previous = pane.held_entries();
    fs::rename(scratch.path() / "a", scratch.path() / "retired"); write(scratch.path() / "a", "replacement");
    PaneRefreshJob job(scan(filesystem, scratch.path(), pane.begin_listing(PaneListingPurpose::Refresh)), previous, model());
    const auto prepared = finish(job);
    CK_CHECK(prepared->binding_issues().size() == 1 && prepared->binding_issues()[0].detail.state == ObservationState::Changed);
    CK_CHECK(pane.publish(*prepared) == PanePublish::Applied && pane.sources().state == PaneSourcesState::SelectionInvalidated);
    CK_CHECK(pane.invalidated_selection().marks == 1 && pane.invalidated_selection().cursor && !pane.focused());
    CK_CHECK(take(held->metadata().logical_size) == 3);
}
CK_TEST(native_pane_refresh_refuses_cross_directory_binding_despite_equal_component_names) {
    const ck::testing::ScratchDirectory scratch("files-pane-refresh-directory"); NativeFileSystem filesystem;
    const auto first = scratch.path() / "first", second = scratch.path() / "second"; fs::create_directory(first); fs::create_directory(second);
    write(first / "a", "old"); write(second / "a", "different"); PaneModel pane; const auto held = seed(pane, filesystem, first); auto previous = pane.held_entries();
    PaneRefreshJob job(scan(filesystem, second, pane.begin_listing(PaneListingPurpose::Refresh)), previous, model());
    const auto prepared = finish(job); CK_CHECK(prepared->binding_issues().size() == 1 && prepared->binding_issues()[0].detail.error.operation == "pane_refresh.directory_identity");
    CK_CHECK(pane.publish(*prepared) == PanePublish::Applied && pane.sources().state == PaneSourcesState::SelectionInvalidated && pane.held_entries().empty());
    CK_CHECK(take(held->metadata().logical_size) == 3);
}
