// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "windows_name.hpp"
#include "windows_directory_record.hpp"
#include "windows_stream_records.hpp"
#include "windows_parent_record.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <winioctl.h>

#include <bit>
#include <set>
#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <cstddef>
#include <cstring>
#include <optional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace ck::files {
namespace {

struct HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

ObservationState error_state(DWORD code) noexcept {
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return ObservationState::Gone;
    if (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD ||
        code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) return ObservationState::Denied;
    if (code == ERROR_FILE_INVALID || code == ERROR_DELETE_PENDING) return ObservationState::Changed;
    if (code == ERROR_INVALID_FUNCTION || code == ERROR_NOT_SUPPORTED ||
        code == ERROR_CALL_NOT_IMPLEMENTED || code == ERROR_INVALID_LEVEL || code == ERROR_INVALID_PARAMETER)
        return ObservationState::Unsupported;
    return ObservationState::Failed;
}

template <class T>
Observation<T> failure(DWORD code, const char* operation) {
    return Observation<T>::unavailable(error_state(code), {ErrorDomain::Win32, code, operation});
}

template <class T>
Observation<T> not_applicable(const char* operation) {
    return Observation<T>::unavailable(ObservationState::Unsupported,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), operation});
}

Observation<ObjectIdentity> identity(HANDLE handle, const AdapterScope& scope) {
    FILE_ID_INFO native{};
    if (GetFileInformationByHandleEx(handle, FileIdInfo, &native, sizeof(native))) {
        NativeIdentifier object;
        static_assert(sizeof(native.FileId.Identifier) == sizeof(object.bytes));
        std::memcpy(object.bytes.data(), native.FileId.Identifier, sizeof(native.FileId.Identifier));
        if (object == NativeIdentifier{})
            return Observation<ObjectIdentity>::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "FileIdInfo.zero"});
        return Observation<ObjectIdentity>::available({scope, IdentityScheme::WindowsFileId128,
            NativeIdentifier::from_u64(native.VolumeSerialNumber), object, std::nullopt});
    }
    const DWORD code = GetLastError();
    if (error_state(code) != ObservationState::Unsupported) return failure<ObjectIdentity>(code, "FileIdInfo");

    // Filesystems without the 128-bit query may expose the documented classic
    // index. Keep its scheme explicit, never truncate a successful 128-bit ID,
    // and never use a query error/zero index as evidence of object identity.
    BY_HANDLE_FILE_INFORMATION legacy{};
    if (!GetFileInformationByHandle(handle, &legacy)) return failure<ObjectIdentity>(GetLastError(), "GetFileInformationByHandle");
    const auto index = (static_cast<std::uint64_t>(legacy.nFileIndexHigh) << 32U) | legacy.nFileIndexLow;
    if (index == 0)
        return Observation<ObjectIdentity>::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "legacy_file_index.zero"});
    return Observation<ObjectIdentity>::available({scope, IdentityScheme::WindowsFileIndex64,
        NativeIdentifier::from_u64(legacy.dwVolumeSerialNumber), NativeIdentifier::from_u64(index), std::nullopt});
}

Observation<FileTimestamp> timestamp(LARGE_INTEGER native) {
    return Observation<FileTimestamp>::available(FileTimestamp::from_windows_ticks(static_cast<std::uint64_t>(native.QuadPart)));
}

Observation<std::uint64_t> nonnegative(LONGLONG value, const char* operation) {
    if (value < 0)
        return Observation<std::uint64_t>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), operation});
    return Observation<std::uint64_t>::available(static_cast<std::uint64_t>(value));
}

Observation<BasicMetadata> handle_metadata(HANDLE handle, const AdapterScope& scope) {
    FILE_BASIC_INFO native{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &native, sizeof(native)))
        return failure<BasicMetadata>(GetLastError(), "FileBasicInfo");
    BasicMetadata result;
    result.identity = identity(handle, scope);
    result.windows_attributes = Observation<std::uint32_t>::available(native.FileAttributes);
    result.accessed = timestamp(native.LastAccessTime);
    result.modified = timestamp(native.LastWriteTime);
    result.changed = timestamp(native.ChangeTime);
    result.created = timestamp(native.CreationTime);
    result.kind = Observation<ObjectKind>::available((native.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? ObjectKind::Directory : ObjectKind::RegularFile);
    if ((native.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        result.kind = Observation<ObjectKind>::available(ObjectKind::ReparsePoint);
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
            result.reparse_tag = Observation<std::uint32_t>::available(tag.ReparseTag);
            if (tag.ReparseTag == IO_REPARSE_TAG_SYMLINK) result.kind = Observation<ObjectKind>::available(ObjectKind::SymbolicLink);
            if (tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) result.kind = Observation<ObjectKind>::available(ObjectKind::Junction);
        } else {
            result.reparse_tag = failure<std::uint32_t>(GetLastError(), "FileAttributeTagInfo");
        }
    } else {
        result.reparse_tag = not_applicable<std::uint32_t>("reparse_tag");
    }
    if (GetFileType(handle) == FILE_TYPE_CHAR) result.kind = Observation<ObjectKind>::available(ObjectKind::CharacterDevice);
    result.posix_uid = not_applicable<std::uint64_t>("posix_uid");
    result.posix_gid = not_applicable<std::uint64_t>("posix_gid");
    result.posix_permissions = not_applicable<std::uint32_t>("posix_permissions");
    result.bsd_flags = not_applicable<std::uint32_t>("bsd_flags");
    result.linux_attributes = not_applicable<LinuxFileAttributes>("linux_attributes");

    FILE_STANDARD_INFO standard{};
    if (GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))) {
        result.link_count = Observation<std::uint64_t>::available(standard.NumberOfLinks);
        if (*result.kind.value() == ObjectKind::RegularFile) {
            result.logical_size = nonnegative(standard.EndOfFile.QuadPart, "FileStandardInfo.size");
            result.allocated_size = nonnegative(standard.AllocationSize.QuadPart, "FileStandardInfo.allocation");
        } else {
            result.logical_size = not_applicable<std::uint64_t>("regular_file.size");
            result.allocated_size = not_applicable<std::uint64_t>("regular_file.allocation");
        }
    } else {
        const DWORD code = GetLastError();
        result.link_count = failure<std::uint64_t>(code, "FileStandardInfo");
        result.logical_size = failure<std::uint64_t>(code, "FileStandardInfo");
        result.allocated_size = failure<std::uint64_t>(code, "FileStandardInfo");
    }
    return Observation<BasicMetadata>::available(std::move(result));
}

using OwnedHandle = std::unique_ptr<void, HandleCloser>;
// Public native-system-service ABI: ZwQueryEaFile contract, Nt user-mode
// entry point. No private information class or hard-coded syscall number.
using QueryEa = NTSTATUS (NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, BOOLEAN, PVOID, ULONG, PULONG, BOOLEAN);
Observation<QueryEa> query_ea_api() {
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto address = module ? GetProcAddress(module, "NtQueryEaFile") : nullptr;
    if (!address) {
        const DWORD code = GetLastError();
        return Observation<QueryEa>::unavailable(ObservationState::Unsupported, {ErrorDomain::Win32, code, "NtQueryEaFile.resolve"});
    }
    static_assert(sizeof(QueryEa) == sizeof(address));
    return Observation<QueryEa>::available(std::bit_cast<QueryEa>(address));
}
constexpr std::uint32_t no_eas = 0xC0000052U;
constexpr std::uint32_t no_more_eas = 0x80000012U;
constexpr std::uint32_t nonexistent_ea = 0xC0000051U;
constexpr std::uint32_t buffer_overflow = 0x80000005U;
constexpr std::uint32_t buffer_too_small = 0xC0000023U;

template <class T>
Observation<T> ea_failure(std::uint32_t code, const char* operation) {
    auto state = error_state(RtlNtStatusToDosError(static_cast<NTSTATUS>(code)));
    if (code == no_eas || code == no_more_eas || code == nonexistent_ea) state = ObservationState::Gone;
    if (code == 0xC000004FU || code == 0xC0000010U) state = ObservationState::Unsupported;
    return Observation<T>::unavailable(state, {ErrorDomain::NtStatus, code, operation});
}

