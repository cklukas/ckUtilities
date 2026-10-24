// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_index_file.hpp"
#include "scratch_directory.hpp"
#include "windows_junction.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#ifdef _WIN32
#include <aclapi.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <membership.h>
#include <sys/acl.h>
#include <sys/mount.h>
#endif
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template <class T> T take(Observation<T> result) {
    if (!result.value())
        throw std::runtime_error("native index fixture: " + result.unavailable_detail()->error.operation + ":" +
                                 std::to_string(result.unavailable_detail()->error.code));
    return std::move(result).take_value();
}
NativeAbsolutePath address(const fs::path &path) {
    return take(NativeAbsolutePath::from_host(path));
}
void private_root(const fs::path &path) {
#ifndef _WIN32
    if (chmod(path.c_str(), 0700) != 0)
        throw std::runtime_error("cannot make private fixture root");
#else
    (void)path;
#endif
}
std::size_t children(const fs::path &path) {
    return static_cast<std::size_t>(std::distance(fs::directory_iterator(path), fs::directory_iterator{}));
}
void write_all(IndexByteFile &file, std::uint64_t offset, std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto n = take(file.write_at(offset, bytes));
        if (!n || n > bytes.size())
            throw std::runtime_error("native index short write fixture");
        offset += n;
        bytes = bytes.subspan(n);
    }
}
void read_all(IndexByteFile &file, std::uint64_t offset, std::span<std::byte> bytes) {
    while (!bytes.empty()) {
        const auto n = take(file.read_at(offset, bytes));
        if (!n || n > bytes.size())
            throw std::runtime_error("native index short read fixture");
        offset += n;
        bytes = bytes.subspan(n);
    }
}
#ifndef _WIN32
std::set<int> descriptors() {
    DIR *directory = opendir("/dev/fd");
    if (!directory)
        throw std::runtime_error("cannot inspect test descriptors");
    const int own = dirfd(directory);
    std::set<int> result;
    while (const auto *entry = readdir(directory)) {
        char *end{};
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (end != entry->d_name && *end == 0 && fd >= 0 && fd <= std::numeric_limits<int>::max() && fd != own)
            result.insert(static_cast<int>(fd));
    }
    closedir(directory);
    return result;
}
#endif
} // namespace
CK_TEST(index_file_native_roundtrip_seek_overwrite_and_cleanup_use_explicit_root) {
    const ck::testing::ScratchDirectory scratch("files-index-native", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    const auto cwd = fs::current_path();
    auto file = take(create_native_index_file(address(scratch.path())));
    CK_CHECK(take(file->size()) == 0);
    std::array<std::byte, 1024> data{};
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<std::byte>(i & 255U);
    write_all(*file, 0, data);
    const std::array patch{std::byte{0xff}, std::byte{}, std::byte{0x80}};
    write_all(*file, 17, patch);
    std::copy(patch.begin(), patch.end(), data.begin() + 17);
    std::array<std::byte, 1024> actual{};
    read_all(*file, 0, actual);
    CK_CHECK(actual == data && take(file->size()) == data.size());
    std::array<std::byte, 1> tail{};
    CK_CHECK(take(file->read_at(data.size(), tail)) == 0);
#ifdef _WIN32
    CK_CHECK(children(scratch.path()) == 1);
#else
    CK_CHECK(children(scratch.path()) == 0);
#endif
    file.reset();
    CK_CHECK(children(scratch.path()) == 0);
    CK_CHECK(fs::current_path() == cwd);
    std::cout << "index-private-native-roundtrip-cleanup-qualified\n";
}
CK_TEST(index_file_native_rejects_overflow_before_IO_and_keeps_exact_size) {
    const ck::testing::ScratchDirectory scratch("files-index-range", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    auto file = take(create_native_index_file(address(scratch.path())));
    std::array<std::byte, 2> bytes{};
    for (auto offset : {UINT64_MAX, static_cast<std::uint64_t>(INT64_MAX)}) {
        CK_CHECK(!file->write_at(offset, bytes).value());
        CK_CHECK(!file->read_at(offset, bytes).value());
    }
    CK_CHECK(take(file->size()) == 0);
}
CK_TEST(index_file_native_uses_host_spelling_for_non_ascii_private_root) {
    const ck::testing::ScratchDirectory scratch("files-index-spelling", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#ifdef _WIN32
    const fs::path component(L"index-\u00e4-e\u0301-\U0001f4c1");
#else
    const fs::path component("index-\xc3\xa4-e\xcc\x81-\\literal");
#endif
    const auto root = scratch.path() / component;
    fs::create_directory(root);
    private_root(root);
    auto file = take(create_native_index_file(address(root)));
    const std::array data{std::byte{0xff}, std::byte{}, std::byte{0x80}};
    write_all(*file, 0, data);
    std::array<std::byte, 3> actual{};
    read_all(*file, 0, actual);
    CK_CHECK(actual == data);
    file.reset();
    CK_CHECK(children(root) == 0);
    std::cout << "index-private-native-non-ascii-root-qualified\n";
}
CK_TEST(index_file_native_independent_owners_and_record_store_preserve_lossless_names) {
    const ck::testing::ScratchDirectory scratch("files-index-store-native", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    DirectoryIndexContext context;
    context.generation[0] = std::byte{7};
    auto budget = std::make_shared<IndexStorageBudget>(1024 * 1024, 2);
    auto store =
        take(DirectoryIndexStore::create(take(create_native_index_file(address(scratch.path()))), budget, context));
    auto other = take(create_native_index_file(address(scratch.path())));
    const auto name = *NativeName::from_windows(std::u16string{u'x', char16_t{0xd800}, u'\u0301'});
    DirectoryIndexRecord record{
        1, name, Observation<BasicMetadata>::unavailable(ObservationState::Denied, {ErrorDomain::Posix, 13, "inspect"}),
        Observation<bool>::unavailable(ObservationState::Unknown)};
    const auto position = take(store->append(record));
    CK_CHECK(take(store->seal()));
    const auto found = take(store->read(position));
    CK_CHECK(found.row_id == 1 && found.name == record.name && found.metadata.state() == ObservationState::Denied);
    CK_CHECK(take(other->size()) == 0);
    CK_CHECK(budget->charged_bytes() == store->bytes());
    store.reset();
    other.reset();
    CK_CHECK(budget->charged_bytes() == 0 && budget->charged_files() == 0);
    CK_CHECK(children(scratch.path()) == 0);
    std::cout << "index-private-native-store-lossless-qualified\n";
}
CK_TEST(index_file_native_refuses_missing_file_foreign_link_and_ambiguous_final_root) {
    const ck::testing::ScratchDirectory scratch("files-index-root", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    CK_CHECK(!create_native_index_file(address(scratch.path() / "missing")).value());
    const auto plain = scratch.path() / "plain";
    {
        std::ofstream out(plain);
        out << "sentinel";
    }
    CK_CHECK(!create_native_index_file(address(plain)).value());
#ifdef _WIN32
    const auto foreign = take(NativeAbsolutePath::from_posix("/foreign"));
    const auto link = scratch.path() / "link";
    fs::create_directory(link);
    ck::testing::make_junction(link, scratch.path());
#else
    const auto foreign = take(NativeAbsolutePath::from_windows(u"V:\\foreign"));
    const auto link = scratch.path() / "link";
    fs::create_directory_symlink(scratch.path(), link);
#endif
    CK_CHECK(create_native_index_file(foreign).state() == ObservationState::Unsupported);
    CK_CHECK(!create_native_index_file(address(link)).value());
    CK_CHECK(!create_native_index_file(address(scratch.path() / ".")).value());
    CK_CHECK(fs::file_size(plain) == 8);
    // Remove the link itself; never recurse through a fixture junction.
    CK_CHECK(fs::remove(link));
    CK_CHECK(children(scratch.path()) == 1);
    std::cout << "index-private-native-final-link-refusal-qualified\n";
}
CK_TEST(index_file_native_survives_root_relocation_or_native_refusal_without_retargeting) {
    const ck::testing::ScratchDirectory scratch("files-index-relocate", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = scratch.path() / "root", moved = scratch.path() / "moved";
    fs::create_directory(root);
    private_root(root);
    auto file = take(create_native_index_file(address(root)));
    std::error_code error;
    fs::rename(root, moved, error);
#ifdef _WIN32
    if (error) {
        CK_CHECK(error.value() == ERROR_ACCESS_DENIED || error.value() == ERROR_SHARING_VIOLATION);
        const std::array data{std::byte{42}};
        write_all(*file, 0, data);
        file.reset();
        CK_CHECK(children(root) == 0);
        std::cout << "index-private-native-parent-rename-refusal-qualified\n";
        return;
    }
#endif
    if (error)
        throw std::runtime_error("cannot relocate private index root");
    fs::create_directory(root);
    private_root(root);
    const auto sentinel = root / "sentinel";
    {
        std::ofstream out(sentinel);
        out << "untouched";
    }
    const std::array data{std::byte{42}, std::byte{0x80}};
    write_all(*file, 0, data);
    std::array<std::byte, 2> actual{};
    read_all(*file, 0, actual);
    CK_CHECK(actual == data && fs::file_size(sentinel) == 9);
    file.reset();
    CK_CHECK(children(moved) == 0 && children(root) == 1);
    std::cout << "index-private-native-held-relocation-qualified\n";
}
#ifndef _WIN32
CK_TEST(index_file_native_posix_owner_mode_unlinked_descriptor_and_close_on_exec) {
    const ck::testing::ScratchDirectory scratch("files-index-private", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    const auto before = descriptors();
    auto file = take(create_native_index_file(address(scratch.path())));
    auto after = descriptors();
    for (int fd : before)
        after.erase(fd);
    CK_CHECK(after.size() == 1);
    if (after.size() != 1)
        throw std::runtime_error("cannot qualify owned index descriptor");
    const int fd = *after.begin();
    struct stat info{};
    CK_CHECK(fstat(fd, &info) == 0);
    CK_CHECK(S_ISREG(info.st_mode) && info.st_uid == geteuid() && info.st_nlink == 0 && (info.st_mode & 0077) == 0);
    CK_CHECK((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    file.reset();
    CK_CHECK(descriptors() == before);
    CK_CHECK(chmod(scratch.path().c_str(), 0755) == 0);
    CK_CHECK(create_native_index_file(address(scratch.path())).state() == ObservationState::Denied);
    CK_CHECK(children(scratch.path()) == 0);
    std::cout << "index-private-native-posix-mode-unlinked-cloexec-qualified\n";
}
#endif
#ifdef __APPLE__
CK_TEST(index_file_native_mac_refuses_ownership_disabled_storage_without_fallback) {
    const ck::testing::ScratchDirectory scratch("files-index-volume");
    private_root(scratch.path());
    struct statfs volume{};
    if (statfs(scratch.path().c_str(), &volume) != 0)
        throw std::runtime_error("cannot inspect selected fixture volume");
    auto result = create_native_index_file(address(scratch.path()));
    if (volume.f_flags & MNT_IGNORE_OWNERSHIP) {
        CK_CHECK(result.state() == ObservationState::Unsupported);
        std::cout << "index-private-native-owners-disabled-refusal-qualified\n";
    } else {
        CK_CHECK(result.value());
    }
    result = Observation<std::unique_ptr<IndexByteFile>>{};
    CK_CHECK(children(scratch.path()) == 0);
}
CK_TEST(index_file_native_mac_rejects_extended_root_acl_without_modifying_it) {
    const ck::testing::ScratchDirectory scratch("files-index-acl", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    acl_t acl = acl_init(1);
    if (!acl)
        throw std::runtime_error("cannot allocate ACL fixture");
    struct FreeAcl {
        acl_t value;
        ~FreeAcl() {
            acl_free(value);
        }
    } owner{acl};
    acl_entry_t entry{};
    acl_permset_t permissions{};
    uuid_t user{};
    if (mbr_uid_to_uuid(geteuid(), user) || acl_create_entry(&owner.value, &entry) ||
        acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) || acl_set_qualifier(entry, user) ||
        acl_get_permset(entry, &permissions) || acl_add_perm(permissions, ACL_READ_DATA) ||
        acl_set_file(scratch.path().c_str(), ACL_TYPE_EXTENDED, owner.value))
        throw std::runtime_error("cannot create root ACL fixture");
    CK_CHECK(create_native_index_file(address(scratch.path())).state() == ObservationState::Denied);
    CK_CHECK(children(scratch.path()) == 0);
    acl_t observed = acl_get_file(scratch.path().c_str(), ACL_TYPE_EXTENDED);
    if (!observed)
        throw std::runtime_error("cannot inspect root ACL fixture");
    FreeAcl observed_owner{observed};
    CK_CHECK(acl_get_entry(observed_owner.value, ACL_FIRST_ENTRY, &entry) == 0);
    std::cout << "index-private-native-mac-acl-refusal-qualified\n";
}
#endif
#ifdef _WIN32
CK_TEST(index_file_native_windows_protects_from_inherited_world_access_and_excludes_other_opens) {
    const ck::testing::ScratchDirectory scratch("files-index-dacl", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    alignas(DWORD) std::array<std::byte, SECURITY_MAX_SID_SIZE> world{};
    DWORD world_size = static_cast<DWORD>(world.size());
    if (!CreateWellKnownSid(WinWorldSid, nullptr, world.data(), &world_size))
        throw std::runtime_error("cannot create world SID fixture");
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(world.data());
    PACL acl{};
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS)
        throw std::runtime_error("cannot create inherited ACL fixture");
    struct FreeLocal {
        void *value;
        ~FreeLocal() {
            LocalFree(value);
        }
    } acl_owner{acl};
    if (SetNamedSecurityInfoW(const_cast<LPWSTR>(scratch.path().c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl,
                              nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("cannot set inherited root ACL fixture");
    auto file = take(create_native_index_file(address(scratch.path())));
    const auto path = fs::directory_iterator(scratch.path())->path();
    HANDLE second = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    CK_CHECK(second == INVALID_HANDLE_VALUE);
    if (second != INVALID_HANDLE_VALUE)
        CloseHandle(second);
    PSECURITY_DESCRIPTOR descriptor{};
    PACL actual{};
    PSID user{};
    const DWORD code = GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                             DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION, &user, nullptr,
                                             &actual, nullptr, &descriptor);
    FreeLocal descriptor_owner{descriptor};
    CK_CHECK(code == ERROR_SUCCESS);
    if (code != ERROR_SUCCESS)
        throw std::runtime_error("cannot independently inspect private ACL");
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    CK_CHECK(GetSecurityDescriptorControl(descriptor, &control, &revision));
    CK_CHECK((control & SE_DACL_PROTECTED) != 0 && actual && actual->AceCount == 2);
    for (DWORD i = 0; i < actual->AceCount; ++i) {
        void *raw{};
        CK_CHECK(GetAce(actual, i, &raw));
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw);
        CK_CHECK(ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0);
        PSID sid = const_cast<DWORD *>(&ace->SidStart);
        CK_CHECK(EqualSid(sid, user) || IsWellKnownSid(sid, WinLocalSystemSid));
        CK_CHECK(!EqualSid(sid, world.data()));
    }
    file.reset();
    CK_CHECK(children(scratch.path()) == 0);
    std::cout << "index-private-native-windows-protected-dacl-exclusive-qualified\n";
}
#endif
