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
#include <winioctl.h>
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
        std::cerr << "storage observation: " << static_cast<int>(e.state) << " " << e.error.operation << " " << e.error.code << std::endl;
        throw std::runtime_error("required storage observation unavailable");
    }
    return *result.value();
}
void write_file(const fs::path& path) {
    std::ofstream file(path, std::ios::binary);
    file << "fixture";
    file.close();
    if (!file) throw std::runtime_error("cannot write storage fixture");
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
    if (!name) throw std::runtime_error("invalid storage fixture name");
    auto opened = parent->open_child(*name, access);
    (void)required(opened);
    return std::move(opened).take_value();
}
}

CK_TEST(native_storage_observations_keep_the_opened_owner_and_never_infer_sparse_from_size) {
    const ck::testing::ScratchDirectory scratch("files-storage-owner");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto before = opened->inspect_basic();
    fs::rename(path, scratch.path() / "retired");
    write_file(path);
    fs::remove(scratch.path() / "retired");
    const auto result = opened->inspect_storage();
    CK_CHECK(compare_identity(required(required(result).owner), required(required(before).identity)) == IdentityComparison::Same);
    CK_CHECK(required(required(result).logical_size) == required(required(before).logical_size));
#if defined(_WIN32)
    CK_CHECK(required(result).model == StorageModel::WindowsHandleAttributes);
    CK_CHECK(required(required(result).flags).value == required(required(before).windows_attributes));
#elif defined(__APPLE__)
    CK_CHECK(required(result).model == StorageModel::DarwinStatFlags);
    CK_CHECK(required(required(result).flags).value == required(required(before).bsd_flags));
    CK_CHECK(reported_storage_flag(required(result), StorageFeature::SparseFlag).state() == ObservationState::Unsupported);
#else
    CK_CHECK(required(result).model == StorageModel::LinuxStatx);
    CK_CHECK(required(required(result).flags).known_mask == required(required(before).linux_attributes).known_mask);
    CK_CHECK(data_access_hint(required(result)).state() == ObservationState::Unsupported);
#endif
}

CK_TEST(native_storage_flag_bindings_match_public_sdk_bits) {
    BasicMetadata metadata;
#if defined(_WIN32)
    constexpr auto model = StorageModel::WindowsHandleAttributes;
    const std::pair<StorageFeature, std::uint64_t> mapping[] = {
        {StorageFeature::CompressionFlag, FILE_ATTRIBUTE_COMPRESSED}, {StorageFeature::EncryptionFlag, FILE_ATTRIBUTE_ENCRYPTED},
        {StorageFeature::SparseFlag, FILE_ATTRIBUTE_SPARSE_FILE}, {StorageFeature::OfflineFlag, FILE_ATTRIBUTE_OFFLINE},
        {StorageFeature::RecallOnDataAccessFlag, FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS},
        {StorageFeature::PinnedIntentFlag, FILE_ATTRIBUTE_PINNED}, {StorageFeature::UnpinnedIntentFlag, FILE_ATTRIBUTE_UNPINNED},
        {StorageFeature::IntegrityStreamFlag, FILE_ATTRIBUTE_INTEGRITY_STREAM}, {StorageFeature::NoScrubFlag, FILE_ATTRIBUTE_NO_SCRUB_DATA},
        {StorageFeature::ReparseFlag, FILE_ATTRIBUTE_REPARSE_POINT}, {StorageFeature::HiddenFlag, FILE_ATTRIBUTE_HIDDEN}};
    metadata.windows_attributes = Observation<std::uint32_t>::available(0);
#elif defined(__APPLE__)
    constexpr auto model = StorageModel::DarwinStatFlags;
    const std::pair<StorageFeature, std::uint64_t> mapping[] = {
        {StorageFeature::CompressionFlag, UF_COMPRESSED}, {StorageFeature::ImmutableFlag, UF_IMMUTABLE},
        {StorageFeature::ImmutableFlag, SF_IMMUTABLE}, {StorageFeature::AppendOnlyFlag, UF_APPEND},
        {StorageFeature::AppendOnlyFlag, SF_APPEND}, {StorageFeature::DatalessFlag, SF_DATALESS}, {StorageFeature::HiddenFlag, UF_HIDDEN}};
    metadata.bsd_flags = Observation<std::uint32_t>::available(0);
#else
    constexpr auto model = StorageModel::LinuxStatx;
    const std::pair<StorageFeature, std::uint64_t> mapping[] = {
        {StorageFeature::CompressionFlag, STATX_ATTR_COMPRESSED}, {StorageFeature::EncryptionFlag, STATX_ATTR_ENCRYPTED},
        {StorageFeature::ImmutableFlag, STATX_ATTR_IMMUTABLE}, {StorageFeature::AppendOnlyFlag, STATX_ATTR_APPEND},
        {StorageFeature::VerityFlag, STATX_ATTR_VERITY}, {StorageFeature::DaxFlag, STATX_ATTR_DAX}};
    metadata.linux_attributes = Observation<LinuxFileAttributes>::available({0, ~std::uint64_t{0}});
#endif
    for (const auto& [feature, bit] : mapping) {
        auto info = required(storage_from_basic(metadata, model));
        info.flags = Observation<NativeStorageFlags>::available({bit, bit});
        CK_CHECK(required(reported_storage_flag(info, feature)) == ReportedFlag::Set);
    }
}

