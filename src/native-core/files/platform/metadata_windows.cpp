// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "windows_name.hpp"
#include "windows_directory_record.hpp"
#include "windows_stream_records.hpp"

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

wchar_t ascii_upper(wchar_t unit) noexcept {
    return unit >= L'a' && unit <= L'z' ? static_cast<wchar_t>(unit - L'a' + L'A') : unit;
}

bool ascii_equal(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (ascii_upper(left[index]) != ascii_upper(right[index])) return false;
    return true;
}


bool filesystem_components(std::wstring_view tail, bool win32_rules) noexcept {
    // Object inspection is separate from future channel inventory. A stream
    // path has its owner's file ID but different data; never conflate the two.
    if (tail.find(L':') != std::wstring_view::npos) return false;
    while (!tail.empty()) {
        const auto separator = tail.find(L'\\');
        if (win32_rules && detail::win32_device_component(tail.substr(0, separator))) return false;
        if (separator == std::wstring_view::npos) break;
        tail.remove_prefix(separator + 1);
    }
    return true;
}

bool drive_path(std::wstring_view path, bool win32_rules) noexcept {
    if (path.size() < 3 || ascii_upper(path[0]) < L'A' || ascii_upper(path[0]) > L'Z' || path[1] != L':' || path[2] != L'\\') return false;
    return filesystem_components(path.substr(3), win32_rules);
}

bool unc_path(std::wstring_view tail, bool win32_rules) noexcept {
    const auto server_end = tail.find(L'\\');
    if (server_end == 0 || server_end == std::wstring_view::npos) return false;
    const auto server = tail.substr(0, server_end);
    if (server == L"." || server == L".." || server == L"?") return false;
    tail.remove_prefix(server_end + 1);
    const auto share_end = tail.find(L'\\');
    auto share = tail.substr(0, share_end);
    while (!share.empty() && (share.back() == L' ' || share.back() == L'.')) share.remove_suffix(1);
    if (share.empty() || ascii_equal(share, L"PIPE") || ascii_equal(share, L"IPC$")) return false;
    return share_end == std::wstring_view::npos || filesystem_components(tail.substr(share_end + 1), win32_rules);
}

bool volume_guid_path(std::wstring_view tail) noexcept {
    if (tail.size() < 45 || !ascii_equal(tail.substr(0, 7), L"Volume{") || tail[43] != L'}' || tail[44] != L'\\') return false;
    for (std::size_t index = 0; index < 36; ++index) {
        const wchar_t unit = ascii_upper(tail[7 + index]);
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (unit != L'-') return false;
        } else if (!((unit >= L'0' && unit <= L'9') || (unit >= L'A' && unit <= L'F'))) return false;
    }
    return filesystem_components(tail.substr(45), false);
}

// A metadata query must not connect to a named pipe or open a console/device
// merely because Win32 accepts that spelling as an absolute path. Validate a
// filesystem namespace before opening anything. Normalize separators only in
// this validation copy; the actual native path bytes/units remain untouched.
bool filesystem_path(const std::filesystem::path& path) {
    std::wstring spelling = path.native();
    for (auto& unit : spelling) if (unit == L'/') unit = L'\\';
    std::wstring_view text(spelling);
    if (text.starts_with(L"\\\\?\\")) {
        text.remove_prefix(4);
        if (drive_path(text, false) || volume_guid_path(text)) return true;
        return text.size() >= 4 && ascii_equal(text.substr(0, 4), L"UNC\\") && unc_path(text.substr(4), false);
    }
    if (drive_path(text, true)) return true;
    return text.starts_with(L"\\\\") && unc_path(text.substr(2), true);
}

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
    WindowsOpenedEntry(OwnedHandle handle, AdapterScope scope)
        : handle_(std::move(handle)), scope_(std::move(scope)) {}
    Observation<BasicMetadata> inspect_basic() const override { return handle_metadata(handle_.get(), scope_); }
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
};

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
    Observation<LocationInfo> inspect_location() const override {
        LocationInfo result;
        result.family = LocationQueryFamily::WindowsHandle;
        result.directory = ck::files::identity(handle_.get(), scope_);
        const auto not_applicable = [](const char* operation) {
            return Observation<std::uint64_t>::unavailable(ObservationState::Unsupported,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), operation});
        };
        result.filesystem_type_code = not_applicable("volume.filesystem_type_code");
        result.filesystem_query_id = Observation<NativeIdentifier>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::NotApplicable), "volume.filesystem_query_id"});
        result.posix_name_max = result.posix_name_chars_max = result.posix_path_max = not_applicable("volume.posix_limits");
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
        using Result = Observation<std::unique_ptr<OpenedEntry>>;
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
        const auto rights = FILE_READ_ATTRIBUTES | SYNCHRONIZE | (requests_attributes(access) ? FILE_READ_EA : 0) |
            (requests_security(access) ? READ_CONTROL : 0) | (requests_audit_security(access) ? ACCESS_SYSTEM_SECURITY : 0);
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
        return Result::available(std::make_unique<WindowsOpenedEntry>(std::move(opened), scope_));
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

} // namespace

Observation<BasicMetadata> NativeFileSystem::inspect_basic(const std::filesystem::path& path) const {
    if (!path.is_absolute() || path.native().find(L'\0') != std::wstring::npos || !filesystem_path(path))
        return Observation<BasicMetadata>::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidPath), "inspect_basic"});
    const std::unique_ptr<void, HandleCloser> handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return failure<BasicMetadata>(GetLastError(), "CreateFileW.metadata");

    return handle_metadata(handle.get(), scope_);
}

Observation<std::unique_ptr<DirectoryCursor>> NativeFileSystem::open_directory(
    const std::filesystem::path& path, DirectoryLinkPolicy policy) const {
    using Result = Observation<std::unique_ptr<DirectoryCursor>>;
    if (!path.is_absolute() || path.native().find(L'\0') != std::wstring::npos || !filesystem_path(path) ||
        (policy != DirectoryLinkPolicy::RejectFinalLink && policy != DirectoryLinkPolicy::FollowFinalLink))
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
