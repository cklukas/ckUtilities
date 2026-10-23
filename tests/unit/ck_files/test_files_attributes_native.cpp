// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include "scratch_directory.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <bit>
#include <cstring>
#include "windows_junction.hpp"
#else
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif


namespace {
using namespace ck::files;
namespace fs = std::filesystem;
template <class T> const T& required(const Observation<T>& result) {
    if (!result.value()) {
        const auto& e = *result.unavailable_detail();
        std::cerr << "attribute observation: " << static_cast<int>(e.state) << " " << e.error.operation << " " << e.error.code << std::endl;
        throw std::runtime_error("required attribute observation unavailable");
    }
    return *result.value();
}
void write_file(const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "fixture";
    file.close();
    if (!file) throw std::runtime_error("cannot write attribute fixture");
}
std::unique_ptr<OpenedEntry> open_entry(const NativeFileSystem& filesystem, const fs::path& path,
    EntryAccess access = EntryAccess::MetadataAndAttributes) {
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
    if (!name) throw std::runtime_error("invalid attribute fixture name");
    auto opened = parent->open_child(*name, access);
    (void)required(opened);
    return std::move(opened).take_value();
}
}

CK_TEST(native_attribute_queries_validate_limits_and_identifier_namespace) {
    const ck::testing::ScratchDirectory scratch("files-attributes-contract");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    CK_CHECK(opened->inspect_attributes({0, 1}).state() == ObservationState::Failed);
#if defined(_WIN32)
    const auto foreign = NativeFeatureIdentifier::from_posix("user.attribute");
#else
    const auto foreign = NativeFeatureIdentifier::from_windows_ea("ATTRIBUTE");
#endif
    CK_CHECK(opened->read_attribute(*foreign).state() == ObservationState::Failed);
}

#if defined(_WIN32)
namespace {
// Independent native fixture writer; no product mutation API exists yet.
void set_ea(const fs::path& path, const std::string& name, const std::string& bytes, unsigned char flags = 0) {
    using SetEa = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG);
    const auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetEaFile");
    if (!address) throw std::runtime_error("NtSetEaFile fixture API unavailable");
    const auto api = std::bit_cast<SetEa>(address);
    const auto handle = CreateFileW(path.c_str(), FILE_WRITE_EA | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open EA fixture writer");
    const auto size = 9 + name.size() + bytes.size();
    std::vector<std::uint32_t> storage((size + 3) / 4, 0);
    auto wire = std::as_writable_bytes(std::span(storage));
    wire[4] = static_cast<std::byte>(flags);
    wire[5] = static_cast<std::byte>(name.size());
    wire[6] = static_cast<std::byte>(bytes.size() & 0xffU);
    wire[7] = static_cast<std::byte>((bytes.size() >> 8U) & 0xffU);
    std::memcpy(wire.data() + 8, name.data(), name.size());
    std::memcpy(wire.data() + 9 + name.size(), bytes.data(), bytes.size());
    IO_STATUS_BLOCK io{};
    const auto status = api(handle, &io, wire.data(), static_cast<ULONG>(size));
    CloseHandle(handle);
    if (status != 0) throw std::runtime_error("EA fixture write failed NTSTATUS " + std::to_string(static_cast<std::uint32_t>(status)));
}
}

CK_TEST(native_windows_eas_preserve_values_flags_and_missing_value_semantics) {
    const ck::testing::ScratchDirectory scratch("files-attributes-windows");
    const auto path = scratch.path() / "file";
    write_file(path);
    const std::map<std::string, std::string> expected{{"CK.BINARY", std::string("a\0\xffz", 4)},
        {"CK.MEDIUM", std::string(8192, 'm')}, {"CK.NEEDED", "required"}};
    for (const auto& [name, value] : expected) set_ea(path, name, value, name == "CK.NEEDED" ? 0x80 : 0);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto result = opened->inspect_attributes();
    const auto& catalog = required(result);
    CK_CHECK(catalog.coverage == AttributeCoverage::WindowsExtendedAttributes);
    CK_CHECK(catalog.attributes.size() == expected.size());
    for (const auto& attribute : catalog.attributes) {
        const auto match = expected.find(*attribute.identifier.windows_ea_bytes());
        CK_CHECK(match != expected.end());
        if (match == expected.end()) continue;
        const bool needed = match->first == "CK.NEEDED";
        CK_CHECK(required(attribute.native_flags) == (needed ? 0x80U : 0U));
        CK_CHECK(attribute.role == (needed ? AttributeRole::InterpretationRequired : AttributeRole::Opaque));
        CK_CHECK(required(attribute.value_size) == match->second.size());
        const auto value = opened->read_attribute(attribute.identifier);
        NativeBytes bytes;
        for (const unsigned char byte : match->second) bytes.push_back(static_cast<std::byte>(byte));
        CK_CHECK(required(value).bytes == bytes);
        CK_CHECK(compare_identity(required(required(value).owner), required(catalog.owner)) == IdentityComparison::Same);
    }
    const auto binary = *NativeFeatureIdentifier::from_windows_ea("CK.BINARY");
    CK_CHECK(opened->read_attribute(*NativeFeatureIdentifier::from_windows_ea("CK.MISSING")).state() == ObservationState::Gone);
    set_ea(path, "CK.BINARY", "");
    CK_CHECK(opened->read_attribute(binary).state() == ObservationState::Gone);
    CK_CHECK(opened->read_attribute(*NativeFeatureIdentifier::from_windows_ea("CK.MEDIUM"), {32, 100}).state() == ObservationState::Unknown);
    CK_CHECK(opened->inspect_attributes({1024 * 1024, 1}).state() == ObservationState::Unknown);
}

