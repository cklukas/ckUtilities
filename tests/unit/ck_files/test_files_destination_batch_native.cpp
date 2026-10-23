// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "destination_batch_job.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>
#if defined(_WIN32)
#include "windows_junction.hpp"
#endif

namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native destination batch observation unavailable");
    return std::move(value).take_value();
}
NativeName name(const char* text) {
#if defined(_WIN32)
    std::u16string units; for (const auto* p = text; *p; ++p) units.push_back(static_cast<char16_t>(*p));
    return *NativeName::from_windows(std::move(units));
#else
    return *NativeName::from_posix(text);
#endif
}
void write(const fs::path& path, const char* bytes) {
    std::ofstream stream(path, std::ios::binary); stream << bytes; stream.close();
    if (!stream) throw std::runtime_error("cannot write native destination batch fixture");
}
std::shared_ptr<const DirectoryCursor> parent(const NativeFileSystem& filesystem, const fs::path& path) {
    return std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(path)));
}
DestinationBatchJob::Result finish(DestinationBatchJob& job) {
    for (int i = 0; i < 100; ++i) {
        const auto state = job.advance();
        if (state == DestinationBatchState::Complete) return take(job.completed());
        if (state == DestinationBatchState::Failed) throw std::runtime_error("native destination batch failed");
    }
    throw std::runtime_error("native destination batch fixture exceeded advance bound");
}
}
CK_TEST(native_destination_batch_separates_exact_name_duplicates_from_distinct_hard_link_targets) {
    const ck::testing::ScratchDirectory scratch("files-destination-batch"); NativeFileSystem filesystem;
    write(scratch.path() / "file", "old"); fs::create_hard_link(scratch.path() / "file", scratch.path() / "alias");
    DestinationBatchJob job(parent(filesystem, scratch.path()), {name("file"), name("alias"), name("alias")}, name("file").encoding());
    const auto result = finish(job);
    CK_CHECK(result->exact_name_count(0) == 1 && result->exact_name_count(1) == 2 && result->probe(1) == result->probe(2));
    CK_CHECK(result->reported_object_leader(0) == 0 && result->reported_object_leader(1) == 0 && result->reported_object_leader(2) == 0);
    CK_CHECK(result->entries()[0].exact_name_leader != result->entries()[1].exact_name_leader);
}
CK_TEST(native_destination_batch_distinct_missing_names_remain_distinct_reports_without_a_collision_free_claim) {
    const ck::testing::ScratchDirectory scratch("files-destination-batch"); NativeFileSystem filesystem;
    DestinationBatchJob job(parent(filesystem, scratch.path()), {name("MISSING"), name("missing")}, name("file").encoding());
    const auto result = finish(job);
    for (std::size_t i = 0; i < 2; ++i) {
        CK_CHECK(result->probe(i)->lookup().value()->state() == ChildLookupState::Missing && result->probe(i)->lookup().value()->missing_witness());
        CK_CHECK(result->exact_name_count(i) == 1 && !result->reported_object_leader(i));
    }
    CK_CHECK(!fs::exists(scratch.path() / "missing") && !fs::exists(scratch.path() / "MISSING"));
}
CK_TEST(native_destination_batch_uses_the_held_parent_after_its_original_path_is_replaced) {
    const ck::testing::ScratchDirectory scratch("files-destination-batch"); NativeFileSystem filesystem;
    const auto directory = scratch.path() / "parent", retired = scratch.path() / "retired";
    fs::create_directory(directory); write(directory / "file", "old"); auto held = parent(filesystem, directory);
    fs::rename(directory, retired); fs::create_directory(directory); write(directory / "file", "replacement");
    DestinationBatchJob job(std::move(held), {name("file")}, name("file").encoding()); const auto result = finish(job);
    const auto observed = take(result->probe(0)->existing_metadata());
    CK_CHECK(take(observed.logical_size) == 3);
    CK_CHECK(compare_identity(take(observed.identity), take(take(filesystem.inspect_basic(directory / "file")).identity)) == IdentityComparison::Different);
}
CK_TEST(native_destination_batch_observes_dangling_final_links_without_following_them) {
    const ck::testing::ScratchDirectory scratch("files-destination-batch"); NativeFileSystem filesystem;
#if defined(_WIN32)
    fs::create_directory(scratch.path() / "link"); ck::testing::make_junction(scratch.path() / "link", scratch.path() / "absent");
    constexpr auto kind = ObjectKind::Junction;
#else
    fs::create_symlink("absent", scratch.path() / "link"); constexpr auto kind = ObjectKind::SymbolicLink;
#endif
    DestinationBatchJob job(parent(filesystem, scratch.path()), {name("link"), name("absent")}, name("file").encoding()); const auto result = finish(job);
    CK_CHECK(result->probe(0)->lookup().value()->state() == ChildLookupState::Found && take(result->probe(0)->existing_metadata().value()->kind) == kind);
    CK_CHECK(result->probe(1)->lookup().value()->state() == ChildLookupState::Missing && !result->reported_object_leader(0));
}
CK_TEST(native_destination_batch_result_keeps_old_targets_while_a_fresh_batch_observes_replacements) {
    const ck::testing::ScratchDirectory scratch("files-destination-batch"); NativeFileSystem filesystem;
    write(scratch.path() / "a", "old"); write(scratch.path() / "b", "other"); auto held = parent(filesystem, scratch.path());
    DestinationBatchJob first(held, {name("a"), name("b")}, name("a").encoding()); auto before = finish(first);
    CK_CHECK(!before->reported_object_leader(0) && !before->reported_object_leader(1));
    fs::rename(scratch.path() / "a", scratch.path() / "retired"); fs::create_hard_link(scratch.path() / "b", scratch.path() / "a");
    DestinationBatchJob second(held, {name("a"), name("b")}, name("a").encoding()); const auto after = finish(second);
    CK_CHECK(after->reported_object_leader(0) == 0 && after->reported_object_leader(1) == 0);
    CK_CHECK(!before->reported_object_leader(0) && take(before->probe(0)->existing_metadata().value()->logical_size) == 3);
    CK_CHECK(take(take(before->probe(0)->lookup().value()->entry()->inspect_basic()).logical_size) == 3);
    CK_CHECK(first.abandon() && before->pin_directory()); before.reset();
}
