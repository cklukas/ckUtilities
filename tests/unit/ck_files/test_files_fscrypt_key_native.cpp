// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#ifdef __linux__
#include <unistd.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native key-status observation unavailable");
    return std::move(value).take_value();
}
}
CK_TEST(native_fscrypt_key_query_does_not_modify_plain_directories_or_advance_enumeration) {
    const ck::testing::ScratchDirectory scratch("files-fscrypt-key");
    fs::create_directory(scratch.path()/"a"); fs::create_directory(scratch.path()/"b");
    NativeFileSystem filesystem; auto cursor = take(filesystem.open_directory(scratch.path()));
    const auto first = cursor->read(1); CK_CHECK(first.names.size() == 1);
    const auto result = cursor->inspect_fscrypt_key_status();
    CK_CHECK(result.state() == ObservationState::Unsupported);
#ifdef __linux__
    const auto policy = cursor->inspect_fscrypt_policy();
    if (policy.value() && std::holds_alternative<FscryptPolicyAbsent>(policy.value()->policy))
        CK_CHECK(result.unavailable_detail()->error.operation == "fscrypt.key_no_policy");
#else
    CK_CHECK(result.unavailable_detail()->error.operation == "directory.fscrypt_key_status");
#endif
    const auto rest = cursor->read(10);
    CK_CHECK(rest.state == DirectoryReadState::End && rest.names.size() == 1 && rest.names[0] != first.names[0]);
    CK_CHECK(std::distance(fs::directory_iterator(scratch.path()), fs::directory_iterator{}) == 2);
}
#ifdef __linux__
CK_TEST(native_fscrypt_key_v1_fixture_limits_absence_to_the_filesystem_keyring) {
    const char* configured = std::getenv("CKTOOLS_FSCRYPT_KEY_FIXTURE");
    if (!configured) { std::cout << "native-fscrypt-key: v1 fixture not configured\n"; return; }
    CK_CHECK(geteuid() != 0); const fs::path root(configured);
    const auto expected = ck::testing::scratch_root().parent_path().parent_path()/"fscrypt-key-fixture-20261024/mount";
    if (!root.is_absolute() || root.lexically_normal() != expected || fs::is_symlink(fs::symlink_status(root)))
        throw std::runtime_error("v1 key fixture must be the private task mount");
    NativeFileSystem filesystem; auto cursor = take(filesystem.open_directory(root/"legacy"));
    const auto report = take(cursor->inspect_fscrypt_key_status());
    CK_CHECK(report.policy.version() == 0 && report.policy.key_reference().size() == 8);
    for (std::size_t i = 0; i < 8; ++i) CK_CHECK(report.policy.key_reference()[i] == static_cast<std::byte>(0xb0 + i));
    CK_CHECK(compare_identity(report.directory, take(cursor->identity())) == IdentityComparison::Same);
    CK_CHECK(take(fscrypt_filesystem_key_state(report)) == FscryptFilesystemKeyState::Absent);
    CK_CHECK(fscrypt_key_added_by_self(report).state() == ObservationState::Unsupported &&
        fscrypt_key_user_count(report).state() == ObservationState::Unsupported);
    std::cout << "native-fscrypt-key: v1 filesystem-only absent key status qualified\n";
}
CK_TEST(native_fscrypt_key_locked_v2_fixture_reports_absent_without_any_key_material_or_path_reopen) {
    const char* configured = std::getenv("CKTOOLS_FSCRYPT_KEY_FIXTURE");
    if (!configured) { std::cout << "native-fscrypt-key: locked fixture not configured\n"; return; }
    CK_CHECK(geteuid() != 0); const fs::path root(configured);
    const auto expected = ck::testing::scratch_root().parent_path().parent_path()/"fscrypt-key-fixture-20261024/mount";
    if (!root.is_absolute() || root.lexically_normal() != expected || fs::is_symlink(fs::symlink_status(root)))
        throw std::runtime_error("key fixture must be the private task mount");
    NativeFileSystem filesystem; const auto original = root/"encrypted", moved = root/"moved";
    auto cursor = take(filesystem.open_directory(original)); const auto id = take(cursor->identity());
    const auto report = take(cursor->inspect_fscrypt_key_status());
    CK_CHECK(compare_identity(report.directory, id) == IdentityComparison::Same && report.policy.version() == 2);
    CK_CHECK(take(fscrypt_filesystem_key_state(report)) == FscryptFilesystemKeyState::Absent);
    CK_CHECK(!take(fscrypt_key_added_by_self(report)) && take(fscrypt_key_user_count(report)) == 0);
    for (std::size_t i = 0; i < 16; ++i) CK_CHECK(report.policy.key_reference()[i] == static_cast<std::byte>(0xa0 + i));
    struct Restore {
        fs::path original, moved; bool renamed = false, replacement = false;
        ~Restore() { if (!renamed) return; std::error_code error; if (replacement) fs::remove(original, error); if (!fs::exists(original, error)) fs::rename(moved, original, error); }
    } restore{original, moved};
    if (fs::exists(moved)) throw std::runtime_error("fixture move target exists");
    fs::rename(original, moved); restore.renamed = true; fs::create_directory(original); restore.replacement = true;
    const auto held = take(cursor->inspect_fscrypt_key_status());
    CK_CHECK(compare_identity(held.directory, id) == IdentityComparison::Same && take(fscrypt_filesystem_key_state(held)) == FscryptFilesystemKeyState::Absent);
    const auto replacement = take(filesystem.open_directory(original));
    CK_CHECK(replacement->inspect_fscrypt_key_status().state() == ObservationState::Unsupported && fs::is_empty(original));
    CK_CHECK(fs::remove(original)); restore.replacement = false; fs::rename(moved, original); restore.renamed = false;
    std::cout << "native-fscrypt-key: absent v2 filesystem key and held-directory replacement isolation qualified\n";
}
#endif
