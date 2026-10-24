// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "source_target_job.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#include <stdexcept>
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native source-target observation unavailable");
    return std::move(value).take_value();
}
NativeName name() {
#ifdef _WIN32
    return *NativeName::from_windows(u"a");
#else
    return *NativeName::from_posix("a");
#endif
}
auto finish(auto& job) {
    for (int i = 0; i < 100; ++i) {
        (void)job.advance(); auto result = job.completed();
        if (result.value()) return std::move(result).take_value();
        if (job.failure()) throw std::runtime_error("native source-target job failed");
    }
    throw std::runtime_error("native source-target fixture exceeded step bound");
}
void write(const fs::path& path, const char* text) { std::ofstream out(path, std::ios::binary); out << text; out.close(); if (!out) throw std::runtime_error("cannot write source-target fixture"); }
HeldChild::Pointer source(const NativeFileSystem& filesystem, const fs::path& path) {
    return take(HeldChild::observe(std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(path))), name()));
}
DestinationBatchJob::Result batch(const NativeFileSystem& filesystem, const fs::path& path) {
    DestinationBatchJob job(std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(path))), {name()}, name().encoding()); return finish(job);
}
}
CK_TEST(native_source_target_detects_same_entry_and_hard_link_aliases_through_held_objects) {
    const ck::testing::ScratchDirectory scratch("files-source-target-same"); NativeFileSystem filesystem;
    fs::create_directory(scratch.path() / "destination"); write(scratch.path() / "a", "source");
    const auto held = source(filesystem, scratch.path());
    SourceTargetJob same({held}, batch(filesystem, scratch.path())); CK_CHECK(take(finish(same)->entries()[0].comparison) == IdentityComparison::Same);
    fs::create_hard_link(scratch.path() / "a", scratch.path() / "destination/a");
    SourceTargetJob alias({held}, batch(filesystem, scratch.path() / "destination")); CK_CHECK(take(finish(alias)->entries()[0].comparison) == IdentityComparison::Same);
}
CK_TEST(native_source_target_distinct_files_do_not_imply_content_inequality_or_copy_permission) {
    const ck::testing::ScratchDirectory scratch("files-source-target-different"); NativeFileSystem filesystem;
    fs::create_directory(scratch.path() / "destination"); write(scratch.path() / "a", "identical"); write(scratch.path() / "destination/a", "identical");
    SourceTargetJob job({source(filesystem, scratch.path())}, batch(filesystem, scratch.path() / "destination"));
    CK_CHECK(take(finish(job)->entries()[0].comparison) == IdentityComparison::Different);
}
CK_TEST(native_source_target_retains_old_objects_after_source_and_target_names_are_replaced) {
    const ck::testing::ScratchDirectory scratch("files-source-target-held"); NativeFileSystem filesystem;
    const auto destination = scratch.path() / "destination"; fs::create_directory(destination); write(scratch.path() / "a", "old"); fs::create_hard_link(scratch.path() / "a", destination / "a");
    auto held = source(filesystem, scratch.path()); auto probed = batch(filesystem, destination);
    fs::rename(scratch.path() / "a", scratch.path() / "retired"); fs::rename(destination / "a", destination / "retired");
    write(scratch.path() / "a", "new source"); write(destination / "a", "new target");
    SourceTargetJob job({held}, std::move(probed)); const auto result = finish(job);
    CK_CHECK(take(result->entries()[0].comparison) == IdentityComparison::Same && take(take(result->entries()[0].source_now).logical_size) == 3);
}
CK_TEST(native_source_target_absence_and_independent_adapter_scopes_never_prove_difference) {
    const ck::testing::ScratchDirectory scratch("files-source-target-unknown"); NativeFileSystem filesystem, independent;
    fs::create_directory(scratch.path() / "destination"); write(scratch.path() / "a", "source"); const auto held = source(filesystem, scratch.path());
    SourceTargetJob absent({held}, batch(filesystem, scratch.path() / "destination")); CK_CHECK(finish(absent)->entries()[0].comparison.state() == ObservationState::Unsupported);
    SourceTargetJob foreign({held}, batch(independent, scratch.path())); CK_CHECK(finish(foreign)->entries()[0].comparison.state() == ObservationState::Unknown);
}
#ifndef _WIN32
CK_TEST(native_source_target_does_not_follow_a_final_destination_symbolic_link) {
    const ck::testing::ScratchDirectory scratch("files-source-target-symlink"); NativeFileSystem filesystem;
    fs::create_directory(scratch.path() / "destination"); write(scratch.path() / "a", "source"); fs::create_symlink(scratch.path() / "a", scratch.path() / "destination/a");
    SourceTargetJob job({source(filesystem, scratch.path())}, batch(filesystem, scratch.path() / "destination")); const auto result = finish(job);
    CK_CHECK(take(result->entries()[0].comparison) == IdentityComparison::Different && take(take(result->entries()[0].target_now).kind) == ObjectKind::SymbolicLink);
}
#endif
