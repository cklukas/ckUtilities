// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "object_inventory_job.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <fstream>

namespace {
using namespace ck::files;
template<class T> T take(Observation<T> observation) {
    if (!observation.value()) throw std::runtime_error("required native inventory observation unavailable");
    return std::move(observation).take_value();
}
NativeName name(const char* value) {
#ifdef _WIN32
    std::u16string units; for (const auto* p = value; *p; ++p) units.push_back(static_cast<char16_t>(*p));
    return *NativeName::from_windows(std::move(units));
#else
    return *NativeName::from_posix(value);
#endif
}
void write(const std::filesystem::path& path, const char* value) {
    std::ofstream out(path, std::ios::binary); out << value; out.close();
    if (!out) throw std::runtime_error("cannot write native inventory fixture");
}
ObjectInventoryJob::Result finish(ObjectInventoryJob& job) {
    for (int i = 0; i < 100; ++i) {
        const auto state = job.advance();
        if (state == ObjectInventoryJobState::Complete) return take(job.completed());
        if (state == ObjectInventoryJobState::Failed) throw std::runtime_error("native inventory job failed");
    }
    throw std::runtime_error("native inventory advance bound exceeded");
}
std::unique_ptr<OpenedEntry> open(const NativeFileSystem& fs, const std::filesystem::path& directory) {
    return take(take(fs.open_directory(directory))->open_child(name("file"), EntryAccess::MetadataAndAttributes | EntryAccess::MetadataAndSecurity));
}
}
CK_TEST(native_object_inventory_collects_one_held_object_and_keeps_its_result_after_name_replacement) {
    const ck::testing::ScratchDirectory scratch("files-object-inventory"); NativeFileSystem fs;
    write(scratch.path() / "file", "old"); ObjectInventoryJob job(open(fs, scratch.path())); auto result = finish(job);
    const auto& data = result->data();
    CK_CHECK(take(data.before.value()->logical_size) == 3 && take(data.after.value()->logical_size) == 3);
    CK_CHECK(take(data.identity_continuity) == IdentityComparison::Same && take(result->compare_owner(data.before.value()->identity)) == IdentityComparison::Same);
    if (const auto* catalog = data.attributes.value()) {
        CK_CHECK(data.attribute_values.size() == catalog->attributes.size());
        for (std::size_t i = 0; i < data.attribute_values.size(); ++i) CK_CHECK(data.attribute_values[i].catalog_index == i);
    } else CK_CHECK(data.attribute_values.empty());
    std::filesystem::rename(scratch.path() / "file", scratch.path() / "retired"); write(scratch.path() / "file", "replacement");
    const auto replacement = take(fs.inspect_basic(scratch.path() / "file"));
    CK_CHECK(compare_identity(take(replacement.identity), take(data.before.value()->identity)) == IdentityComparison::Different);
    const auto held = take(result->object_for_worker().inspect_basic());
    CK_CHECK(take(held.logical_size) == 3 && compare_identity(take(held.identity), take(data.before.value()->identity)) == IdentityComparison::Same);
    CK_CHECK(job.abandon() && result->data().before.value()->logical_size.value() && *result->data().before.value()->logical_size.value() == 3);
    result.reset();
}
CK_TEST(native_object_inventory_cancellation_before_queries_can_resume_or_release_the_held_entry) {
    const ck::testing::ScratchDirectory scratch("files-object-inventory"); NativeFileSystem fs; write(scratch.path() / "file", "data");
    std::stop_source stop; stop.request_stop(); ObjectInventoryJob job(open(fs, scratch.path()));
    CK_CHECK(job.advance(stop.get_token()) == ObjectInventoryJobState::Paused && !job.completed().value());
    const auto result = finish(job); CK_CHECK(take(result->data().identity_continuity) == IdentityComparison::Same);
    ObjectInventoryJob abandoned(open(fs, scratch.path()));
    CK_CHECK(abandoned.advance(stop.get_token()) == ObjectInventoryJobState::Paused && abandoned.abandon());
    CK_CHECK(abandoned.advance() == ObjectInventoryJobState::Abandoned && !abandoned.completed().value());
}
CK_TEST(native_object_inventory_does_not_reopen_a_removed_name) {
    const ck::testing::ScratchDirectory scratch("files-object-inventory"); NativeFileSystem fs; write(scratch.path() / "file", "old");
    auto owned = open(fs, scratch.path()); const auto identity = take(take(owned->inspect_basic()).identity);
    if (!std::filesystem::remove(scratch.path() / "file")) throw std::runtime_error("cannot remove native inventory fixture binding");
    ObjectInventoryJob job(std::move(owned)); const auto result = finish(job); const auto& data = result->data();
    CK_CHECK(take(data.before.value()->logical_size) == 3 && take(data.after.value()->logical_size) == 3);
    CK_CHECK(compare_identity(identity, take(data.before.value()->identity)) == IdentityComparison::Same && take(data.identity_continuity) == IdentityComparison::Same);
    // Windows may keep the name delete-pending until the held handle closes;
    // POSIX removes the binding immediately. Neither permits path reopening to
    // stand in for inspection of the already-held object.
    CK_CHECK(!fs.inspect_basic(scratch.path() / "file").value());
}
