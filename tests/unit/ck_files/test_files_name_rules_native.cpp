// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "directory_name_rules.hpp"
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <iostream>
#include <stdexcept>
#ifdef __linux__
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native name-rule observation unavailable");
    return std::move(value).take_value();
}
NativeName name(const char* bytes) {
    const auto component = *NativeName::from_posix(bytes);
#ifdef _WIN32
    return take(map_native_name(component, NameEncoding::WindowsUtf16));
#else
    return component;
#endif
}
}
CK_TEST(native_name_rules_keep_lexical_identity_separate_from_unqualified_native_comparisons) {
    const ck::testing::ScratchDirectory scratch("files-name-rules"); NativeFileSystem filesystem;
    fs::create_directory(scratch.path() / "a"); fs::create_directory(scratch.path() / "b");
    auto cursor = std::shared_ptr<DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    const auto first = cursor->read(1); CK_CHECK(first.names.size() == 1);
    const auto probe = take(DirectoryNameRulesProbe::inspect(cursor));
    CK_CHECK(probe->before().value());
    const auto identical = take(probe->compare(name("same"), name("same")));
    CK_CHECK(identical.relation == ComponentNameRelation::IdenticalNativeUnits);
    CK_CHECK(compare_identity(take(identical.directory), take(cursor->identity())) == IdentityComparison::Same);
    const auto different = probe->compare(name("MissingA"), name("missinga"));
#ifdef __linux__
    const auto& info = *probe->before().value();
    const bool plain_extended = info.filesystem_type_code.value() && *info.filesystem_type_code.value() == 0xef53 &&
        info.linux_directory_inode_flags.value() && (*info.linux_directory_inode_flags.value() & 0x40000800U) == 0;
    if (plain_extended) {
        CK_CHECK(probe->rule().value() && probe->after().value());
        CK_CHECK(different.value() && different.value()->relation == ComponentNameRelation::DistinctUnderReportedRules);
        std::cout << "native-name-rules: LinuxExtendedFsBytewise qualified\n";
    } else CK_CHECK(!probe->rule().value() && !different.value());
#else
    CK_CHECK(probe->rule().state() == ObservationState::Unsupported && different.state() == ObservationState::Unsupported);
    CK_CHECK(probe->after().unavailable_detail()->error.operation == "name_rules.after_not_attempted");
#endif
    CK_CHECK(take(cursor->lookup_child(name("MissingA"), ChildLookupMode::NativeDefault)).state() == ChildLookupState::Missing);
    CK_CHECK(take(cursor->lookup_child(name("missinga"), ChildLookupMode::NativeDefault)).state() == ChildLookupState::Missing);
    const auto rest = cursor->read(10); CK_CHECK(rest.state == DirectoryReadState::End && rest.names.size() == 1 && rest.names[0] != first.names[0]);
    CK_CHECK(std::distance(fs::directory_iterator(scratch.path()), fs::directory_iterator{}) == 2);
}
CK_TEST(native_name_rules_refresh_uses_the_retained_directory_after_path_replacement_and_keeps_owners) {
    const ck::testing::ScratchDirectory scratch("files-name-rules-move"); NativeFileSystem filesystem;
    const auto original = scratch.path() / "original", moved = scratch.path() / "moved";
    fs::create_directory(original);
    auto cursor = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(original)));
    std::weak_ptr<const DirectoryCursor> weak = cursor;
    auto probe = take(DirectoryNameRulesProbe::inspect(cursor));
    const auto original_id = take(cursor->identity()); cursor.reset(); CK_CHECK(!weak.expired());
    fs::rename(original, moved); fs::create_directory(original);
    auto refreshed = take(probe->refresh());
    const auto& fresh = take(refreshed->before());
    CK_CHECK(compare_identity(take(fresh.directory), original_id) == IdentityComparison::Same);
    auto replacement = take(filesystem.open_directory(original));
    CK_CHECK(compare_identity(take(fresh.directory), take(replacement->identity())) == IdentityComparison::Different);
    probe.reset(); CK_CHECK(!weak.expired()); refreshed.reset(); CK_CHECK(weak.expired());
    CK_CHECK(fs::is_empty(original) && fs::is_empty(moved));
}
#ifdef __linux__
namespace {
void create_exclusive(const fs::path& path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) throw std::runtime_error("native byte-rule fixture exclusive creation failed: " + std::to_string(errno));
    if (close(fd) != 0) throw std::runtime_error("native byte-rule fixture close failed");
}
}
CK_TEST(native_linux_byte_rules_agree_with_exclusive_case_normalization_and_raw_byte_names_without_conflating_hard_links) {
    const ck::testing::ScratchDirectory scratch("files-name-rules-bytes"); NativeFileSystem filesystem;
    auto cursor = std::shared_ptr<DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    const auto probe = take(DirectoryNameRulesProbe::inspect(cursor));
    if (!probe->rule().value()) {
        std::cout << "native-name-rules: bytewise fixture unavailable on this scratch filesystem\n";
        CK_CHECK(!probe->compare(name("A"), name("a")).value()); return;
    }
    for (const auto* component : {"Case", "case", "\xc3\xa9", "e\xcc\x81", "\xff", "\xfe"}) create_exclusive(scratch.path() / component);
    for (const auto& pair : {std::pair{"Case", "case"}, std::pair{"\xc3\xa9", "e\xcc\x81"}, std::pair{"\xff", "\xfe"}}) {
        CK_CHECK(take(probe->compare(name(pair.first), name(pair.second))).relation == ComponentNameRelation::DistinctUnderReportedRules);
        const auto first = take(filesystem.inspect_basic(scratch.path() / pair.first));
        const auto second = take(filesystem.inspect_basic(scratch.path() / pair.second));
        CK_CHECK(compare_identity(take(first.identity), take(second.identity)) == IdentityComparison::Different);
    }
    fs::create_hard_link(scratch.path() / "Case", scratch.path() / "alias");
    CK_CHECK(take(probe->compare(name("Case"), name("alias"))).relation == ComponentNameRelation::DistinctUnderReportedRules);
    const auto first = take(filesystem.inspect_basic(scratch.path() / "Case"));
    const auto alias = take(filesystem.inspect_basic(scratch.path() / "alias"));
    CK_CHECK(compare_identity(take(first.identity), take(alias.identity)) == IdentityComparison::Same);
    std::cout << "native-name-rules: exclusive byte-name and hard-link fixtures qualified\n";
}
#endif