class WindowsOpenedEntry final : public OpenedEntry {
public:
    WindowsOpenedEntry(OwnedHandle handle, AdapterScope scope, bool layout_access = false,
                       bool content_access = false, bool content_write_access = false)
        : handle_(std::move(handle)), scope_(std::move(scope)), layout_access_(layout_access),
          content_access_(content_access), content_write_access_(content_write_access) {}
    Observation<ContentRead> read_content(ContentReadLimits limits = {}, std::uint64_t offset = 0,
                                          std::stop_token stop = {}) const override {
        using Result = Observation<ContentRead>;
        if (!limits.valid())
            return Result::unavailable(ObservationState::Failed,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "content.limits"});
        if (stop.stop_requested())
            return Result::unavailable(ObservationState::Unknown,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "content.cancelled"});
        if (!content_access_)
            return Result::unavailable(ObservationState::Denied,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "content.access_not_requested"});
        const auto before = inspect_basic();
        if (!before.value()) { const auto &e = *before.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!before.value()->kind.value() || *before.value()->kind.value() != ObjectKind::RegularFile)
            return Result::unavailable(ObservationState::Unsupported,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "content.object_kind"});
        if (!before.value()->logical_size.value()) { const auto &e = *before.value()->logical_size.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto size = *before.value()->logical_size.value();
        if (offset > size || offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
            return Result::unavailable(ObservationState::Changed,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "content.offset"});
        const auto count = static_cast<DWORD>(std::min<std::uint64_t>(limits.max_bytes, size - offset));
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN))
            return failure<ContentRead>(GetLastError(), "content.seek");
        std::vector<std::byte> bytes(count);
        DWORD copied = 0;
        if (count && !ReadFile(handle_.get(), bytes.data(), count, &copied, nullptr))
            return failure<ContentRead>(GetLastError(), "content.ReadFile");
        bytes.resize(copied);
        if (stop.stop_requested())
            return Result::unavailable(ObservationState::Unknown,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "content.cancelled"});
        const auto after = inspect_basic();
        if (!after.value()) { const auto &e = *after.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!after.value()->identity.value() || !before.value()->identity.value() ||
            compare_identity(*before.value()->identity.value(), *after.value()->identity.value()) != IdentityComparison::Same ||
            !after.value()->logical_size.value() || *after.value()->logical_size.value() != size)
            return Result::unavailable(ObservationState::Changed,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "content.identity"});
        return Result::available({std::move(bytes), offset + copied >= size});
    }
    Observation<std::size_t> write_content(std::uint64_t offset, std::span<const std::byte> bytes,
                                            ContentWriteLimits limits = {}, std::stop_token stop = {}) override {
        using Result = Observation<std::size_t>;
        if (!limits.valid() || bytes.size() > limits.max_bytes)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "content.write_limits"});
        if (stop.stop_requested())
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "content.write_cancelled"});
        if (!content_write_access_)
            return Result::unavailable(ObservationState::Denied,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "content.write_access_not_requested"});
        const auto before = inspect_basic();
        if (!before.value()) { const auto& e = *before.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!before.value()->kind.value() || *before.value()->kind.value() != ObjectKind::RegularFile)
            return Result::unavailable(ObservationState::Unsupported,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "content.write_object_kind"});
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max());
        if (offset > maximum || bytes.size() > maximum - offset)
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "content.write_offset"});
        LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN))
            return failure<std::size_t>(GetLastError(), "content.write_seek");
        DWORD written = 0;
        if (!WriteFile(handle_.get(), bytes.data(), static_cast<DWORD>(std::min(bytes.size(), static_cast<std::size_t>(MAXDWORD))), &written, nullptr))
            return failure<std::size_t>(GetLastError(), "content.WriteFile");
        if (stop.stop_requested())
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "content.write_cancelled"});
        const auto after = inspect_basic();
        if (!after.value()) { const auto& e = *after.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!after.value()->identity.value() || !before.value()->identity.value() ||
            compare_identity(*before.value()->identity.value(), *after.value()->identity.value()) != IdentityComparison::Same)
            return Result::unavailable(ObservationState::Changed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "content.write_identity"});
        return Result::available(static_cast<std::size_t>(written));
    }
    Observation<bool> flush_content() override {
        if (!content_write_access_)
            return Observation<bool>::unavailable(ObservationState::Denied,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "content.flush_access"});
        if (!FlushFileBuffers(handle_.get())) return failure<bool>(GetLastError(), "content.FlushFileBuffers");
        return Observation<bool>::available(true);
    }
    Observation<bool> publish_noreplace(const DirectoryCursor& destination, const NativeName& name) override;
    Observation<BasicMetadata> inspect_basic() const override { return handle_metadata(handle_.get(), scope_); }
    Observation<FileIntegrityInfo> inspect_integrity(InventoryLimits limits = {}) const override {
        using Result = Observation<FileIntegrityInfo>;
        if (!limits.valid()) return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "integrity.limits"});
        if (limits.max_native_bytes < WindowsIntegrityData::wire_bytes) return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "integrity.buffer_limit"});
        static_assert(sizeof(FSCTL_GET_INTEGRITY_INFORMATION_BUFFER) == WindowsIntegrityData::wire_bytes);
        static_assert(offsetof(FSCTL_GET_INTEGRITY_INFORMATION_BUFFER, ChecksumAlgorithm) == 0 && offsetof(FSCTL_GET_INTEGRITY_INFORMATION_BUFFER, Reserved) == 2);
        static_assert(offsetof(FSCTL_GET_INTEGRITY_INFORMATION_BUFFER, Flags) == 4 && offsetof(FSCTL_GET_INTEGRITY_INFORMATION_BUFFER, ChecksumChunkSizeInBytes) == 8 && offsetof(FSCTL_GET_INTEGRITY_INFORMATION_BUFFER, ClusterSizeInBytes) == 12);
        static_assert(CHECKSUM_TYPE_NONE == 0 && CHECKSUM_TYPE_CRC64 == 2 && FSCTL_INTEGRITY_FLAG_CHECKSUM_ENFORCEMENT_OFF == 1);
        static_assert(FILE_ATTRIBUTE_INTEGRITY_STREAM == 0x8000 && FILE_ATTRIBUTE_REPARSE_POINT == 0x400);
        static_assert(((FSCTL_GET_INTEGRITY_INFORMATION >> 14) & 3U) == FILE_ANY_ACCESS);
        const auto before = inspect_basic();
        const auto valid = detail::qualify_integrity_subject(before, before);
        if (!valid.value()) { const auto& e = *valid.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        FSCTL_GET_INTEGRITY_INFORMATION_BUFFER native{}; DWORD returned = 0;
        if (!DeviceIoControl(handle_.get(), FSCTL_GET_INTEGRITY_INFORMATION, nullptr, 0, &native, sizeof(native), &returned, nullptr)) {
            const DWORD code = GetLastError();
            return Result::unavailable(code == ERROR_INVALID_PARAMETER || code == ERROR_MORE_DATA ? ObservationState::Unknown : error_state(code),
                {ErrorDomain::Win32, code, "FSCTL_GET_INTEGRITY_INFORMATION"});
        }
        if (returned != sizeof(native)) return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "integrity.returned_count"});
        auto decoded = WindowsIntegrityData::decode(std::as_bytes(std::span(&native, 1)));
        if (!decoded.value()) { const auto& e = *decoded.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto after = inspect_basic(); const auto owner = detail::qualify_integrity_subject(before, after);
        if (!owner.value()) { const auto& e = *owner.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available({IntegrityQueryFamily::WindowsFsctl, Observation<ObjectIdentity>::available(*owner.value()), *before.value()->kind.value(),
            *before.value()->windows_attributes.value(), *after.value()->windows_attributes.value(), std::move(decoded).take_value()});
    }
    Observation<FileDataLayout> inspect_data_layout(InventoryLimits limits = {}, std::stop_token stop = {}) const override {
        using Result = Observation<FileDataLayout>;
        if (!limits.valid()) return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "layout.limits"});
        if (stop.stop_requested()) return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "layout.cancelled"});
        if (!layout_access_) return Result::unavailable(ObservationState::Denied,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "layout.access_not_requested"});
        const auto before = inspect_basic();
        if (!before.value()) { const auto& e = *before.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto guard = detail::data_layout_access_guard(*before.value(), StorageModel::WindowsHandleAttributes);
        if (!guard.value()) { const auto& e = *guard.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!before.value()->logical_size.value()) { const auto& e = *before.value()->logical_size.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto size = *before.value()->logical_size.value();
        if (size > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max())) return Result::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "layout.offset_range"});
        const auto query = [&](std::uint64_t at, std::uint64_t end) -> Observation<std::optional<FileDataRange>> {
            using Reply = Observation<std::optional<FileDataRange>>;
            FILE_ALLOCATED_RANGE_BUFFER input{}, output{};
            input.FileOffset.QuadPart = static_cast<LONGLONG>(at); input.Length.QuadPart = static_cast<LONGLONG>(end - at);
            DWORD bytes = 0;
            const auto success = DeviceIoControl(handle_.get(), FSCTL_QUERY_ALLOCATED_RANGES, &input, sizeof(input), &output, sizeof(output), &bytes, nullptr);
            const DWORD code = success ? ERROR_SUCCESS : GetLastError();
            if (!success && code != ERROR_MORE_DATA) return failure<std::optional<FileDataRange>>(code, "FSCTL_QUERY_ALLOCATED_RANGES");
            if (bytes == 0 && success) return Reply::available(std::nullopt);
            if (bytes != sizeof(output) || output.FileOffset.QuadPart < 0 || output.Length.QuadPart <= 0)
                return Reply::unavailable(ObservationState::Failed,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "layout.windows_range"});
            return Reply::available(FileDataRange{static_cast<std::uint64_t>(output.FileOffset.QuadPart), static_cast<std::uint64_t>(output.Length.QuadPart)});
        };
        auto result = detail::collect_data_layout(query, DataLayoutFamily::WindowsAllocatedRanges, before.value()->identity, size, limits, stop);
        if (!result.value()) return result;
        if (stop.stop_requested()) return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "layout.cancelled"});
        const auto after = inspect_basic();
        if (stop.stop_requested()) return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "layout.cancelled"});
        if (!after.value()) { const auto& e = *after.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto unchanged = detail::data_layout_metadata_unchanged(*before.value(), *after.value());
        if (!unchanged.value()) { const auto& e = *unchanged.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return result;
    }
    Observation<std::unique_ptr<OpenedEntry>> open_parent_directory() const override;
    Observation<LinkMetadata> read_link_metadata(InventoryLimits limits = {}) const override {
        using Result = Observation<LinkMetadata>;
        if (!limits.valid()) return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "link.limits"});
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (!metadata.value()->windows_attributes.value()) { const auto& e = *metadata.value()->windows_attributes.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if ((*metadata.value()->windows_attributes.value() & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
            return Result::unavailable(ObservationState::Unsupported,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "link.object_kind"});
        static_assert(MAXIMUM_REPARSE_DATA_BUFFER_SIZE == detail::windows_reparse_max_bytes);
        const auto maximum = std::min(limits.max_native_bytes, detail::windows_reparse_max_bytes);
        if (maximum < 8) return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "reparse.buffer_limit"});
        auto capacity = std::min<std::size_t>(1024, maximum);
        for (;;) {
            std::vector<std::uint64_t> buffer((capacity + 7) / 8, 0);
            DWORD returned = 0;
            if (DeviceIoControl(handle_.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0, buffer.data(), static_cast<DWORD>(capacity), &returned, nullptr)) {
                if (returned > capacity) return Result::unavailable(ObservationState::Failed,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "reparse.returned_count"});
                auto decoded = detail::decode_windows_reparse(std::as_bytes(std::span(buffer)).first(returned), limits);
                if (!decoded.value()) { const auto& e = *decoded.unavailable_detail(); return Result::unavailable(e.state, e.error); }
                auto data = std::move(decoded).take_value();
                if (metadata.value()->reparse_tag.value() && *metadata.value()->reparse_tag.value() != data.tag)
                    return Result::unavailable(ObservationState::Changed,
                        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "reparse.tag_changed"});
                return Result::available({metadata.value()->identity, std::move(data)});
            }
            const DWORD code = GetLastError();
            if (code != ERROR_INSUFFICIENT_BUFFER && code != ERROR_MORE_DATA) return failure<LinkMetadata>(code, "FSCTL_GET_REPARSE_POINT");
            if (capacity == maximum) return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Win32, code, "FSCTL_GET_REPARSE_POINT.buffer_limit"});
            capacity = capacity > maximum / 2 ? maximum : capacity * 2;
        }
    }
    Observation<StorageInfo> inspect_storage() const override {
        using Result = Observation<StorageInfo>;
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        auto decoded = storage_from_basic(*metadata.value(), StorageModel::WindowsHandleAttributes);
        if (!decoded.value()) return decoded;
        auto result = std::move(decoded).take_value();
        if (const auto guarded = provider_guard(*metadata.value()))
            result.windows_compression = Observation<WindowsCompressionDetails>::unavailable(guarded->state, guarded->error);
        else if (metadata.value()->kind.value() && *metadata.value()->kind.value() == ObjectKind::RegularFile) {
            FILE_COMPRESSION_INFO native{};
            if (GetFileInformationByHandleEx(handle_.get(), FileCompressionInfo, &native, sizeof(native))) {
                if (native.CompressedFileSize.QuadPart >= 0)
                    result.windows_compression = Observation<WindowsCompressionDetails>::available({
                        static_cast<std::uint64_t>(native.CompressedFileSize.QuadPart), native.CompressionFormat,
                        native.CompressionUnitShift, native.ChunkShift, native.ClusterShift});
                else result.windows_compression = Observation<WindowsCompressionDetails>::unavailable(ObservationState::Failed,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "FileCompressionInfo.size"});
            } else {
                const DWORD code = GetLastError();
                result.windows_compression = failure<WindowsCompressionDetails>(code, "FileCompressionInfo");
            }
        } else result.windows_compression = Observation<WindowsCompressionDetails>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "storage.regular_compression_details"});
        return Result::available(std::move(result));
    }
    Observation<DataChannelCatalog> inspect_data_channels(InventoryLimits limits = {}) const override {
        using Result = Observation<DataChannelCatalog>;
        static_assert(offsetof(FILE_STREAM_INFO, StreamName) == detail::windows_stream_name_offset);
        static_assert(offsetof(FILE_STREAM_INFO, StreamNameLength) == 4);
        static_assert(offsetof(FILE_STREAM_INFO, StreamSize) == 8);
        static_assert(offsetof(FILE_STREAM_INFO, StreamAllocationSize) == 16);
        if (!limits.valid())
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "channels.limits"});
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (const auto guarded = provider_guard(*metadata.value())) return Result::unavailable(guarded->state, guarded->error);
        if (limits.max_native_bytes < sizeof(FILE_STREAM_INFO))
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "FileStreamInfo.buffer_limit"});
        auto capacity = std::min<std::size_t>(4096, limits.max_native_bytes);
        for (;;) {
            // uint64_t storage provides the SDK-required 8-byte alignment. The
            // supplied byte count is exact; rounding storage costs at most 7 bytes.
            std::vector<std::uint64_t> buffer((capacity + 7) / 8, 0);
            SetLastError(ERROR_SUCCESS);
            const bool success = GetFileInformationByHandleEx(handle_.get(), FileStreamInfo,
                buffer.data(), static_cast<DWORD>(capacity)) != 0;
            const DWORD code = GetLastError();
            // FileStreamInfo documents HANDLE_EOF when no streams are returned,
            // including its special success/no-streams outcome.
            if (code == ERROR_HANDLE_EOF) {
                DataChannelCatalog empty;
                empty.coverage = DataChannelCoverage::WindowsFileStreamInfo;
                empty.owner = identity(handle_.get(), scope_);
                return Result::available(std::move(empty));
            }
            if (success) {
                auto decoded = detail::decode_windows_stream_records(std::as_bytes(std::span(buffer)).first(capacity), limits);
                if (!decoded.value()) return decoded;
                auto result = std::move(decoded).take_value();
                result.owner = identity(handle_.get(), scope_);
                return Result::available(std::move(result));
            }
            if (code != ERROR_MORE_DATA && code != ERROR_INSUFFICIENT_BUFFER && code != ERROR_BAD_LENGTH)
                return failure<DataChannelCatalog>(code, "FileStreamInfo");
            if (capacity == limits.max_native_bytes)
                return Result::unavailable(ObservationState::Unknown, {ErrorDomain::Win32, code, "FileStreamInfo.buffer_limit"});
            capacity = capacity > limits.max_native_bytes / 2 ? limits.max_native_bytes : capacity * 2;
            // Retry this whole read-only query, never a continuation from an
            // unsuccessful/truncated prefix. Denial and I/O errors are not retried.
        }
    }
    Observation<AttributeCatalog> inspect_attributes(InventoryLimits limits = {}) const override {
        using Result = Observation<AttributeCatalog>;
        if (!limits.valid()) return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "attributes.limits"});
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (const auto guarded = provider_guard(*metadata.value())) return Result::unavailable(guarded->state, guarded->error);
        AttributeCatalog result;
        result.coverage = AttributeCoverage::WindowsExtendedAttributes;
        result.owner = identity(handle_.get(), scope_);
        std::set<std::string> seen;
        for (ULONG index = 1; ; ++index) {
            auto queried = query_ea(nullptr, 0, &index, limits);
            if (!queried.value()) {
                const auto& e = *queried.unavailable_detail();
                if (e.error.domain == ErrorDomain::NtStatus &&
                    (e.error.code == no_eas || e.error.code == no_more_eas || e.error.code == nonexistent_ea))
                    return Result::available(std::move(result));
                return Result::unavailable(e.state, e.error);
            }
            auto record = std::move(queried).take_value();
            if (record.value.empty() || !seen.insert(*record.identifier.windows_ea_bytes()).second)
                return Result::unavailable(ObservationState::Failed,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "attributes.ea_enumeration"});
            if (result.attributes.size() == limits.max_records)
                return Result::unavailable(ObservationState::Unknown,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InventoryLimit), "attributes.record_limit"});
            const auto role = attribute_role(record.identifier, record.flags);
            result.attributes.push_back({std::move(record.identifier), role,
                Observation<std::uint64_t>::available(record.value.size()), Observation<std::uint32_t>::available(record.flags)});
        }
    }
    Observation<AttributeValue> read_attribute(const NativeFeatureIdentifier& id, InventoryLimits limits = {}) const override {
        using Result = Observation<AttributeValue>;
        if (!limits.valid()) return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "attributes.limits"});
        const auto* name = id.windows_ea_bytes();
        if (!name || !NativeFeatureIdentifier::from_windows_ea(*name))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "attributes.identifier"});
        const auto metadata = inspect_basic();
        if (!metadata.value()) { const auto& e = *metadata.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        if (const auto guarded = provider_guard(*metadata.value())) return Result::unavailable(guarded->state, guarded->error);
        // FILE_GET_EA_INFORMATION: next offset, one-byte name length, name/NUL.
        std::vector<std::uint32_t> storage((6 + name->size() + 3) / 4, 0);
        auto request = std::as_writable_bytes(std::span(storage));
        request[4] = static_cast<std::byte>(name->size());
        std::memcpy(request.data() + 5, name->data(), name->size());
        auto queried = query_ea(request.data(), static_cast<ULONG>(6 + name->size()), nullptr, limits);
        if (!queried.value()) { const auto& e = *queried.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        auto record = std::move(queried).take_value();
        if (record.value.empty()) // Native filtered-query witness: zero deletes an EA.
            return Result::unavailable(ObservationState::Gone,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::AttributeAbsent), "attributes.ea_absent"});
        return Result::available({std::move(record.identifier), std::move(record.value), identity(handle_.get(), scope_),
            Observation<std::uint32_t>::available(record.flags)});
    }
    Observation<NativeSecurityValue> read_security(SecurityPart part, InventoryLimits limits = {}) const override {
        using Result = Observation<NativeSecurityValue>;
        if (!valid_security_part(part) || !limits.valid())
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(!valid_security_part(part) ? AdapterError::InvalidIdentifier : AdapterError::InvalidLimit), "security.query"});
        SECURITY_INFORMATION mask = 0;
        switch (part) {
        case SecurityPart::WindowsAccess: mask = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION; break;
        case SecurityPart::WindowsIntegrity: mask = LABEL_SECURITY_INFORMATION; break;
        case SecurityPart::WindowsFullDescriptor: mask = BACKUP_SECURITY_INFORMATION; break;
        default: return Result::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "security.model"});
        }
        // No named lookup and no privilege adjustment. Missing handle rights
        // remain native denial, not an empty descriptor or silently weaker query.
        auto bytes = detail::read_attribute_bytes([&](std::span<std::byte> buffer) -> detail::BufferReply {
            DWORD needed = 0;
            const bool success = GetKernelObjectSecurity(handle_.get(), mask,
                buffer.empty() ? nullptr : buffer.data(), static_cast<DWORD>(buffer.size()), &needed) != 0;
            const DWORD code = GetLastError();
            if (success) return static_cast<std::size_t>(needed);
            const NativeError error{ErrorDomain::Win32, code, "GetKernelObjectSecurity"};
            if (code == ERROR_INSUFFICIENT_BUFFER)
                return buffer.empty() ? detail::BufferReply{static_cast<std::size_t>(needed)} : detail::BufferReply{detail::BufferTooSmall{error}};
            return UnavailableObservation{error_state(code), error};
        }, limits);
        if (!bytes.value()) { const auto& e = *bytes.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto checked = detail::validate_windows_descriptor(*bytes.value(), limits);
        if (!checked.value()) { const auto& e = *checked.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available({part, SecurityEncoding::WindowsRelativeDescriptor, SecurityPresence::Present,
            std::move(bytes).take_value(), identity(handle_.get(), scope_), mask});
    }
private:
    friend class WindowsDirectoryCursor;
    static std::optional<UnavailableObservation> provider_guard(const BasicMetadata& metadata) {
        const auto review = provider_review_required(metadata, StorageModel::WindowsHandleAttributes);
        if (!review.value()) return *review.unavailable_detail();
        if (*review.value()) return UnavailableObservation{ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::ProviderReviewRequired), "storage.provider_review"}};
        return std::nullopt;
    }
    Observation<detail::WindowsEaRecord> query_ea(void* names, ULONG names_size, PULONG index, InventoryLimits limits) const {
        using Result = Observation<detail::WindowsEaRecord>;
        const auto api = query_ea_api();
        if (!api.value()) { const auto& e = *api.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        auto capacity = std::min<std::size_t>(4096, limits.max_native_bytes);
        for (;;) {
            std::vector<std::uint32_t> buffer((capacity + 3) / 4, 0);
            IO_STATUS_BLOCK io{};
            // Explicit one-based index makes retries independent of the handle's
            // EA enumeration cursor. A name list, when present, selects the key.
            const NTSTATUS status = (*api.value())(handle_.get(), &io, buffer.data(), static_cast<ULONG>(capacity),
                TRUE, names, names_size, index, FALSE);
            const auto code = static_cast<std::uint32_t>(status);
            if (code == 0 || (code == buffer_overflow && io.Information != 0)) {
                if (io.Information > capacity)
                    return Result::unavailable(ObservationState::Failed,
                        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "attributes.ea_count"});
                return detail::decode_windows_ea_record(std::as_bytes(std::span(buffer)).first(static_cast<std::size_t>(io.Information)));
            }
            if (code != buffer_too_small && code != buffer_overflow) return ea_failure<detail::WindowsEaRecord>(code, "NtQueryEaFile");
            if (capacity == limits.max_native_bytes)
                return Result::unavailable(ObservationState::Unknown, {ErrorDomain::NtStatus, code, "NtQueryEaFile.buffer_limit"});
            capacity = capacity > limits.max_native_bytes / 2 ? limits.max_native_bytes : capacity * 2;
        }
    }
    OwnedHandle handle_;
    AdapterScope scope_;
    bool layout_access_;
    bool content_access_ = false;
    bool content_write_access_ = false;
};