CK_TEST(native_storage_of_final_links_keeps_link_identity_and_does_not_inspect_target_compression) {
    const ck::testing::ScratchDirectory scratch("files-storage-links");
    const auto target = scratch.path() / "target";
    const auto link = scratch.path() / "link";
#if defined(_WIN32)
    fs::create_directory(target);
    fs::create_directory(link);
    ck::testing::make_junction(link, target);
    struct Remove { fs::path path; ~Remove() { RemoveDirectoryW(path.c_str()); } } remove{link};
    constexpr auto kind = ObjectKind::Junction;
#else
    write_file(target);
    fs::create_symlink("target", link);
    constexpr auto kind = ObjectKind::SymbolicLink;
#endif
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, link);
    const auto storage = opened->inspect_storage();
    CK_CHECK(required(required(storage).kind) == kind);
    CK_CHECK(compare_identity(required(required(storage).owner), required(required(filesystem.inspect_basic(target)).identity)) == IdentityComparison::Different);
    CK_CHECK(required(storage).logical_size.state() == ObservationState::Unsupported);
    CK_CHECK(required(storage).windows_compression.state() == ObservationState::Unsupported);
}

#if defined(_WIN32)
CK_TEST(native_windows_sparse_storage_is_observed_from_its_flag_not_inferred_from_allocation) {
    const ck::testing::ScratchDirectory scratch("files-storage-sparse");
    const auto path = scratch.path() / "file";
    write_file(path);
    const auto writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (writer == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open sparse fixture writer");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close_writer{writer};
    DWORD returned = 0;
    LARGE_INTEGER end{}; end.QuadPart = 16 * 1024 * 1024;
    if (!DeviceIoControl(writer, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) ||
        !SetFilePointerEx(writer, end, nullptr, FILE_BEGIN) || !SetEndOfFile(writer))
        throw std::runtime_error("cannot prepare native sparse fixture: " + std::to_string(GetLastError()));
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto storage = opened->inspect_storage();
    CK_CHECK(required(reported_storage_flag(required(storage), StorageFeature::SparseFlag)) == ReportedFlag::Set);
    CK_CHECK(required(required(storage).logical_size) == static_cast<std::uint64_t>(end.QuadPart));
    CK_CHECK(required(required(storage).allocated_size) < required(required(storage).logical_size));
}

CK_TEST(native_windows_compression_details_match_an_independent_attribute_only_handle) {
    const ck::testing::ScratchDirectory scratch("files-storage-compression");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto storage = opened->inspect_storage();
    const auto reference = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (reference == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open compression reference");
    FILE_COMPRESSION_INFO native{};
    const auto success = GetFileInformationByHandleEx(reference, FileCompressionInfo, &native, sizeof(native));
    const auto error = GetLastError();
    CloseHandle(reference);
    if (success) {
        const auto& value = required(required(storage).windows_compression);
        CK_CHECK(value.compressed_file_size == static_cast<std::uint64_t>(native.CompressedFileSize.QuadPart));
        CK_CHECK(value.format == native.CompressionFormat && value.compression_unit_shift == native.CompressionUnitShift);
        CK_CHECK(value.chunk_shift == native.ChunkShift && value.cluster_shift == native.ClusterShift);
    } else {
        CK_CHECK(!required(storage).windows_compression.value());
        CK_CHECK(required(storage).windows_compression.unavailable_detail()->error.code == error);
    }
}

CK_TEST(native_windows_reported_offline_hint_requires_provider_review_before_attribute_or_stream_reads) {
    const ck::testing::ScratchDirectory scratch("files-storage-offline-hint");
    const auto path = scratch.path() / "file";
    write_file(path);
    const auto original = GetFileAttributesW(path.c_str());
    if (original == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("cannot inspect offline fixture");
    struct Restore { fs::path path; DWORD flags; ~Restore() { SetFileAttributesW(path.c_str(), flags); } } restore{path, original};
    // This is an owned synthetic flag fixture, not a real remote provider.
    if (!SetFileAttributesW(path.c_str(), original | FILE_ATTRIBUTE_OFFLINE))
        throw std::runtime_error("cannot set offline-hint fixture: " + std::to_string(GetLastError()));
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto storage = opened->inspect_storage();
    CK_CHECK(required(data_access_hint(required(storage))) == DataAccessHint::RecallFlagReported);
    CK_CHECK(required(storage).windows_compression.state() == ObservationState::Unknown);
    const auto catalog = opened->inspect_attributes();
    const auto attribute = opened->read_attribute(*NativeFeatureIdentifier::from_windows_ea("CK.MISSING"));
    const auto streams = opened->inspect_data_channels();
    for (const auto* e : {catalog.unavailable_detail(), attribute.unavailable_detail(), streams.unavailable_detail()}) {
        CK_CHECK(e && e->state == ObservationState::Unknown);
        if (e) CK_CHECK(e->error.code == static_cast<std::int64_t>(AdapterError::ProviderReviewRequired));
    }
    if (!SetFileAttributesW(path.c_str(), original)) throw std::runtime_error("cannot restore offline-hint fixture");
}
#elif defined(__APPLE__)
CK_TEST(native_darwin_storage_observes_hidden_flag_live_without_changing_the_opened_owner) {
    const ck::testing::ScratchDirectory scratch("files-storage-darwin");
    const auto path = scratch.path() / "file";
    write_file(path);
    NativeFileSystem filesystem;
    auto opened = open_entry(filesystem, path);
    const auto original = opened->inspect_basic();
    const auto flags = required(required(original).bsd_flags);
    struct Restore { fs::path path; std::uint32_t flags; ~Restore() { chflags(path.c_str(), flags); } } restore{path, flags};
    if (chflags(path.c_str(), flags | UF_HIDDEN) != 0) throw std::runtime_error("cannot set hidden storage fixture");
    const auto storage = opened->inspect_storage();
    CK_CHECK(required(reported_storage_flag(required(storage), StorageFeature::HiddenFlag)) == ReportedFlag::Set);
    CK_CHECK(compare_identity(required(required(storage).owner), required(required(original).identity)) == IdentityComparison::Same);
    CK_CHECK(required(data_access_hint(required(storage))) == DataAccessHint::NoRecallFlagReported);
}
#endif

