// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_index_file.hpp"
#include "../windows_name.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <limits>
#include <vector>
namespace ck::files {
namespace {
struct Close {
    void operator()(void *p) const noexcept {
        if (p && p != INVALID_HANDLE_VALUE)
            CloseHandle(p);
    }
};
using Handle = std::unique_ptr<void, Close>;
struct Free {
    void operator()(void *p) const noexcept {
        if (p)
            LocalFree(p);
    }
};
using Local = std::unique_ptr<void, Free>;
ObservationState state(DWORD code) {
    if (code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_PRIVILEGE_NOT_HELD)
        return ObservationState::Denied;
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
        return ObservationState::Gone;
    if (code == ERROR_NOT_SUPPORTED || code == ERROR_INVALID_FUNCTION || code == ERROR_CALL_NOT_IMPLEMENTED)
        return ObservationState::Unsupported;
    return ObservationState::Failed;
}
template <class T> Observation<T> failure(DWORD code, const char *op) {
    return Observation<T>::unavailable(state(code), {ErrorDomain::Win32, code, op});
}
template <class T> Observation<T> nt_failure(NTSTATUS code, const char *op) {
    return Observation<T>::unavailable(state(RtlNtStatusToDosError(code)),
                                       {ErrorDomain::NtStatus, static_cast<std::uint32_t>(code), op});
}
template <class T>
Observation<T> rejected(AdapterError code, const char *op, ObservationState s = ObservationState::Failed) {
    return Observation<T>::unavailable(s, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), op});
}
Observation<bool> verify_security(HANDLE file, PSID expected) {
    PSID owner{};
    PACL acl{};
    PSECURITY_DESCRIPTOR raw{};
    const DWORD code = GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                                       &owner, nullptr, &acl, nullptr, &raw);
    Local storage(raw);
    if (code != ERROR_SUCCESS)
        return failure<bool>(code, "index_file.GetSecurityInfo");
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    if (!GetSecurityDescriptorControl(raw, &control, &revision))
        return failure<bool>(GetLastError(), "index_file.GetSDControl");
    if (!owner || !IsValidSid(owner) || !EqualSid(owner, expected) || !acl || !IsValidAcl(acl) ||
        !(control & SE_DACL_PROTECTED))
        return rejected<bool>(AdapterError::InvalidNativeResult, "index_file.private_descriptor");
    ACL_SIZE_INFORMATION info{};
    if (!GetAclInformation(acl, &info, sizeof(info), AclSizeInformation))
        return failure<bool>(GetLastError(), "index_file.GetAclInformation");
    if (info.AceCount != 2)
        return rejected<bool>(AdapterError::InvalidNativeResult, "index_file.ace_count");
    bool user = false, system = false;
    for (DWORD i = 0; i != info.AceCount; ++i) {
        void *entry{};
        if (!GetAce(acl, i, &entry))
            return failure<bool>(GetLastError(), "index_file.GetAce");
        const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(entry);
        if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags != 0 ||
            ace->Header.AceSize < sizeof(ACCESS_ALLOWED_ACE) || ace->Mask != FILE_ALL_ACCESS)
            return rejected<bool>(AdapterError::InvalidNativeResult, "index_file.ace");
        PSID sid = const_cast<DWORD *>(&ace->SidStart);
        if (!IsValidSid(sid) || GetLengthSid(sid) > ace->Header.AceSize - offsetof(ACCESS_ALLOWED_ACE, SidStart))
            return rejected<bool>(AdapterError::InvalidNativeResult, "index_file.ace_sid");
        if (i == 0 && EqualSid(sid, expected))
            user = true;
        else if (i == 1 && IsWellKnownSid(sid, WinLocalSystemSid))
            system = true;
        else
            return rejected<bool>(AdapterError::InvalidNativeResult, "index_file.ace_principal");
    }
    return Observation<bool>::available(user && system);
}
bool valid_io(std::uint64_t offset, std::size_t count) {
    constexpr auto max = static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max());
    return offset <= max && count <= max - offset;
}
class WindowsIndexFile final : public IndexByteFile {
  public:
    explicit WindowsIndexFile(HANDLE h) noexcept : file_(h) {
    }
    Observation<std::uint64_t> size() override {
        LARGE_INTEGER n{};
        if (!GetFileSizeEx(file_.get(), &n))
            return failure<std::uint64_t>(GetLastError(), "index_file.GetFileSizeEx");
        if (n.QuadPart < 0)
            return rejected<std::uint64_t>(AdapterError::InvalidNativeResult, "index_file.size");
        return Observation<std::uint64_t>::available(static_cast<std::uint64_t>(n.QuadPart));
    }
    Observation<std::size_t> write_at(std::uint64_t offset, std::span<const std::byte> bytes) override {
        if (!valid_io(offset, bytes.size()))
            return rejected<std::size_t>(AdapterError::InvalidLimit, "index_file.write_range");
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_.get(), at, nullptr, FILE_BEGIN))
            return failure<std::size_t>(GetLastError(), "index_file.write_seek");
        DWORD wrote{};
        if (!WriteFile(file_.get(), bytes.data(),
                       static_cast<DWORD>(std::min(bytes.size(), static_cast<std::size_t>(MAXDWORD))), &wrote, nullptr))
            return failure<std::size_t>(GetLastError(), "index_file.WriteFile");
        return Observation<std::size_t>::available(wrote);
    }
    Observation<std::size_t> read_at(std::uint64_t offset, std::span<std::byte> bytes) override {
        if (!valid_io(offset, bytes.size()))
            return rejected<std::size_t>(AdapterError::InvalidLimit, "index_file.read_range");
        LARGE_INTEGER at{};
        at.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(file_.get(), at, nullptr, FILE_BEGIN))
            return failure<std::size_t>(GetLastError(), "index_file.read_seek");
        DWORD got{};
        if (!ReadFile(file_.get(), bytes.data(),
                      static_cast<DWORD>(std::min(bytes.size(), static_cast<std::size_t>(MAXDWORD))), &got, nullptr))
            return failure<std::size_t>(GetLastError(), "index_file.ReadFile");
        return Observation<std::size_t>::available(got);
    }

  private:
    Handle file_;
};
Observation<bool> verify_root(HANDLE directory) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &tag, sizeof(tag)))
        return failure<bool>(GetLastError(), "index_file.root_tag");
    if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return rejected<bool>(AdapterError::FinalLinkRejected, "index_file.root_kind");
    DWORD flags{};
    if (!GetVolumeInformationByHandleW(directory, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0))
        return failure<bool>(GetLastError(), "index_file.volume");
    if (!(flags & FILE_PERSISTENT_ACLS))
        return rejected<bool>(AdapterError::UnsupportedQuery, "index_file.persistent_acls",
                              ObservationState::Unsupported);
    return Observation<bool>::available(true);
}
Observation<std::vector<std::byte>> effective_user() {
    HANDLE raw_token{};
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
        const DWORD code = GetLastError();
        if (code != ERROR_NO_TOKEN)
            return failure<std::vector<std::byte>>(code, "index_file.thread_token");
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
            return failure<std::vector<std::byte>>(GetLastError(), "index_file.process_token");
    }
    Handle token(raw_token);
    DWORD required{};
    if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || required < sizeof(TOKEN_USER) || required > 64 * 1024)
        return rejected<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "index_file.token_size");
    std::vector<std::byte> user_data(required);
    if (!GetTokenInformation(token.get(), TokenUser, user_data.data(), required, &required))
        return failure<std::vector<std::byte>>(GetLastError(), "index_file.token_user");
    PSID user = reinterpret_cast<TOKEN_USER *>(user_data.data())->User.Sid;
    if (!IsValidSid(user))
        return rejected<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "index_file.user_sid");
    const DWORD length = GetLengthSid(user);
    if (length > SECURITY_MAX_SID_SIZE)
        return rejected<std::vector<std::byte>>(AdapterError::InvalidNativeResult, "index_file.sid_size");
    std::vector<std::byte> sid(length);
    if (!CopySid(length, sid.data(), user))
        return failure<std::vector<std::byte>>(GetLastError(), "index_file.CopySid");
    return Observation<std::vector<std::byte>>::available(std::move(sid));
}
Observation<std::unique_ptr<IndexByteFile>> create_in_root(HANDLE directory, PSID expected_user) {
    using Pointer = std::unique_ptr<IndexByteFile>;
    auto valid = verify_root(directory);
    if (!valid.value()) {
        const auto &e = *valid.unavailable_detail();
        return Observation<Pointer>::unavailable(e.state, e.error);
    }
    auto queried = effective_user();
    if (!queried.value()) {
        const auto &e = *queried.unavailable_detail();
        return Observation<Pointer>::unavailable(e.state, e.error);
    }
    auto user_data = std::move(queried).take_value();
    PSID user = user_data.data();
    if (!EqualSid(user, expected_user))
        return rejected<Pointer>(AdapterError::InvalidAccess, "index_file.user_changed", ObservationState::Denied);
    alignas(DWORD) std::array<std::byte, SECURITY_MAX_SID_SIZE> system{};
    DWORD system_size = static_cast<DWORD>(system.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &system_size))
        return failure<Pointer>(GetLastError(), "index_file.system_sid");
    const DWORD acl_size = static_cast<DWORD>(sizeof(ACL) + 2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD))) +
                           GetLengthSid(user) + system_size;
    std::vector<std::byte> acl_data(acl_size);
    PACL acl = reinterpret_cast<PACL>(acl_data.data());
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeAcl(acl, acl_size, ACL_REVISION) ||
        !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, user) ||
        !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, system.data()) ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&descriptor, user, FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return failure<Pointer>(GetLastError(), "index_file.private_descriptor_init");
    for (int attempt = 0; attempt != 64; ++attempt) {
        std::array<UCHAR, 16> entropy{};
        const NTSTATUS random_status = BCryptGenRandom(nullptr, entropy.data(), static_cast<ULONG>(entropy.size()),
                                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (random_status < 0)
            return nt_failure<Pointer>(random_status, "index_file.BCryptGenRandom");
        std::wstring name = L"ck-files-index-";
        for (auto byte : entropy) {
            name += L"0123456789abcdef"[byte >> 4];
            name += L"0123456789abcdef"[byte & 15];
        }
        UNICODE_STRING relative{};
        relative.Buffer = name.data();
        relative.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
        relative.MaximumLength = relative.Length;
        OBJECT_ATTRIBUTES attributes{};
        attributes.Length = sizeof(attributes);
        attributes.RootDirectory = directory;
        attributes.ObjectName = &relative;
        attributes.SecurityDescriptor = &descriptor;
        IO_STATUS_BLOCK result{};
        HANDLE raw{};
        const NTSTATUS status = NtCreateFile(&raw, FILE_GENERIC_READ | FILE_GENERIC_WRITE | DELETE | READ_CONTROL,
                                             &attributes, &result, nullptr, FILE_ATTRIBUTE_TEMPORARY, 0, FILE_CREATE,
                                             FILE_NON_DIRECTORY_FILE | FILE_OPEN_REPARSE_POINT |
                                                 FILE_SYNCHRONOUS_IO_NONALERT | FILE_DELETE_ON_CLOSE,
                                             nullptr, 0);
        Handle file(raw);
        if (status < 0) {
            if (static_cast<ULONG>(status) == 0xc0000035UL)
                continue; // STATUS_OBJECT_NAME_COLLISION
            return nt_failure<Pointer>(status, "index_file.NtCreateFile");
        }
        if (status != 0 || result.Information != FILE_CREATED || !file || file.get() == INVALID_HANDLE_VALUE)
            return rejected<Pointer>(AdapterError::InvalidNativeResult, "index_file.created");
        auto security = verify_security(file.get(), user);
        if (!security.value()) {
            const auto &e = *security.unavailable_detail();
            return Observation<Pointer>::unavailable(e.state, e.error);
        }
        if (!*security.value())
            return rejected<Pointer>(AdapterError::InvalidNativeResult, "index_file.security");
        FILE_STANDARD_INFO info{};
        if (!GetFileInformationByHandleEx(file.get(), FileStandardInfo, &info, sizeof(info)))
            return failure<Pointer>(GetLastError(), "index_file.created_info");
        if (info.Directory || info.EndOfFile.QuadPart != 0 || info.NumberOfLinks != 1)
            return rejected<Pointer>(AdapterError::InvalidNativeResult, "index_file.created_file");
        auto owner = std::make_unique<WindowsIndexFile>(file.get());
        file.release();
        return Observation<Pointer>::available(std::move(owner));
    }
    return rejected<Pointer>(AdapterError::InventoryLimit, "index_file.name_attempts", ObservationState::Unknown);
}
class WindowsIndexWorkspace final : public IndexFileFactory {
  public:
    explicit WindowsIndexWorkspace(HANDLE directory, std::vector<std::byte> user)
        : directory_(directory), user_(std::move(user)) {
    }
    Observation<std::unique_ptr<IndexByteFile>> create_file() override {
        return create_in_root(directory_.get(), user_.data());
    }