Observation<detail::WindowsParentIds> windows_parent_ids(HANDLE handle, IdentityScheme scheme) {
    using Result = Observation<detail::WindowsParentIds>;
    static_assert(offsetof(USN_RECORD_V2, FileName) == 60 && offsetof(USN_RECORD_V3, FileName) == 76);
    const WORD major = scheme == IdentityScheme::WindowsFileId128 ? 3 : scheme == IdentityScheme::WindowsFileIndex64 ? 2 : 0;
    if (major == 0) return Result::unavailable(ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "parent.usn_scheme"});
    READ_FILE_USN_DATA input{major, major};
    // USHORT name length plus the largest supported header, rounded to 8 bytes.
    alignas(8) std::array<std::byte, 65536 + 80> buffer{};
    DWORD bytes = 0;
    if (!DeviceIoControl(handle, FSCTL_READ_FILE_USN_DATA, &input, sizeof(input), buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) {
        const auto code = GetLastError();
        if (code == ERROR_JOURNAL_NOT_ACTIVE || code == ERROR_JOURNAL_DELETE_IN_PROGRESS)
            return Result::unavailable(ObservationState::Unsupported, {ErrorDomain::Win32, code, "FSCTL_READ_FILE_USN_DATA.parent"});
        return failure<detail::WindowsParentIds>(code, "FSCTL_READ_FILE_USN_DATA.parent");
    }
    if (bytes > buffer.size()) return Result::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "parent.usn_bytes"});
    return detail::decode_windows_parent_record(std::span<const std::byte>(buffer).first(bytes), major);
}
Observation<std::unique_ptr<OpenedEntry>> open_windows_parent(HANDLE child, const AdapterScope& scope) {
    using Result = Observation<std::unique_ptr<OpenedEntry>>;
    const auto check = [](const Observation<BasicMetadata>& metadata) -> std::optional<UnavailableObservation> {
        if (!metadata.value()) return *metadata.unavailable_detail();
        if (!metadata.value()->kind.value()) return *metadata.value()->kind.unavailable_detail();
        if (*metadata.value()->kind.value() != ObjectKind::Directory) return UnavailableObservation{ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "parent.object_kind"}};
        const auto review = provider_review_required(*metadata.value(), StorageModel::WindowsHandleAttributes);
        if (!review.value()) return *review.unavailable_detail();
        if (*review.value()) return UnavailableObservation{ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::ProviderReviewRequired), "parent.provider_review"}};
        return {};
    };
    const auto before = handle_metadata(child, scope);
    if (const auto denied = check(before)) return Result::unavailable(denied->state, denied->error);
    if (!before.value()->identity.value()) { const auto& e = *before.value()->identity.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    const auto& own = *before.value()->identity.value();
    auto ids = windows_parent_ids(child, own.scheme);
    if (!ids.value()) { const auto& e = *ids.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    const auto qualify = [&](const NativeIdentifier& id, const ObjectIdentity& observed) -> std::optional<UnavailableObservation> {
        const auto compared = compare_identity({scope, ids.value()->scheme, own.volume, id, {}}, observed);
        if (compared == IdentityComparison::Same) return {};
        return UnavailableObservation{compared == IdentityComparison::Different ? ObservationState::Changed : ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(compared == IdentityComparison::Different ? AdapterError::MetadataChanged : AdapterError::UnqualifiedIdentity), "parent.usn_identity"}};
    };
    if (const auto mismatch = qualify(ids.value()->child, own)) return Result::unavailable(mismatch->state, mismatch->error);
    if ((ids.value()->attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return Result::unavailable(ObservationState::Changed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "parent.usn_kind"});
    FILE_ID_DESCRIPTOR descriptor{}; descriptor.dwSize = sizeof(descriptor);
    if (own.scheme == IdentityScheme::WindowsFileId128) { descriptor.Type = ExtendedFileIdType; std::memcpy(descriptor.ExtendedFileId.Identifier, ids.value()->parent.bytes.data(), 16); }
    else { descriptor.Type = FileIdType; std::memcpy(&descriptor.FileId.QuadPart, ids.value()->parent.bytes.data(), 8); }
    // Original live directory supplies the volume hint. No basename/path open,
    // enumeration, FILE_READ_DATA, backup intent, elevation or journal mutation.
    OwnedHandle parent(OpenFileById(child, &descriptor, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL));
    if (parent.get() == INVALID_HANDLE_VALUE || parent.get() == nullptr) return failure<std::unique_ptr<OpenedEntry>>(GetLastError(), "OpenFileById.parent");
    auto opened = std::make_unique<WindowsOpenedEntry>(std::move(parent), scope);
    const auto metadata = opened->inspect_basic();
    if (const auto denied = check(metadata)) return Result::unavailable(denied->state, denied->error);
    if (!metadata.value()->identity.value()) { const auto& e = *metadata.value()->identity.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (const auto mismatch = qualify(ids.value()->parent, *metadata.value()->identity.value())) return Result::unavailable(mismatch->state, mismatch->error);
    const auto again = windows_parent_ids(child, own.scheme);
    if (!again.value()) { const auto& e = *again.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (again.value()->scheme != ids.value()->scheme || again.value()->child != ids.value()->child || again.value()->parent != ids.value()->parent ||
        (again.value()->attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return Result::unavailable(ObservationState::Changed, {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "parent.usn_changed"});
    const auto after = handle_metadata(child, scope);
    if (const auto denied = check(after)) return Result::unavailable(denied->state, denied->error);
    if (!after.value()->identity.value()) { const auto& e = *after.value()->identity.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (const auto mismatch = qualify(ids.value()->child, *after.value()->identity.value())) return Result::unavailable(mismatch->state, mismatch->error);
    return Result::available(std::move(opened));
}
Observation<std::unique_ptr<OpenedEntry>> WindowsOpenedEntry::open_parent_directory() const { return open_windows_parent(handle_.get(), scope_); }

// Public FILE_FS_FULL_SIZE_INFORMATION wire layout. The desktop SDK does not
// declare this WDK structure; no private members or syscall numbers are used.
struct alignas(8) VolumeSpaceCounters {
    std::int64_t total;
    std::int64_t caller_available;
    std::int64_t actual_available;
    std::uint32_t sectors_per_unit;
    std::uint32_t bytes_per_sector;
};
static_assert(sizeof(VolumeSpaceCounters) == 32 && offsetof(VolumeSpaceCounters, sectors_per_unit) == 24);
Observation<VolumeSpaceCounters> volume_space(HANDLE handle) {
    using Result = Observation<VolumeSpaceCounters>;
    using QueryVolume = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    const auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryVolumeInformationFile");
    if (!address) return Result::unavailable(ObservationState::Unsupported, {ErrorDomain::Win32, GetLastError(), "NtQueryVolumeInformationFile.resolve"});
    const auto query = std::bit_cast<QueryVolume>(address);
    VolumeSpaceCounters counters{};
    IO_STATUS_BLOCK io{};
    // FileFsFullSizeInformation = 7 in the documented FS information classes.
    // This directory's Win32 handle is synchronous (no OVERLAPPED flag).
    const auto status = query(handle, &io, &counters, sizeof(counters), 7);
    if (status != 0) return Result::unavailable(error_state(RtlNtStatusToDosError(status)),
        {ErrorDomain::NtStatus, static_cast<std::uint32_t>(status), "NtQueryVolumeInformationFile.full_size"});
    if (io.Information != sizeof(counters)) return Result::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "NtQueryVolumeInformationFile.full_size_length"});
    return Result::available(counters);
}

class WindowsDirectoryCursor final : public DirectoryCursor {
public:
    WindowsDirectoryCursor(OwnedHandle handle, Observation<ObjectIdentity> native_identity, AdapterScope scope)
        : DirectoryCursor(std::move(native_identity)), handle_(std::move(handle)), scope_(std::move(scope)) {}
    Observation<AdapterScope> source_scope() const override { return Observation<AdapterScope>::available(scope_); }
    Observation<BasicMetadata> inspect_basic() const override { return handle_metadata(handle_.get(), scope_); }
    Observation<std::unique_ptr<OpenedEntry>> open_parent_directory() const override { return open_windows_parent(handle_.get(), scope_); }
    Observation<std::unique_ptr<OpenedEntry>> create_child_file(const NativeName& name) const override {
        using Result = Observation<std::unique_ptr<OpenedEntry>>;
        const auto* units = name.windows_units();
        if (!units || !name.is_valid() || units->find(u':') != std::u16string::npos ||
            units->size() > std::numeric_limits<USHORT>::max() / sizeof(WCHAR))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "create_child_file.name"});
        std::wstring spelling;
        spelling.reserve(units->size());
        for (const auto unit : *units) spelling.push_back(static_cast<wchar_t>(unit));
        UNICODE_STRING native_name{};
        native_name.Length = static_cast<USHORT>(spelling.size() * sizeof(WCHAR));
        native_name.MaximumLength = native_name.Length;
        native_name.Buffer = spelling.data();
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = handle_.get();
        attributes.ObjectName = &native_name;
        IO_STATUS_BLOCK io{};
        HANDLE raw = nullptr;
        const NTSTATUS status = NtCreateFile(&raw, FILE_GENERIC_READ | FILE_GENERIC_WRITE | READ_CONTROL | DELETE,
            &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_CREATE,
            FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
            nullptr, 0);
        OwnedHandle opened(raw);
        if (status < 0)
            return Result::unavailable(error_state(RtlNtStatusToDosError(status)),
                {ErrorDomain::NtStatus, static_cast<std::int64_t>(status), "NtCreateFile.create_child_file"});
        if (io.Information != FILE_CREATED || raw == nullptr || raw == INVALID_HANDLE_VALUE)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "create_child_file.result"});
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(opened.get(), FileBasicInfo, &basic, sizeof(basic)))
            return failure<std::unique_ptr<OpenedEntry>>(GetLastError(), "create_child_file.info");
        if ((basic.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "create_child_file.kind"});
        return Result::available(std::make_unique<WindowsOpenedEntry>(std::move(opened), scope_, false, true, true));
    }
    Observation<std::unique_ptr<DirectoryCursor>> create_child_directory(const NativeName& name) const override {
        using Result = Observation<std::unique_ptr<DirectoryCursor>>;
        const auto* units = name.windows_units();
        if (!units || !name.is_valid() || units->find(u':') != std::u16string::npos ||
            units->size() > std::numeric_limits<USHORT>::max() / sizeof(WCHAR))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "create_child_directory.name"});
        std::wstring spelling;
        spelling.reserve(units->size());
        for (const auto unit : *units) spelling.push_back(static_cast<wchar_t>(unit));
        UNICODE_STRING native_name{};
        native_name.Length = static_cast<USHORT>(spelling.size() * sizeof(WCHAR));
        native_name.MaximumLength = native_name.Length;
        native_name.Buffer = spelling.data();
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = handle_.get();
        attributes.ObjectName = &native_name;
        IO_STATUS_BLOCK io{};
        HANDLE raw = nullptr;
        const NTSTATUS status = NtCreateFile(&raw, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_CREATE,
            FILE_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
            nullptr, 0);
        OwnedHandle opened(raw);
        if (status < 0) {
            const DWORD code = RtlNtStatusToDosError(status);
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)
                return Result::unavailable(ObservationState::Changed,
                    {ErrorDomain::Win32, code, "NtCreateFile.create_child_directory.existing"});
            return Result::unavailable(error_state(code),
                {ErrorDomain::NtStatus, static_cast<std::int64_t>(status), "NtCreateFile.create_child_directory"});
        }
        if (io.Information != FILE_CREATED || raw == nullptr || raw == INVALID_HANDLE_VALUE)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "create_child_directory.result"});
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(opened.get(), FileBasicInfo, &basic, sizeof(basic)))
            return failure<std::unique_ptr<DirectoryCursor>>(GetLastError(), "create_child_directory.info");
        if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "create_child_directory.kind"});
        auto observed = identity(opened.get(), scope_);
        if (!observed.value()) {
            const auto& error = *observed.unavailable_detail();
            return Result::unavailable(error.state, error.error);
        }
        if (!FlushFileBuffers(handle_.get()))
            return failure<std::unique_ptr<DirectoryCursor>>(GetLastError(),
                "FlushFileBuffers.create_child_directory_parent");
        return Result::available(std::make_unique<WindowsDirectoryCursor>(std::move(opened), std::move(observed), scope_));
    }
    Observation<bool> retire_child(const NativeName& name, const ObjectIdentity& expected_identity) const override {
        using Result = Observation<bool>;
        if (!name.is_valid() || compare_identity(expected_identity, expected_identity) != IdentityComparison::Same)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "retire_child.input"});
        // Open relative to the retained parent with DELETE access and
        // no-reparse semantics. The resulting handle, not the basename, is
        // the disposition target, so a rename after open cannot redirect the
        // delete to a replacement object.
        auto opened = open_native_component(name, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                             ChildLookupMode::NativeDefault);
        if (!opened.value()) {
            const auto& error = *opened.unavailable_detail();
            return Result::unavailable(error.state, error.error);
        }
        auto source = std::move(opened).take_value();
        const auto metadata = handle_metadata(source.get(), scope_);
        if (!metadata.value()) {
            const auto& error = *metadata.unavailable_detail();
            return Result::unavailable(error.state, error.error);
        }
        if (!metadata.value()->identity.value() ||
            compare_identity(*metadata.value()->identity.value(), expected_identity) != IdentityComparison::Same)
            return Result::unavailable(ObservationState::Changed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "retire_child.identity"});
        if (!metadata.value()->kind.value() || *metadata.value()->kind.value() != ObjectKind::RegularFile)
            return Result::unavailable(ObservationState::Unsupported,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "retire_child.kind"});

        FILE_STANDARD_INFO standard{};
        if (!GetFileInformationByHandleEx(source.get(), FileStandardInfo, &standard, sizeof(standard)))
            return failure<bool>(GetLastError(), "retire_child.standard_info");
        // A selected hard-link alias is not a removable single-source object;
        // preserve the link group until the planner supplies its full policy.
        if (standard.NumberOfLinks != 1)
            return Result::unavailable(ObservationState::Unsupported,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "retire_child.hard_link"});

        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        if (!SetFileInformationByHandle(source.get(), FileDispositionInfo, &disposition, sizeof(disposition))) {
            const DWORD code = GetLastError();
            return Result::unavailable(code == ERROR_IO_PENDING ? ObservationState::Unknown : error_state(code),
                {ErrorDomain::Win32, code, "retire_child.disposition"});
        }
        if (!FlushFileBuffers(handle_.get()))
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Win32, GetLastError(), "retire_child.directory_flush"});
        return Result::available(true);
    }
    Observation<bool> rename_child_noreplace(const NativeName& name, const ObjectIdentity& expected_identity,
                                              const DirectoryCursor& destination,
                                              const NativeName& destination_name) const override {
        using Result = Observation<bool>;
        const auto* target = dynamic_cast<const WindowsDirectoryCursor*>(&destination);
        const auto* source_units = name.windows_units();
        const auto* destination_units = destination_name.windows_units();
        if (!target || !source_units || !destination_units || !name.is_valid() || !destination_name.is_valid() ||
            source_units->find(u':') != std::u16string::npos || destination_units->find(u':') != std::u16string::npos ||
            source_units->size() > std::numeric_limits<USHORT>::max() / sizeof(WCHAR) ||
            destination_units->size() > std::numeric_limits<USHORT>::max() / sizeof(WCHAR))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "rename_child_noreplace.name"});
        const auto destination_identity = target->identity();
        if (!destination_identity.value() || compare_identity(*destination_identity.value(), *destination_identity.value()) != IdentityComparison::Same ||
            expected_identity.scope != scope_ || destination_identity.value()->scope != scope_ ||
            expected_identity.scheme != destination_identity.value()->scheme ||
            expected_identity.volume != destination_identity.value()->volume)
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "rename_child_noreplace.parent_identity"});

        auto source_opened = open_native_component(name, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                                   ChildLookupMode::NativeDefault);
        if (!source_opened.value()) {
            const auto& error = *source_opened.unavailable_detail();
            return Result::unavailable(error.state, error.error);
        }
        auto source = std::move(source_opened).take_value();
        const auto source_metadata = handle_metadata(source.get(), scope_);
        if (!source_metadata.value() || !source_metadata.value()->identity.value() ||
            compare_identity(*source_metadata.value()->identity.value(), expected_identity) != IdentityComparison::Same)
            return Result::unavailable(ObservationState::Changed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "rename_child_noreplace.source_identity"});

        auto occupied = target->open_native_component(destination_name, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                                      ChildLookupMode::NativeDefault);
        if (occupied.value())
            return Result::unavailable(ObservationState::Changed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::PlanDecisionStale), "rename_child_noreplace.destination_occupied"});
        if (!occupied.unavailable_detail() || occupied.unavailable_detail()->state != ObservationState::Gone)
            return Result::unavailable(occupied.unavailable_detail() ? occupied.unavailable_detail()->state : ObservationState::Unknown,
                occupied.unavailable_detail() ? occupied.unavailable_detail()->error :
                    NativeError{ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "rename_child_noreplace.destination_probe"});

        std::wstring spelling;
        spelling.reserve(destination_units->size());
        for (const auto unit : *destination_units) spelling.push_back(static_cast<wchar_t>(unit));
        const auto length = spelling.size() * sizeof(WCHAR);
        if (length > std::numeric_limits<ULONG>::max() - offsetof(FILE_RENAME_INFO, FileName))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "rename_child_noreplace.name_limit"});
        std::vector<std::byte> buffer(offsetof(FILE_RENAME_INFO, FileName) + length);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = target->handle_.get();
        info->FileNameLength = static_cast<DWORD>(length);
        std::memcpy(info->FileName, spelling.data(), length);
        if (!SetFileInformationByHandle(source.get(), FileRenameInfo, info, static_cast<DWORD>(buffer.size()))) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)
                return Result::unavailable(ObservationState::Changed,
                    {ErrorDomain::Win32, code, "rename_child_noreplace.destination_occupied"});
            if (code == ERROR_INVALID_FUNCTION || code == ERROR_NOT_SUPPORTED || code == ERROR_CALL_NOT_IMPLEMENTED ||
                code == ERROR_INVALID_PARAMETER)
                return Result::unavailable(ObservationState::Unsupported,
                    {ErrorDomain::Win32, code, "rename_child_noreplace.atomic_primitive"});
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Win32, code, "rename_child_noreplace.atomic_primitive"});
        }

        auto published = target->open_native_component(destination_name, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                                       ChildLookupMode::NativeDefault);
        if (!published.value())
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "rename_child_noreplace.destination_identity"});
        const auto published_metadata = handle_metadata(published.value()->get(), scope_);
        if (!published_metadata.value() || !published_metadata.value()->identity.value() ||
            compare_identity(*published_metadata.value()->identity.value(), *source_metadata.value()->identity.value()) != IdentityComparison::Same)
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "rename_child_noreplace.destination_identity"});
        if (!FlushFileBuffers(handle_.get()) || !FlushFileBuffers(target->handle_.get()))
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Win32, GetLastError(), "rename_child_noreplace.directory_flush"});
        return Result::available(true);
    }
    Observation<bool> rename_child_case_only(const NativeName& name, const ObjectIdentity& expected_identity,
                                              const NativeName& destination_name) const override {
        using Result = Observation<bool>;
        const auto* source_units = name.windows_units();
        const auto* destination_units = destination_name.windows_units();
        if (!source_units || !destination_units || !name.is_valid() || !destination_name.is_valid() ||
            name == destination_name || source_units->find(u':') != std::u16string::npos ||
            destination_units->find(u':') != std::u16string::npos ||
            source_units->size() > std::numeric_limits<USHORT>::max() / sizeof(WCHAR) ||
            destination_units->size() > std::numeric_limits<USHORT>::max() / sizeof(WCHAR))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "rename_child_case_only.name"});
        auto source_opened = open_native_component(name, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                                   ChildLookupMode::NativeDefault);
        if (!source_opened.value()) {
            const auto& error = *source_opened.unavailable_detail();
            return Result::unavailable(error.state, error.error);
        }
        auto source = std::move(source_opened).take_value();
        const auto source_metadata = handle_metadata(source.get(), scope_);
        if (!source_metadata.value() || !source_metadata.value()->identity.value() ||
            compare_identity(*source_metadata.value()->identity.value(), expected_identity) != IdentityComparison::Same)
            return Result::unavailable(ObservationState::Changed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "rename_child_case_only.source_identity"});
        if (source_metadata.value()->kind.value() && *source_metadata.value()->kind.value() == ObjectKind::RegularFile) {
            FILE_STANDARD_INFO standard{};
            if (!GetFileInformationByHandleEx(source.get(), FileStandardInfo, &standard, sizeof(standard)))
                return failure<bool>(GetLastError(), "rename_child_case_only.standard_info");
            if (standard.NumberOfLinks != 1)
                return Result::unavailable(ObservationState::Unsupported,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "rename_child_case_only.hard_link"});
        }

        auto destination = open_native_component(destination_name, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                                 ChildLookupMode::NativeDefault);
        if (!destination.value()) {
            if (destination.unavailable_detail() && destination.unavailable_detail()->state == ObservationState::Gone)
                return Result::unavailable(ObservationState::Unsupported,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "rename_child_case_only.native_case_proof"});
            const auto& error = *destination.unavailable_detail();
            return Result::unavailable(error.state, error.error);
        }
        const auto destination_metadata = handle_metadata(destination.value()->get(), scope_);
        if (!destination_metadata.value() || !destination_metadata.value()->identity.value())
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "rename_child_case_only.destination_identity"});
        if (compare_identity(*destination_metadata.value()->identity.value(), *source_metadata.value()->identity.value()) != IdentityComparison::Same)
            return Result::unavailable(ObservationState::Changed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::PlanDecisionStale), "rename_child_case_only.destination_occupied"});

        std::wstring spelling;
        spelling.reserve(destination_units->size());
        for (const auto unit : *destination_units) spelling.push_back(static_cast<wchar_t>(unit));
        const auto length = spelling.size() * sizeof(WCHAR);
        if (length > std::numeric_limits<ULONG>::max() - offsetof(FILE_RENAME_INFO, FileName))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "rename_child_case_only.name_limit"});
        std::vector<std::byte> buffer(offsetof(FILE_RENAME_INFO, FileName) + length);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = handle_.get();
        info->FileNameLength = static_cast<DWORD>(length);
        std::memcpy(info->FileName, spelling.data(), length);
        if (!SetFileInformationByHandle(source.get(), FileRenameInfo, info, static_cast<DWORD>(buffer.size()))) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)
                return Result::unavailable(ObservationState::Changed,
                    {ErrorDomain::Win32, code, "rename_child_case_only.destination_occupied"});
            if (code == ERROR_INVALID_FUNCTION || code == ERROR_NOT_SUPPORTED || code == ERROR_CALL_NOT_IMPLEMENTED ||
                code == ERROR_INVALID_PARAMETER)
                return Result::unavailable(ObservationState::Unsupported,
                    {ErrorDomain::Win32, code, "rename_child_case_only.atomic_primitive"});
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Win32, code, "rename_child_case_only.atomic_primitive"});
        }
        auto renamed = open_native_component(destination_name, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                             ChildLookupMode::NativeDefault);
        if (!renamed.value())
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "rename_child_case_only.destination_identity"});
        const auto renamed_metadata = handle_metadata(renamed.value()->get(), scope_);
        if (!renamed_metadata.value() || !renamed_metadata.value()->identity.value() ||
            compare_identity(*renamed_metadata.value()->identity.value(), *source_metadata.value()->identity.value()) != IdentityComparison::Same)
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "rename_child_case_only.destination_identity"});
        if (!FlushFileBuffers(handle_.get()))
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Win32, GetLastError(), "rename_child_case_only.directory_flush"});
        return Result::available(true);
    }
    Observation<bool> publish_noreplace(HANDLE source, const NativeName& name) const {
        using Result = Observation<bool>;
        const auto* units = name.windows_units();
        if (!name.is_valid() || !units || units->find(u':') != std::u16string::npos ||
            units->size() > (std::numeric_limits<ULONG>::max() / sizeof(WCHAR)))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "publish_noreplace.name"});
        const auto length = units->size() * sizeof(WCHAR);
        if (length > std::numeric_limits<ULONG>::max() - offsetof(FILE_RENAME_INFO, FileName))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidLimit), "publish_noreplace.name_limit"});
        std::vector<std::byte> buffer(offsetof(FILE_RENAME_INFO, FileName) + length);
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = handle_.get();
        info->FileNameLength = static_cast<DWORD>(length);
        std::memcpy(info->FileName, units->data(), length);
        if (!SetFileInformationByHandle(source, FileRenameInfo, info, static_cast<DWORD>(buffer.size()))) {
            const DWORD code = GetLastError();
            if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS)
                return Result::unavailable(ObservationState::Changed,
                    {ErrorDomain::Win32, code, "publish_noreplace.destination_occupied"});
            if (code == ERROR_INVALID_FUNCTION || code == ERROR_NOT_SUPPORTED || code == ERROR_CALL_NOT_IMPLEMENTED ||
                code == ERROR_INVALID_PARAMETER)
                return Result::unavailable(ObservationState::Unsupported,
                    {ErrorDomain::Win32, code, "publish_noreplace.atomic_primitive"});
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Win32, code, "publish_noreplace.atomic_primitive"});
        }

        auto opened = open_native_component(name, FILE_READ_ATTRIBUTES | SYNCHRONIZE, ChildLookupMode::NativeDefault);
        if (!opened.value())
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "publish_noreplace.destination_identity"});
        auto destination = std::move(opened).take_value();
        const auto source_identity = identity(source, scope_);
        const auto destination_metadata = handle_metadata(destination.get(), scope_);
        if (!source_identity.value() || !destination_metadata.value() || !destination_metadata.value()->identity.value() ||
            compare_identity(*source_identity.value(), *destination_metadata.value()->identity.value()) != IdentityComparison::Same ||
            !destination_metadata.value()->kind.value() || *destination_metadata.value()->kind.value() != ObjectKind::RegularFile)
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::MetadataChanged), "publish_noreplace.destination_identity"});
        if (!FlushFileBuffers(handle_.get()))
            return Result::unavailable(ObservationState::Unknown,
                {ErrorDomain::Win32, GetLastError(), "publish_noreplace.directory_flush"});
        return Result::available(true);
    }
    Observation<DirectoryAccessCheck> check_access(DirectoryAccess access) const override {
        using Result = Observation<DirectoryAccessCheck>;
        if (!valid_directory_access(access)) return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "directory.check_access"});
        const auto before = inspect_basic();
        const auto qualified = detail::qualify_access_directory(before, before, StorageModel::WindowsHandleAttributes);
        if (!qualified.value()) { const auto& e = *qualified.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto& id = *qualified.value();
        FILE_ID_DESCRIPTOR descriptor{}; descriptor.dwSize = sizeof(descriptor);
        if (id.scheme == IdentityScheme::WindowsFileId128) { descriptor.Type = ExtendedFileIdType; std::memcpy(descriptor.ExtendedFileId.Identifier, id.object.bytes.data(), 16); }
        else if (id.scheme == IdentityScheme::WindowsFileIndex64) { descriptor.Type = FileIdType; std::memcpy(&descriptor.FileId.QuadPart, id.object.bytes.data(), 8); }
        else return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "directory.access_scheme"});
        const DWORD right = access == DirectoryAccess::List ? FILE_LIST_DIRECTORY : access == DirectoryAccess::Search ? FILE_TRAVERSE :
            access == DirectoryAccess::AddFile ? FILE_ADD_FILE : FILE_ADD_SUBDIRECTORY;
        const DWORD mask = right | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
        // A temporary access-bearing handle, no content/namespace write. No
        // backup intent or privilege enablement; unsupported drivers may refuse.
        OwnedHandle checked(OpenFileById(handle_.get(), &descriptor, mask,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL));
        if (checked.get() == INVALID_HANDLE_VALUE || checked.get() == nullptr) return failure<DirectoryAccessCheck>(GetLastError(), "OpenFileById.directory_access");
        const auto opened = detail::qualify_access_directory(before, handle_metadata(checked.get(), scope_), StorageModel::WindowsHandleAttributes);
        if (!opened.value()) { const auto& e = *opened.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        auto after = detail::qualify_access_directory(before, inspect_basic(), StorageModel::WindowsHandleAttributes);
        if (!after.value()) { const auto& e = *after.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available({access, DirectoryAccessFamily::WindowsOpenById, std::move(after).take_value(), mask});
    }
    Observation<LocationInfo> inspect_location() const override {
        LocationInfo result;
        result.family = LocationQueryFamily::WindowsHandle;
        result.linux_directory_inode_flags = Observation<std::uint32_t>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "volume.linux_directory_inode_flags"});
        result.directory = ck::files::identity(handle_.get(), scope_);
        const auto not_applicable = [](const char* operation) {
            return Observation<std::uint64_t>::unavailable(ObservationState::Unsupported,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), operation});
        };
        result.filesystem_type_code = not_applicable("volume.filesystem_type_code");
        result.filesystem_query_id = Observation<NativeIdentifier>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "volume.filesystem_query_id"});
        result.posix_name_max = result.posix_name_chars_max = result.posix_path_max = not_applicable("volume.posix_limits");
        result.posix_file_size_bits = result.posix_symlink_max = not_applicable("volume.posix_file_limits");
        result.native_mount_flags = not_applicable("volume.posix_mount_flags");
        DWORD serial = 0, maximum = 0, flags = 0;
        std::array<wchar_t, MAX_PATH + 1> name{};
        if (GetVolumeInformationByHandleW(handle_.get(), nullptr, 0, &serial, &maximum, &flags, name.data(), static_cast<DWORD>(name.size()))) {
            result.volume_query_serial = Observation<std::uint64_t>::available(serial);
            result.windows_component_utf16_max = maximum != 0 ? Observation<std::uint64_t>::available(maximum) :
                Observation<std::uint64_t>::unavailable(ObservationState::Unknown,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "volume.component_limit"});
            NativeVolumeFeatures features;
            features.domain = VolumeFeatureDomain::WindowsVolumeFlags;
            features.value[0] = flags;
            features.valid[0] = std::numeric_limits<std::uint32_t>::max();
            result.features = Observation<NativeVolumeFeatures>::available(features);
            result.read_only_mount = Observation<bool>::available((flags & FILE_READ_ONLY_VOLUME) != 0);
            result.case_preserving_names = Observation<bool>::available((flags & FILE_CASE_PRESERVED_NAMES) != 0);
            const auto end = std::find(name.begin(), name.end(), L'\0');
            std::u16string text;
            for (auto at = name.begin(); at != end; ++at) text.push_back(static_cast<char16_t>(*at));
            auto id = end != name.end() ? NativeFeatureIdentifier::from_windows(std::move(text)) : std::nullopt;
            result.filesystem_name = id ? Observation<NativeFeatureIdentifier>::available(std::move(*id)) :
                Observation<NativeFeatureIdentifier>::unavailable(ObservationState::Failed,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "volume.filesystem_name"});
        } else {
            const DWORD code = GetLastError();
            const NativeError error{ErrorDomain::Win32, code, "GetVolumeInformationByHandleW"};
            result.volume_query_serial = result.windows_component_utf16_max = Observation<std::uint64_t>::unavailable(error_state(code), error);
            result.features = Observation<NativeVolumeFeatures>::unavailable(error_state(code), error);
            result.filesystem_name = Observation<NativeFeatureIdentifier>::unavailable(error_state(code), error);
            result.read_only_mount = result.case_preserving_names = Observation<bool>::unavailable(error_state(code), error);
        }
        FILE_CASE_SENSITIVE_INFO case_info{};
        if (GetFileInformationByHandleEx(handle_.get(), FileCaseSensitiveInfo, &case_info, sizeof(case_info))) {
            result.windows_directory_case_flags = Observation<std::uint32_t>::available(case_info.Flags);
            result.case_sensitive_directory = Observation<bool>::available((case_info.Flags & FILE_CS_FLAG_CASE_SENSITIVE_DIR) != 0);
        } else {
            const DWORD code = GetLastError();
            const NativeError error{ErrorDomain::Win32, code, "FileCaseSensitiveInfo"};
            result.windows_directory_case_flags = Observation<std::uint32_t>::unavailable(error_state(code), error);
            result.case_sensitive_directory = Observation<bool>::unavailable(error_state(code), error);
        }
        const auto space = volume_space(handle_.get());
        if (space.value()) {
            const auto native_unit = static_cast<std::uint64_t>(space.value()->sectors_per_unit) * space.value()->bytes_per_sector;
            const auto unit = detail::space_bytes(1, native_unit);
            result.space_accounting_unit = unit;
            if (unit.value()) {
                result.total_bytes = detail::signed_space_bytes(space.value()->total, *unit.value());
                result.free_bytes = detail::signed_space_bytes(space.value()->actual_available, *unit.value());
                result.available_bytes = detail::signed_space_bytes(space.value()->caller_available, *unit.value());
            } else result.total_bytes = result.free_bytes = result.available_bytes = unit;
        } else {
            const auto& e = *space.unavailable_detail();
            result.space_accounting_unit = result.total_bytes = result.free_bytes = result.available_bytes =
                Observation<std::uint64_t>::unavailable(e.state, e.error);
        }
        return Observation<LocationInfo>::available(std::move(result));
    }
    Observation<std::unique_ptr<DirectoryCursor>> open_directory_child(const NativeName& name) const override {
        using Result = Observation<std::unique_ptr<DirectoryCursor>>;
        // As for the absolute Win32 directory open, retain reparse/no-recall
        // options and validate kind on the returned handle before enumeration.
        // FILE_DIRECTORY_FILE has a restricted compatible-option set; do not
        // combine it here or retry an error with weaker no-follow protection.
        auto result = open_native_component(name, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
            ChildLookupMode::NativeDefault);
        if (!result.value()) {
            const auto& error = *result.unavailable_detail();
            return Result::unavailable(error.state, error.error);
        }
        auto child = std::move(result).take_value();
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandleEx(child.get(), FileBasicInfo, &basic, sizeof(basic)))
            return failure<std::unique_ptr<DirectoryCursor>>(GetLastError(), "FileBasicInfo.directory_child");
        if ((basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::FinalLinkRejected), "open_directory_child.reparse"});
        if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotDirectory), "open_directory_child.kind"});
        auto observed = ck::files::identity(child.get(), scope_);
        return Result::available(std::make_unique<WindowsDirectoryCursor>(std::move(child), std::move(observed), scope_));
    }
    Observation<ChildLookup> lookup_child(const NativeName& name, ChildLookupMode mode) const override {
        using Result=Observation<ChildLookup>;
        if(!valid_child_lookup_mode(mode))return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter,static_cast<std::int64_t>(AdapterError::InvalidIdentifier),"lookup_child.mode"});
        auto opened=open_component(name,EntryAccess::MetadataOnly,mode);
        if(opened.value()) {
            auto owned=std::move(opened).take_value();
            if(!owned)return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter,static_cast<std::int64_t>(AdapterError::InvalidNativeResult),"lookup_child.null_entry"});
            return Result::available(ChildLookup::found({name,mode,identity()},std::move(owned)));
        }
        const auto& e=*opened.unavailable_detail();
        // STATUS_OBJECT_NAME_NOT_FOUND / STATUS_NO_SUCH_FILE. Never turn a
        // path failure, delete-pending state or generic Gone into absence.
        if(detail::definite_child_absence(e,detail::ChildLookupFamily::WindowsNt))
            return Result::available(ChildLookup::missing({name,mode,identity()},e.error));
        return Result::unavailable(e.state,e.error);
    }
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& name, EntryAccess access = EntryAccess::MetadataOnly) const override {
        return open_component(name,access,ChildLookupMode::NativeDefault);
    }
