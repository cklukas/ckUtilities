// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include "windows_junction.hpp"
#else
#include <sys/stat.h>
#include <sys/xattr.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#include <membership.h>
#endif
#include <unistd.h>
#endif


namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template <class T> const T& required(const Observation<T>& result) {
    if (!result.value()) {
        const auto& e = *result.unavailable_detail();
        std::cerr << "security observation: " << static_cast<int>(e.state) << " " << e.error.operation << " " << e.error.code << std::endl;
        throw std::runtime_error("required security observation unavailable");
    }
    return *result.value();
}
void write_file(const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "fixture";
    file.close();
    if (!file) throw std::runtime_error("cannot write security fixture");
}
std::unique_ptr<OpenedEntry> open_entry(const NativeFileSystem& filesystem, const fs::path& path,
    EntryAccess access = EntryAccess::MetadataAndSecurity) {
    auto parent_result = filesystem.open_directory(path.parent_path());
    (void)required(parent_result);
    auto parent = std::move(parent_result).take_value();
    const auto filename = path.filename();
#if defined(_WIN32)
    std::u16string units;
    for (const auto unit : filename.native()) units.push_back(static_cast<char16_t>(unit));
    auto name = NativeName::from_windows(std::move(units));
#else
    auto name = NativeName::from_posix(filename.native());
#endif
    if (!name) throw std::runtime_error("invalid security fixture name");
    auto opened = parent->open_child(*name, access);
    (void)required(opened);
    return std::move(opened).take_value();
}
}

CK_TEST(native_security_query_limits_foreign_models_and_invalid_parts_remain_explicit) {
    const ck::testing::ScratchDirectory scratch("files-security-contract");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    CK_CHECK(opened->read_security(static_cast<SecurityPart>(255)).state() == ObservationState::Failed);
    CK_CHECK(opened->read_security(SecurityPart::LinuxAccessAcl, {0, 1}).state() == ObservationState::Failed);
#if defined(_WIN32)
    constexpr auto foreign = SecurityPart::LinuxAccessAcl;
#else
    constexpr auto foreign = SecurityPart::WindowsAccess;
#endif
    CK_CHECK(opened->read_security(foreign).state() == ObservationState::Unsupported);
}

CK_TEST(native_move_source_access_is_explicitly_qualified) {
    const ck::testing::ScratchDirectory scratch("files-move-source-access");
    write_file(scratch.path() / "file");
    NativeFileSystem filesystem;
    auto parent_result = filesystem.open_directory(scratch.path());
    CK_CHECK(parent_result.value());
    if (!parent_result.value()) return;
    auto parent = std::move(parent_result).take_value();
#if defined(_WIN32)
    const auto name = NativeName::from_windows(u"file");
#else
    const auto name = NativeName::from_posix("file");
#endif
    CK_CHECK(name.has_value());
    if (!name) return;
    auto opened = parent->open_child(*name, EntryAccess::ContentRead | EntryAccess::MoveSource);
#if defined(_WIN32)
    CK_CHECK(opened.value());
#else
    CK_CHECK(!opened.value() && opened.state() == ObservationState::Unsupported &&
             opened.unavailable_detail()->error.code == static_cast<std::int64_t>(AdapterError::UnsupportedQuery));
#endif
}

#if defined(_WIN32)
CK_TEST(native_move_source_access_refuses_an_existing_writer) {
    const ck::testing::ScratchDirectory scratch("files-move-source-writer");
    const auto path = scratch.path() / "file";
    write_file(path);
    const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CK_CHECK(writer != INVALID_HANDLE_VALUE);
    if (writer == INVALID_HANDLE_VALUE) return;

    NativeFileSystem filesystem;
    auto parent_result = filesystem.open_directory(scratch.path());
    CK_CHECK(parent_result.value());
    if (!parent_result.value()) {
        CloseHandle(writer);
        return;
    }
    auto parent = std::move(parent_result).take_value();
    const auto opened = parent->open_child(NativeName::from_windows(u"file").value(),
                                           EntryAccess::ContentRead | EntryAccess::MoveSource);
    CK_CHECK(!opened.value() && opened.state() == ObservationState::Denied);
    if (!opened.value() && opened.unavailable_detail()) {
        // NtCreateFile preserves the native status domain; compare the
        // documented status value rather than the Win32 translation.
        CK_CHECK(opened.unavailable_detail()->error.domain == ErrorDomain::NtStatus);
        CK_CHECK(static_cast<std::uint32_t>(opened.unavailable_detail()->error.code) ==
                 static_cast<std::uint32_t>(STATUS_SHARING_VIOLATION));
    }
    CloseHandle(writer);
}
#endif

