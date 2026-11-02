// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "../operation_journal_file.hpp"
#include "../windows_name.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <aclapi.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace ck::files {
namespace {
struct Close {
    void operator()(void* value) const noexcept {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};
using Handle = std::unique_ptr<void, Close>;
struct LocalFreeOwner {
    void operator()(void* value) const noexcept { if (value) LocalFree(value); }
};
using Local = std::unique_ptr<void, LocalFreeOwner>;

ObservationState state(DWORD code) {
    if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_PRIVILEGE_NOT_HELD)
        return ObservationState::Denied;
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        return ObservationState::Gone;
    if (code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION || code == ERROR_CALL_NOT_IMPLEMENTED)
        return ObservationState::Unsupported;
    return ObservationState::Failed;
}
template <class T>
Observation<T> failure(DWORD code, const char* operation) {
    return Observation<T>::unavailable(state(code), {ErrorDomain::Win32, code, operation});
}
template <class T>
Observation<T> nt_failure(NTSTATUS code, const char* operation) {
    return Observation<T>::unavailable(state(RtlNtStatusToDosError(code)),
        {ErrorDomain::NtStatus, static_cast<std::int64_t>(code), operation});
}
template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

Observation<std::vector<std::byte>> effective_user() {
    HANDLE raw{};
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw)) {
        const auto code = GetLastError();
        if (code != ERROR_NO_TOKEN) return failure<std::vector<std::byte>>(code, "journal_file.thread_token");
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
            return failure<std::vector<std::byte>>(GetLastError(), "journal_file.process_token");
    }
    Handle token(raw);
    DWORD required = 0;
    if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || required < sizeof(TOKEN_USER) || required > 64 * 1024)
        return rejected<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "journal_file.token_size");
    std::vector<std::byte> data(required);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), required, &required))
        return failure<std::vector<std::byte>>(GetLastError(), "journal_file.token_user");
    const auto* user = reinterpret_cast<const TOKEN_USER*>(data.data())->User.Sid;
    if (!IsValidSid(const_cast<PSID>(user)))
        return rejected<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "journal_file.user_sid");
    const auto length = GetLengthSid(const_cast<PSID>(user));
    if (length == 0 || length > SECURITY_MAX_SID_SIZE)
        return rejected<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "journal_file.sid_length");
    std::vector<std::byte> result(length);
    if (!CopySid(length, result.data(), const_cast<PSID>(user)))
        return failure<std::vector<std::byte>>(GetLastError(), "journal_file.CopySid");
    return Observation<std::vector<std::byte>>::available(std::move(result));
}

Observation<bool> verify_security(HANDLE file, PSID expected) {
    PSID owner{}; PACL acl{}; PSECURITY_DESCRIPTOR raw{};
    const auto code = GetSecurityInfo(file, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &raw);
    Local descriptor(raw);
    if (code != ERROR_SUCCESS) return failure<bool>(code, "journal_file.GetSecurityInfo");
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
    if (!GetSecurityDescriptorControl(raw, &control, &revision))
        return failure<bool>(GetLastError(), "journal_file.GetSecurityDescriptorControl");
    if (!owner || !IsValidSid(owner) || !EqualSid(owner, expected)) {
        return rejected<bool>(AdapterError::InvalidNativeResult, "journal_file.private_descriptor.owner");
    }
    if (!acl || !IsValidAcl(acl))
        return rejected<bool>(AdapterError::InvalidNativeResult, "journal_file.private_descriptor.dacl");
    if (!(control & SE_DACL_PROTECTED))
        return rejected<bool>(AdapterError::InvalidNativeResult, "journal_file.private_descriptor.protection");
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(acl, &info, sizeof(info), AclSizeInformation))
        return failure<bool>(GetLastError(), "journal_file.GetAclInformation");
    if (info.AceCount != 2) return rejected<bool>(AdapterError::InvalidNativeResult, "journal_file.private_descriptor.ace_count");
    bool user = false, system = false;
    for (DWORD index = 0; index != info.AceCount; ++index) {
        void* raw_ace{};
        if (!GetAce(acl, index, &raw_ace)) return failure<bool>(GetLastError(), "journal_file.GetAce");
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0 ||
            ace->Header.AceSize < sizeof(ACCESS_ALLOWED_ACE) || ace->Mask != FILE_ALL_ACCESS)
            return rejected<bool>(AdapterError::InvalidNativeResult, "journal_file.private_descriptor.ace");
        auto* sid = reinterpret_cast<PSID>(const_cast<SID*>(reinterpret_cast<const SID*>(&ace->SidStart)));
        if (!IsValidSid(sid) || GetLengthSid(sid) > ace->Header.AceSize - offsetof(ACCESS_ALLOWED_ACE, SidStart))
            return rejected<bool>(AdapterError::InvalidNativeResult, "journal_file.private_descriptor.ace_sid");
        if (index == 0 && EqualSid(sid, expected)) user = true;
        else if (index == 1 && IsWellKnownSid(sid, WinLocalSystemSid)) system = true;
        else return rejected<bool>(AdapterError::InvalidNativeResult, "journal_file.private_descriptor.ace_principal");
    }
    return Observation<bool>::available(user && system);
}