  private:
    Handle directory_;
    std::vector<std::byte> user_;
};
} // namespace
Observation<std::shared_ptr<IndexFileFactory>> open_native_index_workspace(const NativeAbsolutePath &root) {
    using Pointer = std::shared_ptr<IndexFileFactory>;
    auto host = root.host_path();
    if (!host.value()) {
        const auto &e = *host.unavailable_detail();
        return Observation<Pointer>::unavailable(e.state, e.error);
    }
    const auto &path = *host.value();
    const auto last = path.native().back();
    if (last == L'\\' || last == L'/' || path.filename() == L"." || path.filename() == L"..")
        return rejected<Pointer>(AdapterError::InvalidPath, "index_file.root_address");
    const auto native_path = windows_extended_path(path.native());
    if (!native_path) return rejected<Pointer>(AdapterError::InvalidPath, "index_file.root_address");
    Handle directory(CreateFileW(native_path->c_str(), FILE_ADD_FILE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (directory.get() == INVALID_HANDLE_VALUE)
        return failure<Pointer>(GetLastError(), "index_file.open_root");
    auto valid = verify_root(directory.get());
    if (!valid.value()) {
        const auto &e = *valid.unavailable_detail();
        return Observation<Pointer>::unavailable(e.state, e.error);
    }
    auto user = effective_user();
    if (!user.value()) {
        const auto &e = *user.unavailable_detail();
        return Observation<Pointer>::unavailable(e.state, e.error);
    }
    auto owner = std::make_shared<WindowsIndexWorkspace>(directory.get(), std::move(user).take_value());
    directory.release();
    return Observation<Pointer>::available(std::move(owner));
}
Observation<std::unique_ptr<IndexByteFile>> create_native_index_file(const NativeAbsolutePath &root) {
    auto workspace = open_native_index_workspace(root);
    if (!workspace.value()) {
        const auto &e = *workspace.unavailable_detail();
        return Observation<std::unique_ptr<IndexByteFile>>::unavailable(e.state, e.error);
    }
    return (*workspace.value())->create_file();
}
} // namespace ck::files