private:
    Observation<std::unique_ptr<OpenedEntry>> open_component(const NativeName& name, EntryAccess access, ChildLookupMode mode) const {
        if (!valid_entry_access(access)) return Observation<std::unique_ptr<OpenedEntry>>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "open_child.access"});
        const auto rights = FILE_READ_ATTRIBUTES | SYNCHRONIZE | (requests_content_read(access) ? FILE_READ_DATA : 0) |
            (requests_attributes(access) ? FILE_READ_EA : 0) |
            (requests_security(access) ? READ_CONTROL : 0) | (requests_audit_security(access) ? ACCESS_SYSTEM_SECURITY : 0);
        auto result = open_native_component(name, rights, mode);
        if (!result.value()) {
            const auto& error = *result.unavailable_detail();
            return Observation<std::unique_ptr<OpenedEntry>>::unavailable(error.state, error.error);
        }
        using Result = Observation<std::unique_ptr<OpenedEntry>>;
        auto original = std::make_unique<WindowsOpenedEntry>(std::move(result).take_value(), scope_, false,
                                                             requests_content_read(access));
        if (!requests_layout(access)) return Result::available(std::move(original));
        const auto before = original->inspect_basic();
        if (!before.value()) { const auto& e = *before.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto guard = detail::data_layout_access_guard(*before.value(), StorageModel::WindowsHandleAttributes);
        if (!guard.value()) { const auto& e = *guard.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto* id = before.value()->identity.value();
        if (!id || compare_identity(*id, *id) != IdentityComparison::Same) return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "layout.open_identity"});
        FILE_ID_DESCRIPTOR descriptor{}; descriptor.dwSize = sizeof(descriptor);
        if (id->scheme == IdentityScheme::WindowsFileId128) {
            descriptor.Type = ExtendedFileIdType; std::memcpy(descriptor.ExtendedFileId.Identifier, id->object.bytes.data(), 16);
        } else if (id->scheme == IdentityScheme::WindowsFileIndex64) {
            descriptor.Type = FileIdType; std::memcpy(&descriptor.FileId.QuadPart, id->object.bytes.data(), 8);
        } else return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity), "layout.open_scheme"});
        // A live original handle supplies the volume/share hint and prevents ID
        // reuse. Native identity open avoids a basename-replacement race. No
        // backup privilege, elevation, name fallback or content read is used.
        OwnedHandle readable(OpenFileById(original->handle_.get(), &descriptor, rights | FILE_READ_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL));
        if (readable.get() == nullptr || readable.get() == INVALID_HANDLE_VALUE) return failure<std::unique_ptr<OpenedEntry>>(GetLastError(), "OpenFileById.layout");
        auto opened = std::make_unique<WindowsOpenedEntry>(std::move(readable), scope_, true,
                                                           requests_content_read(access));
        const auto after = opened->inspect_basic();
        if (!after.value()) { const auto& e = *after.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto checked = detail::data_layout_metadata_unchanged(*before.value(), *after.value());
        if (!checked.value()) { const auto& e = *checked.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        const auto final_guard = detail::data_layout_access_guard(*after.value(), StorageModel::WindowsHandleAttributes);
        if (!final_guard.value()) { const auto& e = *final_guard.unavailable_detail(); return Result::unavailable(e.state, e.error); }
        return Result::available(std::move(opened));
    }
    Observation<OwnedHandle> open_native_component(const NativeName& name, ACCESS_MASK rights, ChildLookupMode mode) const {
        using Result = Observation<OwnedHandle>;
        const auto* units = name.windows_units();
        if (!units || !name.is_valid() || units->find(u':') != std::u16string::npos ||
            units->size() > std::numeric_limits<USHORT>::max() / sizeof(WCHAR))
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "open_child.name"});
        // A counted, single relative component has no Win32 DOS-device aliasing
        // or trailing-dot/space rewriting. Stream syntax is refused above.
        std::wstring spelling;
        spelling.reserve(units->size());
        for (const auto unit : *units) spelling.push_back(static_cast<wchar_t>(unit));
        UNICODE_STRING native_name{};
        native_name.Length = static_cast<USHORT>(spelling.size() * sizeof(WCHAR));
        native_name.MaximumLength = native_name.Length;
        native_name.Buffer = spelling.data();
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = handle_.get();
        attributes.ObjectName = &native_name;
        // Zero delegates comparison to native defaults. This is not a
        // guarantee of byte-exact filesystem spelling or normalization.
        static_assert(OBJ_CASE_INSENSITIVE==0x40);
        attributes.Attributes=mode==ChildLookupMode::WindowsIgnoreCase?OBJ_CASE_INSENSITIVE:0;
        IO_STATUS_BLOCK io{};
        HANDLE raw = nullptr;
        const NTSTATUS status = NtCreateFile(&raw, rights, &attributes, &io, nullptr, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
            FILE_OPEN_REPARSE_POINT | FILE_OPEN_NO_RECALL | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
        OwnedHandle opened(raw);
        if (status < 0 || status == 0x00000103L)
            return Result::unavailable(status < 0 ? error_state(RtlNtStatusToDosError(status)) : ObservationState::Failed,
                {ErrorDomain::NtStatus, static_cast<std::uint32_t>(status), "NtCreateFile.child"});
        if (raw == nullptr || raw == INVALID_HANDLE_VALUE)
            return Result::unavailable(ObservationState::Failed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "NtCreateFile.child_handle"});
        return Result::available(std::move(opened));
    }
