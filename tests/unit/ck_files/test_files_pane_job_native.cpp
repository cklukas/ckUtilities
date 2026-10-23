// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pane_listing_job.hpp"
#include "manual_monotonic_clock.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <stdexcept>
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/stat.h>
#endif
namespace {
using namespace ck::files; namespace fs = std::filesystem;
#if defined(_WIN32)
constexpr auto model = StorageModel::WindowsHandleAttributes;
#elif defined(__APPLE__)
constexpr auto model = StorageModel::DarwinStatFlags;
#else
constexpr auto model = StorageModel::LinuxStatx;
#endif
template<class T> const T& required(const Observation<T>& value) { if (!value.value()) throw std::runtime_error("native pane job observation unavailable"); return *value.value(); }
void file(const fs::path& path) { std::ofstream out(path); out << "fixture"; out.close(); if (!out) throw std::runtime_error("cannot write pane job fixture"); }
std::unique_ptr<DirectoryListing> listing(const NativeFileSystem& filesystem, const fs::path& path) {
    auto result = DirectoryListing::open(filesystem, path, {1, 1024, 1024}, std::make_shared<ck::testing::ManualMonotonicClock>());
    (void)required(result); return std::move(result).take_value();
}
PaneListingJob::Result finish(PaneListingJob& job) {
    for (int step = 0; step < 100; ++step) {
        if (job.advance() == PaneListingJobState::Complete) return required(job.completed());
        if (job.state() == PaneListingJobState::Failed) throw std::runtime_error("native pane job failed");
    }
    throw std::runtime_error("native pane job exceeded fixture budget");
}
}
CK_TEST(native_pane_job_publishes_native_rows_identity_and_hidden_policy_after_end) {
    const ck::testing::ScratchDirectory scratch("files-panejob-native"); file(scratch.path() / "visible"); file(scratch.path() / ".dot");
#if defined(_WIN32)
    if (!SetFileAttributesW((scratch.path() / ".dot").c_str(), FILE_ATTRIBUTE_HIDDEN)) throw std::runtime_error("cannot hide native pane fixture");
#endif
    NativeFileSystem filesystem; PaneModel pane;
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(filesystem, scratch.path()), model);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && pane.size() == 0 && pane.listing_pending());
    CK_CHECK(pane.publish(*finish(job)) == PanePublish::Applied && pane.size() == 2 && pane.visible_size() == 1);
    CK_CHECK(pane.visible_row(0)->display_name == "visible" && !pane.visible_row(0)->entry.inspection_error());
    CK_CHECK(compare_identity(required(pane.listing_directory()), required(required(filesystem.inspect_basic(scratch.path())).identity)) == IdentityComparison::Same);
    auto view = pane.view(); view.show_hidden = true; pane.set_view(view); CK_CHECK(pane.visible_size() == 2);
}
CK_TEST(native_pane_job_keeps_held_directory_across_path_replacement_between_batches) {
    const ck::testing::ScratchDirectory scratch("files-panejob-owner"); const auto path = scratch.path() / "parent", retired = scratch.path() / "retired";
    fs::create_directory(path); file(path / "a"); file(path / "b"); NativeFileSystem filesystem; PaneModel pane;
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(filesystem, path), model);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting);
    fs::rename(path, retired); fs::create_directory(path); file(path / "replacement");
    CK_CHECK(pane.publish(*finish(job)) == PanePublish::Applied && pane.size() == 2);
    CK_CHECK(pane.visible_row(0)->display_name == "a" && pane.visible_row(1)->display_name == "b");
    CK_CHECK(compare_identity(required(pane.listing_directory()), required(required(filesystem.inspect_basic(retired)).identity)) == IdentityComparison::Same);
}
CK_TEST(native_pane_job_capacity_failure_does_not_publish_a_partial_directory) {
    const ck::testing::ScratchDirectory scratch("files-panejob-limit"); file(scratch.path() / "a"); file(scratch.path() / "b");
    NativeFileSystem filesystem; PaneModel pane; PaneLimits limits; limits.rows = 1;
    PaneListingJob job(pane.begin_listing(PaneListingPurpose::Navigate), listing(filesystem, scratch.path()), model, limits);
    CK_CHECK(job.advance() == PaneListingJobState::Collecting && job.advance() == PaneListingJobState::Failed);
    if (!job.failure()) throw std::runtime_error("expected capacity failure");
    CK_CHECK(pane.fail_listing(job.request(), *job.failure()) && pane.size() == 0 && !job.completed().value());
}