CK_TEST(native_windows_ea_owner_survives_rename_replacement_unlink_and_directory_queries) {
    const ck::testing::ScratchDirectory scratch("files-attributes-windows-owner");
    const auto path = scratch.path() / "file";
    const auto retired = scratch.path() / "retired";
    write_file(path);
    set_ea(path, "CK.OWNER", "original");
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto before = opened->inspect_basic();
    fs::rename(path, retired);
    write_file(path);
    set_ea(path, "CK.OWNER", "replacement");
    fs::remove(retired);
    const auto value = opened->read_attribute(*NativeFeatureIdentifier::from_windows_ea("CK.OWNER"));
    CK_CHECK(required(value).bytes.size() == 8);
    CK_CHECK(compare_identity(required(required(value).owner), required(required(before).identity)) == IdentityComparison::Same);
    const auto directory = scratch.path() / "directory";
    fs::create_directory(directory);
    auto folder = open_entry(filesystem, directory);
    CK_CHECK(required(folder->inspect_attributes()).attributes.empty());
    set_ea(directory, "CK.DIRECTORY", "folder");
    CK_CHECK(required(folder->inspect_attributes()).attributes.size() == 1);
}

CK_TEST(native_windows_ea_reads_require_read_ea_access_and_do_not_follow_junction_targets) {
    const ck::testing::ScratchDirectory scratch("files-attributes-windows-access");
    const auto path = scratch.path() / "file";
    write_file(path);
    set_ea(path, "CK.SECRET", "secret");
    NativeFileSystem filesystem;
    auto metadata_only = open_entry(filesystem, path, EntryAccess::MetadataOnly);
    CK_CHECK(metadata_only->inspect_attributes().state() == ObservationState::Denied);
    CK_CHECK(metadata_only->read_attribute(*NativeFeatureIdentifier::from_windows_ea("CK.SECRET")).state() == ObservationState::Denied);
    const auto target = scratch.path() / "target";
    const auto link = scratch.path() / "junction";
    fs::create_directory(target);
    set_ea(target, "CK.SECRET", "target secret");
    fs::create_directory(link);
    ck::testing::make_junction(link, target);
    struct Remove { fs::path path; ~Remove() { RemoveDirectoryW(path.c_str()); } } remove{link};
    auto junction = open_entry(filesystem, link);
    const auto value = junction->read_attribute(*NativeFeatureIdentifier::from_windows_ea("CK.SECRET"));
    CK_CHECK(!value.value());
    CK_CHECK(value.state() == ObservationState::Gone || value.state() == ObservationState::Unsupported);
    const auto catalog = junction->inspect_attributes();
    CK_CHECK((catalog.value() && catalog.value()->attributes.empty()) || catalog.state() == ObservationState::Unsupported);
}
#endif

#if !defined(_WIN32)
namespace {
void set_attribute(const fs::path& path, const std::string& name, const std::string& bytes) {
#if defined(__APPLE__)
    const auto result = setxattr(path.c_str(), name.c_str(), bytes.data(), bytes.size(), 0, 0);
#else
    const auto result = setxattr(path.c_str(), name.c_str(), bytes.data(), bytes.size(), 0);
#endif
    if (result != 0) throw std::runtime_error("cannot set fixture xattr: " + std::to_string(errno));
}
std::string as_string(const NativeBytes& bytes) {
    std::string value;
    for (auto byte : bytes) value.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    return value;
}
}