Observation<bool> verify_root(HANDLE directory, PSID expected) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &tag, sizeof(tag)))
        return failure<bool>(GetLastError(), "journal_file.root_tag");
    if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return rejected<bool>(AdapterError::FinalLinkRejected, "journal_file.root_kind");
    DWORD flags{};
    if (!GetVolumeInformationByHandleW(directory, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
        return failure<bool>(GetLastError(), "journal_file.volume");
    if (!(flags & FILE_PERSISTENT_ACLS))
        return rejected<bool>(AdapterError::UnsupportedQuery, "journal_file.persistent_acls", ObservationState::Unsupported);
    return verify_security(directory, expected);
}

Observation<SECURITY_ATTRIBUTES> make_security(std::vector<std::byte>& user,
                                               std::vector<std::byte>& system,
                                               std::vector<std::byte>& acl_data,
                                               SECURITY_DESCRIPTOR& descriptor) {
    DWORD system_size = SECURITY_MAX_SID_SIZE;
    system.resize(system_size);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &system_size))
        return failure<SECURITY_ATTRIBUTES>(GetLastError(), "journal_file.system_sid");
    system.resize(system_size);
    const auto acl_size = static_cast<DWORD>(sizeof(ACL) +
        2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
        GetLengthSid(user.data()) + GetLengthSid(system.data()));
    acl_data.resize(acl_size);
    auto* acl = reinterpret_cast<PACL>(acl_data.data());
    if (!InitializeAcl(acl, acl_size, ACL_REVISION) ||
        !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, user.data()) ||
        !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, system.data()) ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&descriptor, user.data(), FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return failure<SECURITY_ATTRIBUTES>(GetLastError(), "journal_file.security_descriptor");
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = &descriptor;
    return Observation<SECURITY_ATTRIBUTES>::available(attributes);
}

bool valid_io(std::uint64_t offset, std::size_t count) {
    constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max());
    return offset <= max && count <= max - offset;
}

