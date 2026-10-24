// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_index_file.hpp"
#include "directory_index_sort_job.hpp"
#include "directory_index_listing_job.hpp"
#include "directory_index_lookup.hpp"
#include "indexed_pane.hpp"
#include "object_inventory_job.hpp"
#include "destination_directory_assessment.hpp"
#include "scratch_directory.hpp"
#include "windows_junction.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <array>
#include <chrono>
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
#include <sys/xattr.h>
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
CK_TEST(index_workspace_native_pins_original_directory_before_any_file_creation) {
    const ck::testing::ScratchDirectory scratch("files-index-workspace", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = scratch.path() / "root", moved = scratch.path() / "moved";
    fs::create_directory(root);
    private_root(root);
    auto workspace = take(open_native_index_workspace(address(root)));
    std::error_code error;
    fs::rename(root, moved, error);
#ifdef _WIN32
    if (error) {
        CK_CHECK(error.value() == ERROR_ACCESS_DENIED || error.value() == ERROR_SHARING_VIOLATION);
        auto file = take(workspace->create_file());
        const std::array data{std::byte{42}};
        write_all(*file, 0, data);
        file.reset();
        workspace.reset();
        fs::rename(root, moved);
        CK_CHECK(children(moved) == 0);
        std::cout << "index-workspace-native-pinned-parent-rename-refusal-qualified\n";
        return;
    }
#endif
    if (error)
        throw std::runtime_error("cannot relocate workspace fixture");
    fs::create_directory(root);
    private_root(root);
    {
        std::ofstream out(root / "sentinel");
        out << "untouched";
    }
    auto file = take(workspace->create_file());
    const std::array data{std::byte{0xff}, std::byte{0x80}};
    write_all(*file, 0, data);
    std::array<std::byte, 2> actual{};
    read_all(*file, 0, actual);
    file.reset();
    workspace.reset();
    CK_CHECK(actual == data && fs::file_size(root / "sentinel") == 9);
    CK_CHECK(children(root) == 1 && children(moved) == 0);
    std::cout << "index-workspace-native-held-root-replacement-qualified\n";
}
CK_TEST(index_workspace_native_budgeted_store_retains_factory_and_releases_native_files) {
    const ck::testing::ScratchDirectory scratch("files-index-budget", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    auto workspace = take(open_native_index_workspace(address(scratch.path())));
    const std::weak_ptr<IndexFileFactory> weak = workspace;
    auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
    DirectoryIndexContext context;
    context.generation[0] = std::byte{1};
    auto store = take(DirectoryIndexStore::create_new(workspace, budget, context));
    CK_CHECK(budget->charged_files() == 1);
    CK_CHECK(DirectoryIndexStore::create_new(workspace, budget, context).state() == ObservationState::Unknown);
    std::stop_source stop;
    stop.request_stop();
    CK_CHECK(!DirectoryIndexStore::create_new(workspace, budget, context, {}, stop.get_token()).value());
    workspace.reset();
    CK_CHECK(!weak.expired());
    DirectoryIndexRecord row{1, *NativeName::from_posix("record"), {}, Observation<bool>::available(false)};
    const auto position = take(store->append(row));
    CK_CHECK(take(store->seal()));
    CK_CHECK(take(store->read(position)).name == row.name);
    store.reset();
    CK_CHECK(weak.expired() && budget->charged_files() == 0 && budget->charged_bytes() == 0);
    CK_CHECK(children(scratch.path()) == 0);
    std::cout << "index-workspace-native-budget-factory-lifetime-qualified\n";
}
#ifndef _WIN32
CK_TEST(index_workspace_native_rechecks_private_mode_after_root_open_and_recovers_without_reopen) {
    const ck::testing::ScratchDirectory scratch("files-index-recheck", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    auto workspace = take(open_native_index_workspace(address(scratch.path())));
    CK_CHECK(chmod(scratch.path().c_str(), 0755) == 0);
    auto budget = std::make_shared<IndexStorageBudget>(1024, 1);
    DirectoryIndexContext context;
    context.generation[0] = std::byte{1};
    CK_CHECK(DirectoryIndexStore::create_new(workspace, budget, context).state() == ObservationState::Denied);
    CK_CHECK(budget->charged_files() == 0 && children(scratch.path()) == 0);
    CK_CHECK(chmod(scratch.path().c_str(), 0700) == 0);
    auto store = take(DirectoryIndexStore::create_new(workspace, budget, context));
    store.reset();
    workspace.reset();
    CK_CHECK(budget->charged_files() == 0 && children(scratch.path()) == 0);
    std::cout << "index-workspace-native-private-mode-recheck-qualified\n";
}
#endif
CK_TEST(index_file_native_independent_owners_and_record_store_preserve_lossless_names) {
    const ck::testing::ScratchDirectory scratch("files-index-store-native", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    DirectoryIndexContext context;
    context.generation[0] = std::byte{7};
    auto budget = std::make_shared<IndexStorageBudget>(1024 * 1024, 2);
    auto store = take(
        DirectoryIndexStore::create_new(take(open_native_index_workspace(address(scratch.path()))), budget, context));
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
    auto workspace = take(open_native_index_workspace(address(scratch.path())));
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
    auto file = take(workspace->create_file());
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
CK_TEST(index_scan_native_streams_100000_synthetic_records_without_retaining_address_table) {
    const ck::testing::ScratchDirectory scratch("files-index-scale", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    auto workspace = take(open_native_index_workspace(address(scratch.path())));
    auto budget = std::make_shared<IndexStorageBudget>(64ULL * 1024 * 1024, 1);
    DirectoryIndexContext context;
    context.generation[0] = std::byte{1};
    auto store = take(DirectoryIndexStore::create_new(workspace, budget, context, {100000, 64ULL * 1024 * 1024, {}}));
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 1; i <= 100000; ++i) {
        BasicMetadata metadata;
        metadata.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        metadata.logical_size = Observation<std::uint64_t>::available(i * 3);
        DirectoryIndexRecord record{i, *NativeName::from_posix("item-" + std::to_string(i)),
                                    Observation<BasicMetadata>::available(std::move(metadata)),
                                    Observation<bool>::available(i % 7 == 0)};
        CK_CHECK(store->append(record).value());
    }
    CK_CHECK(take(store->seal()));
    const auto built = std::chrono::steady_clock::now();
    auto cursor = take(store->begin_scan());
    std::uint64_t seen = 0, scanned_bytes = 0;
    std::size_t pages = 0, max_rows = 0, max_wire = 0;
    for (;;) {
        const auto page = take(store->read_page(cursor, {64, 16 * 1024}));
        CK_CHECK(page.rows.size() <= 64 && page.wire_bytes <= 16 * 1024);
        CK_CHECK(!page.rows.empty() || page.end);
        max_rows = std::max(max_rows, page.rows.size());
        max_wire = std::max(max_wire, page.wire_bytes);
        scanned_bytes += page.wire_bytes;
        ++pages;
        for (const auto &entry : page.rows) {
            ++seen;
            CK_CHECK(entry.record.row_id == seen && entry.address.row_id() == seen);
            CK_CHECK(entry.record.name == *NativeName::from_posix("item-" + std::to_string(seen)));
            const auto *metadata = entry.record.metadata.value();
            if (!metadata)
                throw std::runtime_error("scale record metadata unavailable");
            CK_CHECK(metadata->logical_size.value() && *metadata->logical_size.value() == seen * 3);
            CK_CHECK(metadata->kind.value() && *metadata->kind.value() == ObjectKind::RegularFile);
            CK_CHECK(entry.record.hidden.value() && *entry.record.hidden.value() == (seen % 7 == 0));
        }
        cursor = page.next;
        if (page.end)
            break;
    }
    const auto finished = std::chrono::steady_clock::now();
    CK_CHECK(seen == 100000 && store->rows() == seen && cursor.position() == seen);
    CK_CHECK(scanned_bytes == store->bytes() && budget->charged_bytes() == store->bytes());
    const auto end = take(store->read_page(cursor));
    CK_CHECK(end.end && end.rows.empty() && end.wire_bytes == 0);
    std::cout << "index-scan-native-100000-qualified rows=" << seen << " bytes=" << store->bytes() << " pages=" << pages
              << " max_page_rows=" << max_rows << " max_page_wire=" << max_wire
              << " append_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(built - start).count()
              << " scan_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished - built).count() << '\n';
    store.reset();
    workspace.reset();
    CK_CHECK(budget->charged_bytes() == 0 && budget->charged_files() == 0 && children(scratch.path()) == 0);
}
CK_TEST(index_sort_native_external_merges_100000_records_and_preserves_stable_source_rows) {
    const ck::testing::ScratchDirectory scratch("files-index-sort", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    auto factory = take(open_native_index_workspace(address(scratch.path())));
    auto budget = std::make_shared<IndexStorageBudget>(128ULL * 1024 * 1024, 32);
    DirectoryIndexContext context;
    context.generation[0] = std::byte{1};
    std::shared_ptr<DirectoryIndexStore> source =
        take(DirectoryIndexStore::create_new(factory, budget, context, {100000, 64ULL * 1024 * 1024, {}}));
    for (std::uint64_t i = 1; i <= 100000; ++i) {
        const auto digits = std::to_string(100001 - i);
        const auto name = *NativeName::from_posix("sort-" + std::string(6 - digits.size(), '0') + digits);
        BasicMetadata metadata;
        metadata.kind = Observation<ObjectKind>::available(ObjectKind::RegularFile);
        metadata.logical_size = Observation<std::uint64_t>::available(i * 3);
        DirectoryIndexRecord row{i, name, Observation<BasicMetadata>::available(std::move(metadata)),
                                 Observation<bool>::available(i % 7 == 0)};
        take(source->append(row));
    }
    take(source->seal());
    const auto source_bytes = source->bytes();
    const auto namespace_before = source->row_scope();
    const auto start = std::chrono::steady_clock::now();
    DirectoryIndexSortJob job(source, {PaneSortKey::Name, false, false}, {{4096, 4 * 1024 * 1024}, 256});
    std::size_t advances = 0;
    while (job.state() == DirectoryIndexSortState::Running) {
        job.advance();
        ++advances;
    }
    auto output = take(job.completed());
    const auto finished = std::chrono::steady_clock::now();
    CK_CHECK(output->row_scope() == namespace_before && output->rows() == 100000 &&
             source->state() == DirectoryIndexStoreState::Sealed);
    auto cursor = take(output->begin_scan());
    std::uint64_t seen = 0;
    for (;;) {
        auto page = take(output->read_page(cursor, {64, 16 * 1024}));
        for (const auto &row : page.rows) {
            ++seen;
            const auto expected = 100001 - seen;
            CK_CHECK(row.record.row_id == expected && row.address.row_id() == expected);
            const auto digits = std::to_string(seen);
            CK_CHECK(row.record.name ==
                     *NativeName::from_posix("sort-" + std::string(6 - digits.size(), '0') + digits));
            const auto *metadata = row.record.metadata.value();
            if (!metadata)
                throw std::runtime_error("sorted metadata missing");
            CK_CHECK(metadata->logical_size.value() && *metadata->logical_size.value() == expected * 3);
            CK_CHECK(row.record.hidden.value() && *row.record.hidden.value() == (expected % 7 == 0));
        }
        cursor = page.next;
        if (page.end)
            break;
    }
    const auto peak_bytes = budget->peak_charged_bytes();
    const auto peak_files = budget->peak_charged_files();
    CK_CHECK(seen == 100000 && output->bytes() == source_bytes && peak_bytes <= 3 * source_bytes && peak_files <= 32);
    std::cout << "index-sort-native-100000-qualified rows=" << seen << " bytes=" << source_bytes
              << " advances=" << advances << " peak_files=" << peak_files << " peak_bytes=" << peak_bytes
              << " sort_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(finished - start).count() << '\n';
    const auto lookup_start = std::chrono::steady_clock::now();
    DirectoryIndexLookupJob lookup_job(output, {256, 4 * 1024 * 1024});
    std::size_t lookup_advances = 0;
    while (lookup_job.state() == DirectoryIndexLookupJobState::Initializing ||
           lookup_job.state() == DirectoryIndexLookupJobState::Building) {
        if (++lookup_advances > 10000)
            throw std::runtime_error("native lookup budget");
        lookup_job.advance();
    }
    auto lookup = take(lookup_job.completed());
    const auto lookup_built = std::chrono::steady_clock::now();
    CK_CHECK(lookup->bytes() == 12800000 && lookup->rows() == 100000 && lookup->row_scope() == namespace_before);
    std::size_t queried = 0;
    for (const std::uint64_t rank : {99999ULL, 0ULL, 50000ULL, 99890ULL, 1ULL}) {
        const auto page = take(lookup->read_page(rank, {64, 16 * 1024}));
        CK_CHECK(page.rows.size() <= 64 && page.wire_bytes <= 16 * 1024 && !page.rows.empty());
        for (std::size_t i = 0; i < page.rows.size(); ++i) {
            const auto expected = 100000 - rank - i;
            CK_CHECK(page.rows[i].record.row_id == expected &&
                     *page.rows[i].record.metadata.value()->logical_size.value() == expected * 3);
            ++queried;
        }
        CK_CHECK(take(lookup->find_row({namespace_before, 100000 - rank})) == rank);
    }
    const auto end_page = take(lookup->read_page(100000));
    CK_CHECK(end_page.end && end_page.rows.empty());
    const auto lookup_done = std::chrono::steady_clock::now();
    std::cout << "index-lookup-native-100000-qualified rows=" << lookup->rows() << " bytes=" << lookup->bytes()
              << " advances=" << lookup_advances << " queried=" << queried << " build_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(lookup_built - lookup_start).count()
              << " query_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(lookup_done - lookup_built).count() << '\n';
    DirectoryIndexLookupJob filtered_job(output, {256, 4 * 1024 * 1024}, 64 * 1024 * 1024, {},
                                         PaneFilter{false, "sort-0", true});
    std::size_t filtered_advances = 0;
    const auto filtered_start = std::chrono::steady_clock::now();
    while (filtered_job.state() == DirectoryIndexLookupJobState::Initializing ||
           filtered_job.state() == DirectoryIndexLookupJobState::Building) {
        if (++filtered_advances > 10000)
            throw std::runtime_error("native filter budget");
        filtered_job.advance();
    }
    auto filtered = take(filtered_job.completed());
    CK_CHECK(filtered->total_rows() == 100000 && filtered->rows() == 85714 && filtered->bytes() == 12800000);
    std::size_t filtered_queried = 0;
    for (const std::uint64_t rank : {0ULL, 85713ULL, 40000ULL, 1ULL}) {
        auto page = take(filtered->read_page(rank, {64, 16 * 1024}));
        std::uint64_t ordinal = 0, id = 100000;
        for (; id >= 2; --id)
            if (id % 7 != 0) {
                if (ordinal == rank)
                    break;
                ++ordinal;
            }
        for (const auto &row : page.rows) {
            CK_CHECK(row.record.row_id == id && id % 7 != 0 && row.record.name.display_name().starts_with("sort-0"));
            CK_CHECK(*row.record.metadata.value()->logical_size.value() == id * 3);
            ++filtered_queried;
            do {
                --id;
            } while (id && id % 7 == 0);
        }
    }
    CK_CHECK(!take(filtered->locate_row({namespace_before, 1})).has_value() &&
             !take(filtered->locate_row({namespace_before, 7})).has_value());
    CK_CHECK(take(filtered->locate_row({namespace_before, 100000})) == 0 && take(filtered->read_page(85714)).end);
    std::cout << "index-filter-native-100000-qualified rows=" << filtered->total_rows()
              << " visible=" << filtered->rows() << " bytes=" << filtered->bytes() << " advances=" << filtered_advances
              << " queried=" << filtered_queried << " elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                       filtered_start)
                     .count()
              << '\n';
    filtered_job.abandon();
    filtered.reset();
    lookup_job.abandon();
    lookup.reset();
    output.reset();
    job.abandon();
    CK_CHECK(budget->charged_files() == 1 && budget->charged_bytes() == source_bytes);
    source.reset();
    factory.reset();
    CK_CHECK(budget->charged_files() == 0 && budget->charged_bytes() == 0 && children(scratch.path()) == 0);
}

CK_TEST(index_listing_native_streams_real_directory_through_private_sort_and_keeps_pin) {
    const ck::testing::ScratchDirectory source("files-index-listing-source");
    const ck::testing::ScratchDirectory scratch("files-index-listing-cache", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    for (int i = 0; i < 257; ++i) {
        std::ofstream file(source.path() / ("entry-" + std::to_string(i)), std::ios::binary);
        file << "payload";
        if (!file)
            throw std::runtime_error("write listing fixture");
    }
    constexpr std::array feature_bytes{std::byte{0}, std::byte{1}, std::byte{2}};
#ifdef _WIN32
    {
        std::ofstream stream(source.path() / "entry-0:ckInventory", std::ios::binary);
        stream.write(reinterpret_cast<const char *>(feature_bytes.data()), feature_bytes.size());
        stream.close();
        if (!stream)
            throw std::runtime_error("write native held inventory stream");
    }
#elif defined(__APPLE__)
    if (setxattr((source.path() / "entry-0").c_str(), "com.ckutilities.bound", feature_bytes.data(),
                 feature_bytes.size(), 0, 0) != 0)
        throw std::runtime_error("write native held inventory xattr");
#else
    if (setxattr((source.path() / "entry-0").c_str(), "user.ckutilities.bound", feature_bytes.data(),
                 feature_bytes.size(), 0) != 0)
        throw std::runtime_error("write native held inventory xattr");
#endif
    fs::create_directory(source.path() / "directory");
    {
        std::ofstream file(source.path() / "directory" / "nested");
        file << "nested";
    }
#ifdef _WIN32
    fs::create_directory(source.path() / "link");
    ck::testing::make_junction(source.path() / "link", source.path() / "directory");
    constexpr auto model = StorageModel::WindowsHandleAttributes;
    constexpr auto link_kind = ObjectKind::Junction;
#else
    fs::create_directory_symlink("directory", source.path() / "link");
#ifdef __APPLE__
    constexpr auto model = StorageModel::DarwinStatFlags;
#else
    constexpr auto model = StorageModel::LinuxStatx;
#endif
    constexpr auto link_kind = ObjectKind::SymbolicLink;
#endif
    NativeFileSystem filesystem;
    auto listing = take(DirectoryListing::open(filesystem, source.path(), {7, 4096, 4096}));
    const auto scope = take(listing->source_scope());
    const auto root = listing->identity();
    CK_CHECK(root.value() && root.value()->scope == scope);
    auto workspace = take(open_native_index_workspace(address(scratch.path())));
    auto budget = std::make_shared<IndexStorageBudget>(8 * 1024 * 1024, 16);
    std::array<std::byte, 16> generation{};
    generation[0] = std::byte{5};
    auto pane = std::make_unique<IndexedPane>();
    const PaneOrder order{PaneSortKey::Name, false, false};
    const auto request = pane->begin_listing(order);
    auto job = std::make_unique<DirectoryIndexListingJob>(request, std::move(listing), model, workspace, budget,
                                                          generation, DirectoryIndexStoreLimits{},
                                                          DirectoryIndexSortLimits{{16, 16 * 1024}, 8});
    std::size_t advances = 0;
    while (job->state() == DirectoryIndexListingState::Collecting ||
           job->state() == DirectoryIndexListingState::SortingNames ||
           job->state() == DirectoryIndexListingState::CheckingNames) {
        if (++advances > 10000)
            throw std::runtime_error("native listing budget");
        job->advance();
    }
    auto result = take(job->completed());
    CK_CHECK(result->store()->rows() == 259 && result->source_scope() == scope);
    CK_CHECK(result->before().value() && result->after().value());
    auto cursor = take(result->store()->begin_scan());
    std::optional<NativeName> previous;
    std::set<std::uint64_t> ids;
    std::size_t seen = 0;
    for (;;) {
        auto page = take(result->store()->read_page(cursor, {11, 16 * 1024}));
        CK_CHECK(page.rows.size() <= 11);
        for (const auto &row : page.rows) {
            ++seen;
            CK_CHECK(ids.insert(row.record.row_id).second && row.record.row_id >= 1 && row.record.row_id <= 259);
            if (previous)
                CK_CHECK(compare_native_names(*previous, row.record.name) < 0);
            previous = row.record.name;
            const auto &m = *row.record.metadata.value();
            CK_CHECK(m.identity.value() && m.identity.value()->scope == scope);
            const auto name = row.record.name.display_name();
            CK_CHECK(name != "nested");
            if (name == "directory")
                CK_CHECK(*m.kind.value() == ObjectKind::Directory);
            else if (name == "link")
                CK_CHECK(*m.kind.value() == link_kind);
            else
                CK_CHECK(*m.kind.value() == ObjectKind::RegularFile && *m.logical_size.value() == 7);
        }
        cursor = page.next;
        if (page.end)
            break;
    }
    CK_CHECK(seen == 259 && budget->charged_files() == 1);
    CK_CHECK(compare_identity(*root.value(), *result->directory_pin()->inspect_basic().value()->identity.value()) ==
             IdentityComparison::Same);
    std::cout << "index-listing-native-qualified rows=" << seen << " advances=" << advances
              << " bytes=" << result->store()->bytes() << '\n';
    IndexedPaneDataJob prepared(result, order, {{16, 16 * 1024}, 8}, {16, 16 * 1024});
    std::size_t prepared_steps = 0;
    while (prepared.state() == IndexedPaneDataJobState::Sorting ||
           prepared.state() == IndexedPaneDataJobState::Indexing ||
           prepared.state() == IndexedPaneDataJobState::CountingMarks) {
        if (++prepared_steps > 10000)
            throw std::runtime_error("native pane preparation budget");
        prepared.advance();
    }
    CK_CHECK(pane->publish(take(prepared.completed())) == IndexedPanePublish::Applied);
    prepared.abandon();
    auto captured = pane->capture_page(pane->state_stamp(), 0, {{11, 16 * 1024}, 16 * 1024});
    CK_CHECK(captured.state == IndexedPaneCaptureState::Ready && captured.page->rows().size() == 11 &&
             captured.page->total_rows() == 259);
    CK_CHECK(captured.page->rows()[0].focused && captured.page->rows()[0].id.scope == result->store()->row_scope());
    for (const auto &row : captured.page->rows())
        CK_CHECK(row.metadata.identity.value() && row.metadata.identity.value()->scope == scope);
    std::cout << "indexed-pane-native-qualified rows=" << captured.page->total_rows()
              << " page_rows=" << captured.page->rows().size() << " prepared_advances=" << prepared_steps << '\n';
    std::size_t marking_advances = 0;
    const auto mark = [&](const IndexedMarkCommand &command) {
        auto change = take(pane->begin_mark_change(pane->state_stamp(), command, 16));
        while (change->state() == IndexedMarkJobState::Running) {
            if (++marking_advances > 10000)
                throw std::runtime_error("native marking budget");
            change->advance();
        }
        CK_CHECK(pane->publish_marks(take(change->completed())) == IndexedPanePublish::Applied);
    };
    mark(IndexedMarkVisible{true});
    CK_CHECK(pane->selection().marks->total == 259 && pane->selection().candidates == 259);
    auto view = pane->data()->view();
    view.show_hidden = false;
    view.display_substring = "ENTRY-1";
    const auto view_request = take(pane->begin_view_change(pane->state_stamp(), view));
    IndexedPaneDataJob view_job(pane->data(), view_request, view, {{16, 16 * 1024}, 8}, {16, 16 * 1024},
                                256ULL * 1024 * 1024, pane->marks());
    std::size_t view_advances = 0;
    while (view_job.state() == IndexedPaneDataJobState::Sorting ||
           view_job.state() == IndexedPaneDataJobState::Indexing ||
           view_job.state() == IndexedPaneDataJobState::CountingMarks) {
        if (++view_advances > 10000)
            throw std::runtime_error("native view budget");
        view_job.advance();
    }
    CK_CHECK(pane->publish(take(view_job.completed())) == IndexedPanePublish::Applied);
    view_job.abandon();
    auto filtered_page = pane->capture_page(pane->state_stamp(), 0, {{11, 16 * 1024}, 16 * 1024});
    CK_CHECK(filtered_page.state == IndexedPaneCaptureState::Ready && filtered_page.page->total_rows() == 259 &&
             filtered_page.page->visible_rows() == 111);
    CK_CHECK(!pane->focused_position() && !captured.page->stamp().current());
    for (const auto &row : filtered_page.page->rows())
        CK_CHECK(row.marked && row.display_name.starts_with("entry-1") &&
                 row.id.scope == result->store()->row_scope() && row.metadata.identity.value()->scope == scope);
    std::cout << "indexed-pane-filter-native-qualified rows=" << filtered_page.page->total_rows()
              << " visible=" << filtered_page.page->visible_rows() << " page_rows=" << filtered_page.page->rows().size()
              << " advances=" << view_advances << '\n';
    CK_CHECK(pane->selection().marks->total == 259 && pane->selection().marks->visible == 111 &&
             pane->selection().marks->hidden == 148 && pane->selection().candidates == 0 &&
             pane->selection().state == PaneSourcesState::HiddenMarksNeedReview);
    mark(IndexedInvertVisible{});
    CK_CHECK(pane->selection().marks->total == 148 && pane->selection().marks->visible == 0);
    mark(IndexedMarkRange{filtered_page.page->rows().front().id, filtered_page.page->rows().back().id, true});
    CK_CHECK(pane->selection().marks->total == 159 && pane->selection().marks->visible == 11 &&
             pane->selection().marks->hidden == 148);
    std::cout
        << "indexed-pane-marks-native-qualified rows=259 total=159 visible=11 hidden=148 bitmap_bytes=576 advances="
        << marking_advances << '\n';
    CK_CHECK(!pane->begin_source_acquisition(pane->state_stamp()).value());
    view.display_substring.clear();
    view.show_hidden = true;
    const auto reveal_request = take(pane->begin_view_change(pane->state_stamp(), view));
    IndexedPaneDataJob revealed(pane->data(), reveal_request, view, {{16, 16 * 1024}, 8}, {16, 16 * 1024},
                                256ULL * 1024 * 1024, pane->marks());
    std::size_t reveal_steps = 0;
    while (revealed.state() == IndexedPaneDataJobState::Sorting ||
           revealed.state() == IndexedPaneDataJobState::Indexing ||
           revealed.state() == IndexedPaneDataJobState::CountingMarks) {
        if (++reveal_steps > 10000)
            throw std::runtime_error("native reveal budget");
        revealed.advance();
    }
    CK_CHECK(pane->publish(take(revealed.completed())) == IndexedPanePublish::Applied);
    revealed.abandon();
    CK_CHECK(pane->selection().marks->hidden == 0 && pane->selection().candidates == 159);
    auto acquisition = take(pane->begin_source_acquisition(pane->state_stamp(), {159, 64 * 1024, 64 * 1024, 16}));
    std::size_t acquisition_steps = 0;
    while (acquisition->state() == IndexedSourceJobState::Running) {
        if (++acquisition_steps > 10000)
            throw std::runtime_error("native acquisition budget");
        acquisition->advance();
    }
    auto acquired = take(acquisition->completed());
    CK_CHECK(acquired->sources().size() == 159 && acquired->review_required() && pane->accepts_sources(*acquired));
    for (const auto &entry : acquired->sources()) {
        CK_CHECK(entry.discovery_row().scope == pane->data()->row_scope());
        CK_CHECK(entry.binding_row().scope != entry.discovery_row().scope && entry.held()->metadata().identity.value());
        CK_CHECK(entry.cached_identity_match() == IdentityComparison::Same);
        if (entry.held()->name().display_name() == "link")
            CK_CHECK(*entry.held()->metadata().kind.value() == link_kind);
    }
    std::cout << "indexed-pane-sources-native-qualified rows=259 acquired=159 review_required=1 advances="
              << acquisition_steps << " names=" << acquired->native_name_bytes()
              << " diagnostics=" << acquired->diagnostic_bytes() << '\n';
    acquisition->abandon();
    mark(IndexedClearMarks{});
    const auto wanted = std::find_if(acquired->sources().begin(), acquired->sources().end(), [](const auto &entry) {
        return entry.held()->name().display_name() == "entry-0";
    });
    CK_CHECK(wanted != acquired->sources().end());
    CK_CHECK(pane->focus(pane->state_stamp(), wanted->discovery_row()) == IndexedPaneFocus::Applied);
    IndexedSourceLimits full_limits;
    full_limits.access = EntryAccess::MetadataAndAttributes | EntryAccess::MetadataAndSecurity |
                         EntryAccess::MetadataAndLayout | EntryAccess::MetadataAndInodeFlags;
    auto full_acquisition = take(pane->begin_source_acquisition(pane->state_stamp(), full_limits));
    std::size_t full_steps = 0;
    while (full_acquisition->state() == IndexedSourceJobState::Running) {
        if (++full_steps > 100)
            throw std::runtime_error("native full access acquisition budget");
        full_acquisition->advance();
    }
    auto full_source = take(full_acquisition->completed());
    CK_CHECK(full_source->sources().size() == 1 && full_source->sources()[0].held()->access() == full_limits.access);
    auto inventory_job = ObjectInventoryJob::for_held_source(full_source->sources()[0].held());
    fs::rename(source.path() / "entry-0", source.path() / "retired-0");
    {
        std::ofstream replacement(source.path() / "entry-0", std::ios::binary);
        replacement << "replacement";
        if (!replacement)
            throw std::runtime_error("write native inventory replacement");
    }
    std::size_t inventory_steps = 0;
    while (inventory_job->state() == ObjectInventoryJobState::Collecting ||
           inventory_job->state() == ObjectInventoryJobState::Paused) {
        if (++inventory_steps > 100)
            throw std::runtime_error("native held inventory budget");
        inventory_job->advance();
    }
    auto inventory = take(inventory_job->completed());
    CK_CHECK(inventory->held_source_for_worker() == full_source->sources()[0].held());
    CK_CHECK(take(inventory->held_identity_continuity()) == IdentityComparison::Same);
    CK_CHECK(take(inventory->data().before.value()->logical_size) == 7 &&
             take(inventory->data().after.value()->logical_size) == 7);
    const auto replacement = take(filesystem.inspect_basic(source.path() / "entry-0"));
    CK_CHECK(compare_identity(take(replacement.identity), take(inventory->data().before.value()->identity)) ==
             IdentityComparison::Different);
    bool found_feature = false;
#ifdef _WIN32
    const auto &channels = take(inventory->data().channels);
    CK_CHECK(take(inventory->compare_owner(channels.owner)) == IdentityComparison::Same);
    for (const auto &channel : channels.channels)
        if (channel.identifier.windows_units() && *channel.identifier.windows_units() == u":ckInventory:$DATA") {
            CK_CHECK(take(channel.logical_size) == feature_bytes.size());
            found_feature = true;
        }
#else
    const auto &attributes = take(inventory->data().attributes);
    CK_CHECK(take(inventory->compare_owner(attributes.owner)) == IdentityComparison::Same);
    for (const auto &read : inventory->data().attribute_values) {
        const auto &descriptor = attributes.attributes[read.catalog_index];
#ifdef __APPLE__
        constexpr auto feature_name = "com.ckutilities.bound";
#else
        constexpr auto feature_name = "user.ckutilities.bound";
#endif
        if (descriptor.identifier.posix_bytes() && *descriptor.identifier.posix_bytes() == feature_name) {
            const auto &value = take(read.value);
            CK_CHECK(value.bytes == std::vector<std::byte>(feature_bytes.begin(), feature_bytes.end()));
            CK_CHECK(take(inventory->compare_owner(value.owner)) == IdentityComparison::Same);
            found_feature = true;
        }
    }
#endif
    CK_CHECK(found_feature);
    std::cout << "indexed-held-inventory-native-qualified rows=259 acquired=1 feature=1 identity_same=1 advances="
              << inventory_steps << '\n';
    auto destination_directory = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(source.path())));
    DirectoryInventoryJob destination_inventory(destination_directory);
    std::size_t destination_steps = 0;
    while (destination_inventory.state() == DirectoryInventoryState::Collecting ||
           destination_inventory.state() == DirectoryInventoryState::Paused) {
        if (++destination_steps > 100)
            throw std::runtime_error("native destination inventory budget");
        destination_inventory.advance();
    }
    auto destination = take(DestinationDirectoryAssessment::inspect(take(destination_inventory.completed())));
    CK_CHECK(take(destination->context()) == IdentityComparison::Same);
    std::size_t advertised = 0, not_advertised = 0, unavailable_features = 0;
    const auto &native_location = *destination->inventory_for_worker()->data().location_after.value();
    for (const auto feature : destination_advertised_features) {
        const auto &observed = destination->advertisement(feature);
        const auto raw = advertised_feature(native_location.features, feature);
        CK_CHECK(observed.state() == raw.state());
        if (observed.value()) {
            CK_CHECK(*observed.value() == *raw.value());
            if (*observed.value() == FeatureAdvertisement::Advertised)
                ++advertised;
            else
                ++not_advertised;
        } else {
            CK_CHECK(observed.unavailable_detail()->error == raw.unavailable_detail()->error);
            ++unavailable_features;
        }
    }
    CK_CHECK(advertised + not_advertised + unavailable_features == destination_advertised_features.size());
    for (const auto access_request : {DirectoryAccess::AddFile, DirectoryAccess::AddDirectory}) {
        const auto &access = destination->inventory_for_worker()->data().access[static_cast<std::size_t>(access_request)];
        const auto &assessed = destination->creation(access_request);
        if (!access.value())
            CK_CHECK(assessed.unavailable_detail()->error == access.unavailable_detail()->error);
        else if (destination->read_only_mount().value())
            CK_CHECK(take(assessed) == (*destination->read_only_mount().value()
                                            ? DestinationCreationObservation::ReadOnlyMountReported
                                            : DestinationCreationObservation::AccessReportedWithoutReadOnlyMount));
        else
            CK_CHECK(assessed.unavailable_detail()->error ==
                     destination->read_only_mount().unavailable_detail()->error);
    }
    std::cout << "destination-assessment-native-qualified features=12 advertised=" << advertised
              << " not_advertised=" << not_advertised << " unavailable=" << unavailable_features
              << " variable_bytes=" << destination->variable_bytes() << '\n';
    destination_inventory.abandon();
    destination_directory.reset();
    destination.reset();
    inventory_job->abandon();
    full_acquisition->abandon();
    full_source.reset();
    job.reset();
    result.reset();
    workspace.reset();
    CK_CHECK(budget->charged_files() == 2);
    pane.reset();
    CK_CHECK(!captured.page->stamp().current() && captured.page->rows().size() == 11);
    CK_CHECK(budget->charged_files() == 0 && budget->charged_bytes() == 0 && children(scratch.path()) == 0);
    CK_CHECK(!acquired->stamp().current() && acquired->sources().front().held()->inspect_basic().value());
    acquired.reset();
    CK_CHECK(take(inventory->object_for_worker().inspect_basic().value()->logical_size) == 7);
    inventory.reset();
}
CK_TEST(index_listing_native_rejects_observed_directory_changes_after_enumeration) {
    const ck::testing::ScratchDirectory source("files-index-listing-change");
    const ck::testing::ScratchDirectory scratch("files-index-listing-change-cache",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    private_root(scratch.path());
    {
        std::ofstream file(source.path() / "entry");
        file << "payload";
    }
    NativeFileSystem filesystem;
#ifdef _WIN32
    constexpr auto model = StorageModel::WindowsHandleAttributes;
#elif defined(__APPLE__)
    constexpr auto model = StorageModel::DarwinStatFlags;
#else
    constexpr auto model = StorageModel::LinuxStatx;
#endif
    auto budget = std::make_shared<IndexStorageBudget>(1024 * 1024, 16);
    std::array<std::byte, 16> generation{};
    generation[0] = std::byte{9};
    DirectoryIndexListingJob job({AdapterScope{}, 1}, take(DirectoryListing::open(filesystem, source.path())), model,
                                 take(open_native_index_workspace(address(scratch.path()))), budget, generation);
    while (job.state() != DirectoryIndexListingState::CheckingNames) {
        if (job.state() != DirectoryIndexListingState::Collecting &&
            job.state() != DirectoryIndexListingState::SortingNames)
            throw std::runtime_error("native change setup");
        job.advance();
    }
    fs::last_write_time(source.path(), fs::last_write_time(source.path()) + std::chrono::seconds(2));
    job.advance();
    CK_CHECK(job.state() == DirectoryIndexListingState::Failed && job.failure()->state == ObservationState::Changed);
    CK_CHECK(!job.completed().value() && budget->charged_files() == 0 && budget->charged_bytes() == 0 &&
             children(scratch.path()) == 0);
}
