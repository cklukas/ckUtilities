// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "windows_directory_record.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>

#include <array>
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

bool dos_device_component(std::wstring_view component) noexcept {
    if (component == L"." || component == L"..") return false;
    while (!component.empty() && (component.back() == L' ' || component.back() == L'.')) component.remove_suffix(1);
    component = component.substr(0, component.find_first_of(L".:"));
    while (!component.empty() && component.back() == L' ') component.remove_suffix(1);
    if (ascii_equal(component, L"CON") || ascii_equal(component, L"PRN") || ascii_equal(component, L"AUX") ||
        ascii_equal(component, L"NUL") || ascii_equal(component, L"CONIN$") || ascii_equal(component, L"CONOUT$")) return true;
    if (component.size() != 4 || (!ascii_equal(component.substr(0, 3), L"COM") && !ascii_equal(component.substr(0, 3), L"LPT"))) return false;
    const wchar_t number = component[3];
    return (number >= L'1' && number <= L'9') || number == L'\u00B9' || number == L'\u00B2' || number == L'\u00B3';
}

bool filesystem_components(std::wstring_view tail, bool win32_rules) noexcept {
    // Object inspection is separate from future channel inventory. A stream
    // path has its owner's file ID but different data; never conflate the two.
    if (tail.find(L':') != std::wstring_view::npos) return false;
    while (!tail.empty()) {
        const auto separator = tail.find(L'\\');
        if (win32_rules && dos_device_component(tail.substr(0, separator))) return false;
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
class WindowsOpenedEntry final : public OpenedEntry {
public:
    WindowsOpenedEntry(OwnedHandle handle, AdapterScope scope)
        : handle_(std::move(handle)), scope_(std::move(scope)) {}
    Observation<BasicMetadata> inspect_basic() const override { return handle_metadata(handle_.get(), scope_); }
private:
    OwnedHandle handle_;
    AdapterScope scope_;
};

class WindowsDirectoryCursor final : public DirectoryCursor {
public:
    WindowsDirectoryCursor(OwnedHandle handle, Observation<ObjectIdentity> native_identity, AdapterScope scope)
        : DirectoryCursor(std::move(native_identity)), handle_(std::move(handle)), scope_(std::move(scope)) {}
    Observation<std::unique_ptr<OpenedEntry>> open_child(const NativeName& name) const override {
        using Result = Observation<std::unique_ptr<OpenedEntry>>;
        const auto* units = name.windows_units();
        if (!units || !NativeName::from_windows(*units) || units->find(u':') != std::u16string::npos ||
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
        // Attributes=0 requests exact native spelling, not case-insensitive lookup.
        IO_STATUS_BLOCK io{};
        HANDLE raw = nullptr;
        const NTSTATUS status = NtCreateFile(&raw, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &io, nullptr, 0,
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