class WindowsOperationJournalFile final : public OperationJournalFile {
public:
    explicit WindowsOperationJournalFile(HANDLE file) noexcept : file_(file) {}
    Observation<std::uint64_t> size() override {
        LARGE_INTEGER value{};
        if (!GetFileSizeEx(file_.get(), &value)) return failure<std::uint64_t>(GetLastError(), "journal_file.GetFileSizeEx");
        if (value.QuadPart < 0) return rejected<std::uint64_t>(AdapterError::InvalidNativeResult, "journal_file.size");
        return Observation<std::uint64_t>::available(static_cast<std::uint64_t>(value.QuadPart));
    }
    Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> bytes) override {
        if (!valid_io(offset, bytes.size())) return rejected<std::size_t>(AdapterError::InvalidLimit, "journal_file.read_range");
        LARGE_INTEGER at{}; at.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_.get(), at, nullptr, FILE_BEGIN)) return failure<std::size_t>(GetLastError(), "journal_file.read_seek");
        DWORD count{};
        if (!ReadFile(file_.get(), bytes.data(), static_cast<DWORD>(std::min(bytes.size(), static_cast<std::size_t>(MAXDWORD))), &count, nullptr))
            return failure<std::size_t>(GetLastError(), "journal_file.ReadFile");
        return Observation<std::size_t>::available(count);
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        if (!valid_io(offset, bytes.size())) return rejected<std::size_t>(AdapterError::InvalidLimit, "journal_file.write_range");
        LARGE_INTEGER at{}; at.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_.get(), at, nullptr, FILE_BEGIN)) return failure<std::size_t>(GetLastError(), "journal_file.write_seek");
        DWORD count{};
        if (!WriteFile(file_.get(), bytes.data(), static_cast<DWORD>(std::min(bytes.size(), static_cast<std::size_t>(MAXDWORD))), &count, nullptr))
            return failure<std::size_t>(GetLastError(), "journal_file.WriteFile");
        return Observation<std::size_t>::available(count);
    }
    Observation<bool> flush() override {
        if (!FlushFileBuffers(file_.get())) return failure<bool>(GetLastError(), "journal_file.FlushFileBuffers");
        return Observation<bool>::available(true);
    }
private:
    Handle file_;
};
}

