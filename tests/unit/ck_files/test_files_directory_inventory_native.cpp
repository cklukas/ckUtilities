// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_inventory_job.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <stdexcept>
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> result) {
    if (!result.value()) throw std::runtime_error("native directory inventory observation unavailable");
    return std::move(result).take_value();
}
DirectoryInventoryJob::Result finish(DirectoryInventoryJob& job) {
    for (int i = 0; i < 1000; ++i) {
        if (job.advance() == DirectoryInventoryState::Complete) return take(job.completed());
        if (job.state() == DirectoryInventoryState::Failed) throw std::runtime_error("native directory inventory failed");
    }
    throw std::runtime_error("native directory inventory step limit");
}
}
CK_TEST(native_directory_inventory_retains_native_location_access_and_identity_without_enumerating) {
    const ck::testing::ScratchDirectory scratch("files-directory-inventory");
    fs::create_directory(scratch.path()/"a"); fs::create_directory(scratch.path()/"b");
    NativeFileSystem filesystem; auto cursor = std::shared_ptr<DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    // Enumerate only on the owning test worker before/after the job.
    const auto first = cursor->read(1); CK_CHECK(first.names.size() == 1);
    DirectoryInventoryJob job(cursor); const auto result = finish(job); const auto& data = result->data();
    CK_CHECK(take(data.directory_continuity) == IdentityComparison::Same);
    CK_CHECK(data.location_before.value() && data.location_after.value());
    CK_CHECK(take(result->compare_owner(data.location_before.value()->directory)) == IdentityComparison::Same);
    for (std::size_t i = 0; i < data.access.size(); ++i) if (data.access[i].value()) {
        CK_CHECK(data.access[i].value()->requested == inventory_directory_access[i]);
        CK_CHECK(take(result->compare_owner(Observation<ObjectIdentity>::available(data.access[i].value()->directory))) == IdentityComparison::Same);
    }
    const auto rest = cursor->read(10); CK_CHECK(rest.state == DirectoryReadState::End && rest.names.size() == 1 && rest.names[0] != first.names[0]);
    CK_CHECK(std::distance(fs::directory_iterator(scratch.path()), fs::directory_iterator{}) == 2);
}
CK_TEST(native_directory_inventory_uses_the_held_directory_after_original_path_replacement) {
    const ck::testing::ScratchDirectory scratch("files-directory-inventory-held");
    const auto original = scratch.path()/"original", moved = scratch.path()/"moved";
    fs::create_directory(original); fs::create_directory(original/"retained"); NativeFileSystem filesystem;
    auto cursor = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(original)));
    const auto id = take(cursor->identity()); fs::rename(original, moved); fs::create_directory(original);
    DirectoryInventoryJob job(cursor); cursor.reset(); const auto result = finish(job);
    CK_CHECK(take(result->data().directory_continuity) == IdentityComparison::Same);
    CK_CHECK(take(result->compare_owner(Observation<ObjectIdentity>::available(id))) == IdentityComparison::Same);
    auto replacement = take(filesystem.open_directory(original));
    CK_CHECK(take(result->compare_owner(replacement->identity())) == IdentityComparison::Different);
    CK_CHECK(fs::is_empty(original) && fs::is_directory(moved/"retained"));
}
