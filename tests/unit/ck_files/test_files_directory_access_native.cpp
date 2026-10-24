// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "scratch_directory.hpp"
#include "cvision/testing/cktest.hpp"
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#include <membership.h>
#endif
#endif
namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template<class T> T take(Observation<T> value) {
    if (!value.value()) throw std::runtime_error("required native directory access unavailable");
    return std::move(value).take_value();
}
}
CK_TEST(native_directory_access_checks_every_request_without_creating_children_or_consuming_enumeration) {
    const ck::testing::ScratchDirectory scratch("files-access-check"); NativeFileSystem filesystem;
    fs::create_directory(scratch.path() / "a"); fs::create_directory(scratch.path() / "b");
    auto cursor = take(filesystem.open_directory(scratch.path()));
    const auto first = cursor->read(1); CK_CHECK(first.names.size() == 1);
    for (auto request : {DirectoryAccess::List, DirectoryAccess::Search, DirectoryAccess::AddFile, DirectoryAccess::AddDirectory}) {
        const auto result = take(cursor->check_access(request));
        CK_CHECK(result.requested == request && result.native_requested_mask != 0);
        CK_CHECK(compare_identity(result.directory, take(cursor->identity())) == IdentityComparison::Same);
#ifdef _WIN32
        CK_CHECK(result.family == DirectoryAccessFamily::WindowsOpenById);
        const DWORD right = request == DirectoryAccess::List ? FILE_LIST_DIRECTORY : request == DirectoryAccess::Search ? FILE_TRAVERSE : request == DirectoryAccess::AddFile ? FILE_ADD_FILE : FILE_ADD_SUBDIRECTORY;
        CK_CHECK(result.native_requested_mask == (right | FILE_READ_ATTRIBUTES | SYNCHRONIZE));
#else
#ifdef __linux__
        CK_CHECK(result.family == DirectoryAccessFamily::LinuxFaccessat2);
#else
        CK_CHECK(result.family == DirectoryAccessFamily::DarwinFaccessat);
#endif
        const auto mode = request == DirectoryAccess::List ? R_OK : request == DirectoryAccess::Search ? X_OK : W_OK | X_OK;
        CK_CHECK(result.native_requested_mask == static_cast<std::uint32_t>(mode));
#endif
    }
    const auto rest = cursor->read(10); CK_CHECK(rest.names.size() == 1 && rest.names[0] != first.names[0]);
    CK_CHECK(rest.state == DirectoryReadState::End);
    CK_CHECK(std::distance(fs::directory_iterator(scratch.path()), fs::directory_iterator{}) == 2);
    CK_CHECK(cursor->check_access(static_cast<DirectoryAccess>(255)).state() == ObservationState::Failed);
}
CK_TEST(native_directory_access_checks_the_held_object_after_original_name_replacement) {
    const ck::testing::ScratchDirectory scratch("files-access-rename"); NativeFileSystem filesystem;
    const auto original = scratch.path() / "original", moved = scratch.path() / "moved";
    fs::create_directory(original); auto cursor = take(filesystem.open_directory(original));
    fs::rename(original, moved); fs::create_directory(original);
    const auto checked = take(cursor->check_access(DirectoryAccess::AddFile));
    auto replacement = take(filesystem.open_directory(original));
    CK_CHECK(compare_identity(checked.directory, take(cursor->identity())) == IdentityComparison::Same);
    CK_CHECK(compare_identity(checked.directory, take(replacement->identity())) == IdentityComparison::Different);
    CK_CHECK(fs::is_empty(original) && fs::is_empty(moved));
}
#ifndef _WIN32
CK_TEST(native_directory_access_uses_current_effective_permissions_instead_of_cached_mode_bits) {
    CK_CHECK(geteuid() != 0);
    const ck::testing::ScratchDirectory scratch("files-access-denied"); NativeFileSystem filesystem;
    const auto path = scratch.path() / "directory"; fs::create_directory(path);
    auto cursor = take(filesystem.open_directory(path));
    struct Restore { fs::path path; ~Restore() { chmod(path.c_str(), 0700); } } restore{path};
    CK_CHECK(chmod(path.c_str(), 0500) == 0);
    CK_CHECK(cursor->check_access(DirectoryAccess::List).value());
    CK_CHECK(cursor->check_access(DirectoryAccess::Search).value());
    for (auto request : {DirectoryAccess::AddFile, DirectoryAccess::AddDirectory}) {
        const auto denied = cursor->check_access(request);
        CK_CHECK(denied.state() == ObservationState::Denied && denied.unavailable_detail()->error.domain == ErrorDomain::Posix);
    }
    CK_CHECK(chmod(path.c_str(), 0700) == 0);
    CK_CHECK(cursor->check_access(DirectoryAccess::AddFile).value());
    CK_CHECK(fs::is_empty(path));
}
#else
CK_TEST(native_directory_access_rechecks_windows_dacl_without_backup_privilege_bypass) {
    const ck::testing::ScratchDirectory scratch("files-access-dacl"); NativeFileSystem filesystem;
    const auto path = scratch.path() / "directory"; fs::create_directory(path);
    auto cursor = take(filesystem.open_directory(path));
    struct Security {
        std::wstring path; PSECURITY_DESCRIPTOR descriptor = nullptr; PACL old_acl = nullptr, new_acl = nullptr; bool changed = false;
        ~Security() {
            if (changed) SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, old_acl, nullptr);
            if (new_acl) LocalFree(new_acl);
            if (descriptor) LocalFree(descriptor);
        }
    } security{path.native()};
    CK_CHECK(GetNamedSecurityInfoW(security.path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &security.old_acl, nullptr, &security.descriptor) == ERROR_SUCCESS);
    alignas(DWORD) std::array<std::byte, SECURITY_MAX_SID_SIZE> sid{}; DWORD sid_bytes = static_cast<DWORD>(sid.size());
    CK_CHECK(CreateWellKnownSid(WinWorldSid, nullptr, sid.data(), &sid_bytes));
    EXPLICIT_ACCESSW entry{}; entry.grfAccessPermissions = FILE_ADD_FILE; entry.grfAccessMode = DENY_ACCESS;
    entry.Trustee.TrusteeForm = TRUSTEE_IS_SID; entry.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entry.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
    CK_CHECK(SetEntriesInAclW(1, &entry, security.old_acl, &security.new_acl) == ERROR_SUCCESS);
    CK_CHECK(SetNamedSecurityInfoW(security.path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, security.new_acl, nullptr) == ERROR_SUCCESS);
    security.changed = true;
    const auto denied = cursor->check_access(DirectoryAccess::AddFile);
    CK_CHECK(denied.state() == ObservationState::Denied && denied.unavailable_detail()->error.code == ERROR_ACCESS_DENIED);
    CK_CHECK(cursor->check_access(DirectoryAccess::AddDirectory).value());
    CK_CHECK(SetNamedSecurityInfoW(security.path.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, security.old_acl, nullptr) == ERROR_SUCCESS);
    security.changed = false;
    CK_CHECK(cursor->check_access(DirectoryAccess::AddFile).value());
    CK_CHECK(fs::is_empty(path));
}
#endif