Observation<bool> prepare_native_operation_journal_root(const NativeAbsolutePath& private_root) {
    using Result = Observation<bool>;
    auto host = private_root.host_path();
    if (!host.value())
        return Result::unavailable(host.unavailable_detail()->state, host.unavailable_detail()->error);
    auto user = effective_user();
    if (!user.value())
        return Result::unavailable(user.unavailable_detail()->state, user.unavailable_detail()->error);
    auto user_data = std::move(user).take_value();
    const auto native_path = windows_extended_path(host.value()->native());
    if (!native_path) return rejected<bool>(AdapterError::InvalidPath, "journal_file.prepare_root.path");
    Handle directory(CreateFileW(native_path->c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL |
            WRITE_DAC | WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (directory.get() == INVALID_HANDLE_VALUE)
        return failure<bool>(GetLastError(), "journal_file.prepare_root.open");
    std::vector<std::byte> system, acl_data;
    SECURITY_DESCRIPTOR descriptor{};
    auto security = make_security(user_data, system, acl_data, descriptor);
    if (!security.value())
        return Result::unavailable(security.unavailable_detail()->state, security.unavailable_detail()->error);
    BOOL present = FALSE, defaulted = FALSE;
    PACL acl = nullptr;
    if (!GetSecurityDescriptorDacl(&descriptor, &present, &acl, &defaulted) || !present || !acl)
        return rejected<bool>(AdapterError::InvalidNativeResult, "journal_file.prepare_root.dacl");
    const auto code = SetSecurityInfo(directory.get(), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        user_data.data(), nullptr, acl, nullptr);
    if (code != ERROR_SUCCESS)
        return failure<bool>(code, "journal_file.prepare_root.security");
    return verify_root(directory.get(), user_data.data());
}

Observation<std::unique_ptr<OperationJournalFile>> open_native_operation_journal_file(
    const NativeAbsolutePath& private_root, const NativeName& basename, bool create_new) {
    using Pointer = std::unique_ptr<OperationJournalFile>;
    if (!basename.is_valid() || !basename.windows_units())
        return rejected<Pointer>(AdapterError::InvalidIdentifier, "journal_file.basename");
    auto host = private_root.host_path();
    if (!host.value()) {
        const auto& detail = *host.unavailable_detail();
        return Observation<Pointer>::unavailable(detail.state, detail.error);
    }
    const auto& root = *host.value();
    if (root.native().empty() || root.filename() == L"." || root.filename() == L"..")
        return rejected<Pointer>(AdapterError::InvalidPath, "journal_file.root_address");
    auto user = effective_user();
    if (!user.value()) {
        const auto& detail = *user.unavailable_detail();
        return Observation<Pointer>::unavailable(detail.state, detail.error);
    }
    auto user_data = std::move(user).take_value();
    const auto native_path = windows_extended_path(root.native());
    if (!native_path) return rejected<Pointer>(AdapterError::InvalidPath, "journal_file.root_address");
    Handle directory(CreateFileW(native_path->c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (directory.get() == INVALID_HANDLE_VALUE) return failure<Pointer>(GetLastError(), "journal_file.open_root");
    auto root_valid = verify_root(directory.get(), user_data.data());
    if (!root_valid.value()) {
        const auto& detail = *root_valid.unavailable_detail();
        return Observation<Pointer>::unavailable(detail.state, detail.error);
    }
    std::wstring name;
    name.reserve(basename.windows_units()->size());
    for (const auto unit : *basename.windows_units()) name.push_back(static_cast<wchar_t>(unit));
    std::vector<std::byte> system, acl_data;
    std::vector<std::byte> relative_descriptor;
    SECURITY_DESCRIPTOR descriptor{};
    if (create_new) {
        auto security = make_security(user_data, system, acl_data, descriptor);
        if (!security.value()) {
            const auto& detail = *security.unavailable_detail();
            return Observation<Pointer>::unavailable(detail.state, detail.error);
        }
        DWORD relative_size = 0;
        if (MakeSelfRelativeSD(&descriptor, nullptr, &relative_size))
            return rejected<Pointer>(AdapterError::InvalidNativeResult, "journal_file.relative_descriptor_size");
        const DWORD size_error = GetLastError();
        if (size_error != ERROR_INSUFFICIENT_BUFFER || relative_size == 0)
            return failure<Pointer>(size_error, "journal_file.relative_descriptor_size");
        relative_descriptor.resize(relative_size);
        if (!MakeSelfRelativeSD(&descriptor,
                reinterpret_cast<PSECURITY_DESCRIPTOR>(relative_descriptor.data()), &relative_size))
            return failure<Pointer>(GetLastError(), "journal_file.relative_descriptor");
    }
    UNICODE_STRING relative{};
    relative.Buffer = name.data();
    relative.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    relative.MaximumLength = relative.Length;
    OBJECT_ATTRIBUTES object{};
    object.Length = sizeof(object);
    object.RootDirectory = directory.get();
    object.ObjectName = &relative;
    object.SecurityDescriptor = create_new ? relative_descriptor.data() : nullptr;
    IO_STATUS_BLOCK io{};
    HANDLE raw_file{};
    const auto status = NtCreateFile(&raw_file, FILE_GENERIC_READ | FILE_GENERIC_WRITE | READ_CONTROL | WRITE_OWNER,
        &object, &io, nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE,
        create_new ? FILE_CREATE : FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
        nullptr, 0);
    Handle file(raw_file);
    if (status < 0) return nt_failure<Pointer>(status, "journal_file.NtCreateFile");
    const auto expected = create_new ? FILE_CREATED : FILE_OPENED;
    if (io.Information != expected || file.get() == INVALID_HANDLE_VALUE)
        return rejected<Pointer>(AdapterError::InvalidNativeResult, "journal_file.open_result");
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO info{};
    if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
        !GetFileInformationByHandleEx(file.get(), FileStandardInfo, &info, sizeof(info)))
        return failure<Pointer>(GetLastError(), "journal_file.file_info");
    if ((tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.NumberOfLinks != 1 ||
        (create_new && info.EndOfFile.QuadPart != 0))
        return rejected<Pointer>(AdapterError::InvalidAccess, "journal_file.file_kind", ObservationState::Denied);
    if (create_new) {
        const auto owner_code = SetSecurityInfo(file.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
            user_data.data(), nullptr, nullptr, nullptr);
        if (owner_code != ERROR_SUCCESS) return failure<Pointer>(owner_code, "journal_file.set_owner");
    }
    auto security = verify_security(file.get(), user_data.data());
    if (!security.value()) {
        const auto& detail = *security.unavailable_detail();
        return Observation<Pointer>::unavailable(detail.state, detail.error);
    }
    if (!*security.value()) return rejected<Pointer>(AdapterError::InvalidNativeResult, "journal_file.file_security");
    return Observation<Pointer>::available(std::make_unique<WindowsOperationJournalFile>(file.release()));
}
} // namespace ck::files