CK_TEST(native_posix_attributes_preserve_binary_empty_values_and_per_name_sizes) {
    const ck::testing::ScratchDirectory scratch("files-attributes-values");
    const auto path = scratch.path() / "file";
    write_file(path);
    std::map<std::string, std::string> expected{
        {"user.ckfiles.binary", std::string("a\0\xffz", 4)},
        {"user.ckfiles.empty", ""}, {"user.ckfiles.medium", std::string(2048, 'm')}};
#if defined(__linux__)
    expected.emplace(std::string("user.ckfiles.raw-") + static_cast<char>(0xFF), "raw name");
#else
    expected.emplace("user.ckfiles.\xC3\xA4", "unicode name");
#endif
    for (const auto& [name, value] : expected) set_attribute(path, name, value);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto result = opened->inspect_attributes();
    const auto& catalog = required(result);
    CK_CHECK(catalog.coverage == AttributeCoverage::CredentialsVisiblePosix);
    std::size_t found = 0;
    for (const auto& attribute : catalog.attributes) {
        const auto match = expected.find(*attribute.identifier.posix_bytes());
        if (match == expected.end()) continue;
        ++found;
        CK_CHECK(required(attribute.value_size) == match->second.size());
        const auto value = opened->read_attribute(attribute.identifier);
        CK_CHECK(as_string(required(value).bytes) == match->second);
        CK_CHECK(compare_identity(required(required(value).owner), required(catalog.owner)) == IdentityComparison::Same);
        CK_CHECK(attribute.native_flags.state() == ObservationState::Unsupported);
    }
    CK_CHECK(found == expected.size());
    CK_CHECK(opened->read_attribute(*NativeFeatureIdentifier::from_posix("user.ckfiles.missing")).state() == ObservationState::Gone);
    const auto limited = opened->read_attribute(*NativeFeatureIdentifier::from_posix("user.ckfiles.medium"), {32, 100});
    CK_CHECK(limited.state() == ObservationState::Unknown && !limited.value());
    CK_CHECK(opened->inspect_attributes({1, 100}).state() == ObservationState::Unknown);
    CK_CHECK(opened->inspect_attributes({1024 * 1024, 1}).state() == ObservationState::Unknown);
}

CK_TEST(native_posix_attribute_owner_survives_rename_replacement_unlink_and_live_updates) {
    const ck::testing::ScratchDirectory scratch("files-attributes-owner");
    const auto path = scratch.path() / "file";
    const auto retired = scratch.path() / "retired";
    write_file(path);
    const auto id = *NativeFeatureIdentifier::from_posix("user.ckfiles.owner");
    set_attribute(path, *id.posix_bytes(), "before");
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto original = opened->inspect_basic();
    fs::rename(path, retired);
    write_file(path);
    set_attribute(path, *id.posix_bytes(), "replacement");
    set_attribute(retired, *id.posix_bytes(), "changed");
    CK_CHECK(as_string(required(opened->read_attribute(id)).bytes) == "changed");
    fs::remove(retired);
    const auto value = opened->read_attribute(id);
    CK_CHECK(as_string(required(value).bytes) == "changed");
    CK_CHECK(compare_identity(required(required(value).owner), required(required(original).identity)) == IdentityComparison::Same);
    CK_CHECK(required(opened->inspect_attributes()).coverage == AttributeCoverage::CredentialsVisiblePosix);
}

CK_TEST(native_posix_directory_and_symlink_attributes_do_not_read_target_attributes) {
    const ck::testing::ScratchDirectory scratch("files-attributes-links");
    const auto directory = scratch.path() / "directory";
    fs::create_directory(directory);
    const auto id = *NativeFeatureIdentifier::from_posix("user.ckfiles.secret");
    set_attribute(directory, *id.posix_bytes(), "target attribute");
    NativeFileSystem filesystem;
    auto opened_directory = open_entry(filesystem, directory);
    CK_CHECK(as_string(required(opened_directory->read_attribute(id)).bytes) == "target attribute");
    fs::create_symlink("directory", scratch.path() / "link");
    auto link = open_entry(filesystem, scratch.path() / "link");
    const auto value = link->read_attribute(id);
    CK_CHECK(value.state() == ObservationState::Gone || value.state() == ObservationState::Unsupported ||
        value.state() == ObservationState::Denied);
    CK_CHECK(!value.value());
    const auto catalog = link->inspect_attributes();
    if (catalog.value()) {
        for (const auto& attribute : catalog.value()->attributes) CK_CHECK(attribute.identifier != id);
    } else {
        CK_CHECK(catalog.state() == ObservationState::Unsupported || catalog.state() == ObservationState::Denied);
    }
}
#if defined(__linux__)
CK_TEST(native_linux_attribute_denial_is_reported_without_changing_permissions) {
    // This conformance fixture requires an ordinary account, as do the native
    // permission checks elsewhere in this suite. Never change credentials.
    CK_CHECK(geteuid() != 0);
    if (geteuid() == 0) return;
    const ck::testing::ScratchDirectory scratch("files-attributes-linux-denied");
    const auto path = scratch.path() / "file";
    write_file(path);
    set_attribute(path, "user.ckfiles.secret", "private");
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    struct Restore { fs::path path; ~Restore() { chmod(path.c_str(), 0600); } } restore{path};
    if (chmod(path.c_str(), 0000) != 0) throw std::runtime_error("cannot set attribute fixture mode");
    const auto denied = opened->read_attribute(*NativeFeatureIdentifier::from_posix("user.ckfiles.secret"));
    CK_CHECK(denied.state() == ObservationState::Denied && !denied.value());
    CK_CHECK(denied.unavailable_detail()->error.code == EACCES);
    CK_CHECK(required(required(opened->inspect_basic()).posix_permissions) == 0);
}
#endif
#endif