#if defined(__linux__)
namespace {
NativeBytes linux_acl_fixture() {
    // Linux UAPI little-endian v2: owner, named uid, group, mask, other.
    NativeBytes bytes(44);
    bytes[0] = std::byte{2};
    const std::uint16_t tags[] = {1, 2, 4, 16, 32};
    for (std::size_t entry = 0; entry < 5; ++entry) {
        const auto at = 4 + entry * 8;
        bytes[at] = static_cast<std::byte>(tags[entry]);
        bytes[at + 2] = std::byte{6};
        const auto id = entry == 1 ? 65534U : 0xffffffffU;
        for (std::size_t index = 0; index < 4; ++index) bytes[at + 4 + index] = static_cast<std::byte>((id >> (index * 8U)) & 255U);
    }
    return bytes;
}
void set_acl(const fs::path& path, const char* name, const NativeBytes& bytes) {
    if (setxattr(path.c_str(), name, bytes.data(), bytes.size(), 0) != 0)
        throw std::runtime_error("cannot write Linux ACL fixture: " + std::to_string(errno));
}
}
CK_TEST(native_linux_acl_queries_keep_absence_and_native_access_default_acl_bytes) {
    const ck::testing::ScratchDirectory scratch("files-security-linux");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto absent = opened->read_security(SecurityPart::LinuxAccessAcl);
    CK_CHECK(required(absent).presence == SecurityPresence::Absent && required(absent).bytes.empty());
    const auto bytes = linux_acl_fixture();
    set_acl(path, "system.posix_acl_access", bytes);
    const auto before = opened->inspect_basic();
    fs::rename(path, scratch.path() / "retired");
    write_file(path);
    fs::remove(scratch.path() / "retired");
    const auto value = opened->read_security(SecurityPart::LinuxAccessAcl);
    CK_CHECK(required(value).encoding == SecurityEncoding::LinuxPosixAclXattrV2);
    CK_CHECK(required(value).presence == SecurityPresence::Present && required(value).bytes == bytes);
    CK_CHECK(compare_identity(required(required(value).owner), required(required(before).identity)) == IdentityComparison::Same);
    CK_CHECK(opened->read_security(SecurityPart::LinuxAccessAcl, {16, 100}).state() == ObservationState::Unknown);
    CK_CHECK(opened->read_security(SecurityPart::LinuxAccessAcl, {1024, 4}).state() == ObservationState::Unknown);
    const auto directory = scratch.path() / "directory";
    fs::create_directory(directory);
    set_acl(directory, "system.posix_acl_default", bytes);
    auto folder = open_entry(filesystem, directory);
    CK_CHECK(required(folder->read_security(SecurityPart::LinuxDefaultAcl)).bytes == bytes);
    fs::create_symlink("directory", scratch.path() / "link");
    auto link = open_entry(filesystem, scratch.path() / "link");
    const auto link_acl = link->read_security(SecurityPart::LinuxDefaultAcl);
    CK_CHECK((link_acl.value() && link_acl.value()->presence == SecurityPresence::Absent) || link_acl.state() == ObservationState::Unsupported);
}
#elif defined(__APPLE__)
namespace {
struct AclOwner { acl_t value; ~AclOwner() { if (value) acl_free(value); } };
NativeBytes acl_bytes(acl_t acl) {
    const auto size = acl_size(acl);
    if (size < 0) throw std::runtime_error("cannot size native ACL");
    NativeBytes result(static_cast<std::size_t>(size));
    if (acl_copy_ext(result.data(), acl, size) != size) throw std::runtime_error("cannot export native ACL");
    return result;
}
}
CK_TEST(native_darwin_acl_export_preserves_entries_flags_and_the_opened_owner) {
    const ck::testing::ScratchDirectory scratch("files-security-darwin");
    const auto path = scratch.path() / "file";
    write_file(path);
    AclOwner acl{acl_init(2)};
    if (!acl.value) throw std::runtime_error("cannot allocate ACL fixture");
    uuid_t qualifier{};
    if (mbr_uid_to_uuid(getuid(), qualifier) != 0) throw std::runtime_error("cannot resolve fixture principal");
    for (const auto tag : {ACL_EXTENDED_DENY, ACL_EXTENDED_ALLOW}) {
        acl_entry_t entry{};
        acl_permset_t permissions{};
        if (acl_create_entry(&acl.value, &entry) != 0 || acl_set_tag_type(entry, tag) != 0 ||
            acl_set_qualifier(entry, qualifier) != 0 || acl_get_permset(entry, &permissions) != 0 ||
            acl_add_perm(permissions, tag == ACL_EXTENDED_DENY ? ACL_WRITE_DATA : ACL_READ_DATA) != 0)
            throw std::runtime_error("cannot prepare ACL fixture entry");
    }
    if (acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, acl.value) != 0) throw std::runtime_error("cannot set ACL fixture");
    AclOwner independent{acl_get_file(path.c_str(), ACL_TYPE_EXTENDED)};
    if (!independent.value) throw std::runtime_error("cannot independently inspect ACL fixture");
    const auto expected = acl_bytes(independent.value);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto before = opened->inspect_basic();
    fs::rename(path, scratch.path() / "retired");
    write_file(path);
    fs::remove(scratch.path() / "retired");
    const auto value = opened->read_security(SecurityPart::DarwinExtendedAcl);
    CK_CHECK(required(value).encoding == SecurityEncoding::DarwinExternalAcl);
    CK_CHECK(required(value).bytes == expected);
    CK_CHECK(compare_identity(required(required(value).owner), required(required(before).identity)) == IdentityComparison::Same);
    CK_CHECK(opened->read_security(SecurityPart::DarwinExtendedAcl, {1, 100}).state() == ObservationState::Unknown);
    CK_CHECK(opened->read_security(SecurityPart::DarwinExtendedAcl, {1024 * 1024, 1}).state() == ObservationState::Unknown);
}
CK_TEST(native_darwin_directory_inheritance_acl_and_final_link_security_remain_separate) {
    const ck::testing::ScratchDirectory scratch("files-security-darwin-links");
    const auto target = scratch.path() / "target";
    fs::create_directory(target);
    NativeFileSystem filesystem;
    auto empty = open_entry(filesystem, target);
    const auto absent = empty->read_security(SecurityPart::DarwinExtendedAcl);
    CK_CHECK(required(absent).presence == SecurityPresence::Absent && required(absent).bytes.empty());
    AclOwner acl{acl_init(1)};
    if (!acl.value) throw std::runtime_error("cannot allocate directory ACL");
    uuid_t qualifier{}; acl_entry_t entry{}; acl_permset_t permissions{}; acl_flagset_t flags{};
    if (mbr_uid_to_uuid(getuid(), qualifier) != 0 || acl_create_entry(&acl.value, &entry) != 0 ||
        acl_set_tag_type(entry, ACL_EXTENDED_DENY) != 0 || acl_set_qualifier(entry, qualifier) != 0 ||
        acl_get_permset(entry, &permissions) != 0 || acl_add_perm(permissions, ACL_WRITE_DATA) != 0 ||
        acl_get_flagset_np(entry, &flags) != 0 || acl_add_flag_np(flags, ACL_ENTRY_FILE_INHERIT) != 0 ||
        acl_add_flag_np(flags, ACL_ENTRY_DIRECTORY_INHERIT) != 0 || acl_add_flag_np(flags, ACL_ENTRY_ONLY_INHERIT) != 0 ||
        acl_set_file(target.c_str(), ACL_TYPE_EXTENDED, acl.value) != 0)
        throw std::runtime_error("cannot set inheritable ACL fixture");
    AclOwner independent{acl_get_file(target.c_str(), ACL_TYPE_EXTENDED)};
    if (!independent.value) throw std::runtime_error("cannot inspect directory ACL");
    const auto expected = acl_bytes(independent.value);
    auto folder = open_entry(filesystem, target);
    CK_CHECK(required(folder->read_security(SecurityPart::DarwinExtendedAcl)).bytes == expected);
    fs::create_symlink("target", scratch.path() / "link");
    auto link = open_entry(filesystem, scratch.path() / "link");
    const auto observed = link->read_security(SecurityPart::DarwinExtendedAcl);
    if (observed.value()) CK_CHECK(observed.value()->bytes != expected);
    else {
        const auto& e = *observed.unavailable_detail();
        std::cerr << "Darwin symlink ACL query: " << static_cast<int>(e.state) << " " << e.error.operation << " " << e.error.code << std::endl;
        CK_CHECK(observed.state() == ObservationState::Unsupported || observed.state() == ObservationState::Denied);
    }
}
#elif defined(_WIN32)
CK_TEST(native_windows_security_queries_keep_protected_dacl_and_owner_after_unlink) {
    const ck::testing::ScratchDirectory scratch("files-security-windows");
    const auto path = scratch.path() / "file";
    write_file(path);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;DC;;;WD)(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("cannot create security fixture descriptor");
    struct Free { PSECURITY_DESCRIPTOR value; ~Free() { LocalFree(value); } } free{descriptor};
    const auto handle = CreateFileW(path.c_str(), WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open security fixture writer");
    const auto written = SetKernelObjectSecurity(handle, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    CloseHandle(handle);
    if (!written) throw std::runtime_error("cannot set security fixture");
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    auto narrow = open_entry(filesystem, path, EntryAccess::MetadataOnly);
    CK_CHECK(narrow->read_security(SecurityPart::WindowsAccess).state() == ObservationState::Denied);
    CK_CHECK(opened->read_security(SecurityPart::WindowsFullDescriptor).state() == ObservationState::Denied);
    const auto before = opened->inspect_basic();
    const auto security = opened->read_security(SecurityPart::WindowsAccess);
    const auto& value = required(security);
    CK_CHECK(value.encoding == SecurityEncoding::WindowsRelativeDescriptor);
    // The portable bounds validator has already accepted the complete buffer.
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    CK_CHECK(GetSecurityDescriptorControl(const_cast<std::byte*>(value.bytes.data()), &control, &revision) != 0);
    CK_CHECK((control & SE_DACL_PROTECTED) != 0);
    BOOL present = FALSE, defaulted = FALSE; PACL dacl = nullptr;
    CK_CHECK(GetSecurityDescriptorDacl(const_cast<std::byte*>(value.bytes.data()), &present, &dacl, &defaulted) != 0);
    CK_CHECK(present && dacl && dacl->AceCount == 2);
    fs::rename(path, scratch.path() / "retired");
    write_file(path);
    fs::remove(scratch.path() / "retired");
    const auto retained = opened->read_security(SecurityPart::WindowsAccess);
    CK_CHECK(required(retained).bytes == value.bytes);
    CK_CHECK(compare_identity(required(required(retained).owner), required(required(before).identity)) == IdentityComparison::Same);
    CK_CHECK(opened->read_security(SecurityPart::WindowsAccess, {1, 100}).state() == ObservationState::Unknown);
    CK_CHECK(opened->read_security(SecurityPart::WindowsAccess, {1024 * 1024, 1}).state() == ObservationState::Unknown);
    CK_CHECK(opened->read_security(SecurityPart::WindowsIntegrity).state() == ObservationState::Available);
}
CK_TEST(native_windows_full_security_access_matches_native_rights_without_privilege_changes) {
    const ck::testing::ScratchDirectory scratch("files-security-windows-full");
    const auto path = scratch.path() / "file";
    write_file(path);
    const auto reference = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | SYNCHRONIZE | READ_CONTROL | ACCESS_SYSTEM_SECURITY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    const auto native_error = GetLastError();
    NativeFileSystem filesystem;
    auto parent_result = filesystem.open_directory(scratch.path());
    (void)required(parent_result);
    auto parent = std::move(parent_result).take_value();
    auto opened = parent->open_child(*NativeName::from_windows(u"file"), EntryAccess::MetadataAndFullSecurity);
    if (reference == INVALID_HANDLE_VALUE) {
        CK_CHECK(native_error == ERROR_PRIVILEGE_NOT_HELD || native_error == ERROR_ACCESS_DENIED);
        CK_CHECK(opened.state() == ObservationState::Denied && !opened.value());
    } else {
        CloseHandle(reference);
        const auto value = required(opened)->read_security(SecurityPart::WindowsFullDescriptor);
        CK_CHECK(required(value).native_query_mask == BACKUP_SECURITY_INFORMATION);
    }
}
#endif