private:
    Observation<DirectoryStep> next_step() override {
        using Result = Observation<DirectoryStep>;
        static_assert(offsetof(FILE_ID_BOTH_DIR_INFO, FileName) == detail::windows_directory_name_offset);
        static_assert(offsetof(FILE_ID_BOTH_DIR_INFO, FileNameLength) == detail::windows_directory_name_length_offset);
        if (!offset_) {
            buffer_.fill(std::byte{});
            const auto info_class = started_ ? FileIdBothDirectoryInfo : FileIdBothDirectoryRestartInfo;
            if (!GetFileInformationByHandleEx(handle_.get(), info_class, buffer_.data(), static_cast<DWORD>(buffer_.size()))) {
                const DWORD code = GetLastError();
                if (code == ERROR_NO_MORE_FILES) return Result::available(DirectoryEnd{});
                return failure<DirectoryStep>(code, "FileIdBothDirectoryInfo");
            }
            started_ = true;
            offset_ = 0;
        }
        auto decoded = detail::decode_windows_directory_record(buffer_, *offset_);
        if (!decoded.value()) {
            const auto& error = *decoded.unavailable_detail();
            return Result::unavailable(error.state, error.error);
        }
        auto record = std::move(decoded).take_value();
        offset_ = record.next_offset;
        if (!record.name) return Result::available(DirectorySkip{});
        return Result::available(std::move(*record.name));
    }
    OwnedHandle handle_;
    AdapterScope scope_;
    // Fixed bound, sufficient for a full native UTF-16 component plus header.
    // A provider rejecting the query reports an error; never reopen by path.
    alignas(8) std::array<std::byte, 128 * 1024> buffer_{};
    std::optional<std::size_t> offset_;
    bool started_ = false;
};