#ifdef __APPLE__
CK_TEST(native_directory_access_darwin_acl_denial_is_not_replaced_by_mode_bit_inference) {
    const ck::testing::ScratchDirectory scratch("files-access-acl"); NativeFileSystem filesystem;
    const auto path = scratch.path() / "directory"; fs::create_directory(path);
    auto cursor = take(filesystem.open_directory(path));
    struct Security {
        fs::path path; acl_t old_acl = nullptr, new_acl = nullptr; bool changed = false;
        ~Security() {
            if (changed) acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, old_acl);
            if (new_acl) acl_free(new_acl);
            if (old_acl) acl_free(old_acl);
        }
    } security{path};
    errno = 0;
    security.old_acl = acl_get_file(path.c_str(), ACL_TYPE_EXTENDED);
    const int acl_error = errno;
    // Darwin reports ENOENT for an absent extended ACL. Restore that semantic
    // absence using the native empty ACL representation, not a guessed policy.
    if (!security.old_acl && acl_error == ENOENT) security.old_acl = acl_init(0);
    security.new_acl = acl_init(1);
    CK_CHECK(security.old_acl && security.new_acl);
    uuid_t qualifier{}; CK_CHECK(mbr_uid_to_uuid(geteuid(), qualifier) == 0);
    acl_entry_t entry{}; acl_permset_t permissions{};
    CK_CHECK(acl_create_entry(&security.new_acl, &entry) == 0);
    CK_CHECK(acl_set_tag_type(entry, ACL_EXTENDED_DENY) == 0);
    CK_CHECK(acl_set_qualifier(entry, qualifier) == 0);
    CK_CHECK(acl_get_permset(entry, &permissions) == 0);
    CK_CHECK(acl_add_perm(permissions, ACL_ADD_FILE) == 0);
    CK_CHECK(acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, security.new_acl) == 0);
    security.changed = true;
    struct stat native{}; CK_CHECK(stat(path.c_str(), &native) == 0 && (native.st_mode & S_IWUSR) != 0);
    CK_CHECK(cursor->check_access(DirectoryAccess::AddFile).state() == ObservationState::Denied);
    CK_CHECK(cursor->check_access(DirectoryAccess::List).value());
    CK_CHECK(cursor->check_access(DirectoryAccess::Search).value());
    CK_CHECK(acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, security.old_acl) == 0); security.changed = false;
    CK_CHECK(cursor->check_access(DirectoryAccess::AddFile).value());
    CK_CHECK(fs::is_empty(path));
}
#endif