Observation<bool> WindowsOpenedEntry::publish_noreplace(const DirectoryCursor& destination, const NativeName& name) {
    const auto* cursor = dynamic_cast<const WindowsDirectoryCursor*>(&destination);
    if (!cursor)
        return Observation<bool>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "publish_noreplace.destination_adapter"});
    return cursor->publish_noreplace(handle_.get(), name);
}

} // namespace

Observation<BasicMetadata> NativeFileSystem::inspect_basic(const std::filesystem::path& path) const {
    const auto address = NativeAbsolutePath::from_host(path);
    if (!address.value()) { const auto& e = *address.unavailable_detail(); return Observation<BasicMetadata>::unavailable(e.state, e.error); }
    const std::unique_ptr<void, HandleCloser> handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return failure<BasicMetadata>(GetLastError(), "CreateFileW.metadata");

    return handle_metadata(handle.get(), scope_);
}

Observation<std::unique_ptr<DirectoryCursor>> NativeFileSystem::open_directory(
    const std::filesystem::path& path, DirectoryLinkPolicy policy) const {
    using Result = Observation<std::unique_ptr<DirectoryCursor>>;
    const auto address = NativeAbsolutePath::from_host(path);
    if (!address.value()) { const auto& e = *address.unavailable_detail(); return Result::unavailable(e.state, e.error); }
    if (policy != DirectoryLinkPolicy::RejectFinalLink && policy != DirectoryLinkPolicy::FollowFinalLink)
        return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "open_directory"});
    const DWORD link_flag = policy == DirectoryLinkPolicy::RejectFinalLink ? FILE_FLAG_OPEN_REPARSE_POINT : 0;
    OwnedHandle handle(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_NO_RECALL | link_flag, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return failure<std::unique_ptr<DirectoryCursor>>(GetLastError(), "CreateFileW.directory");
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic)))
        return failure<std::unique_ptr<DirectoryCursor>>(GetLastError(), "FileBasicInfo.directory");
    if (policy == DirectoryLinkPolicy::RejectFinalLink && (basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::FinalLinkRejected), "open_directory.reparse"});
    if ((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotDirectory), "open_directory.kind"});
    auto native_identity = identity(handle.get(), scope_);
    return Result::available(std::make_unique<WindowsDirectoryCursor>(std::move(handle), std::move(native_identity), scope_));
}
} // namespace ck::files
