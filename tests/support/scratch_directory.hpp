// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <cstddef>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <windows.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace ck::testing {

#if defined(_WIN32)
namespace detail {

struct HandleCloser {
    void operator()(void* value) const noexcept {
        if (value) CloseHandle(value);
    }
};

bool qualify_windows_scratch_directory(const std::filesystem::path& path) {
    HANDLE raw_token{};
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
        if (GetLastError() != ERROR_NO_TOKEN ||
            !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token))
            return false;
    }
    std::unique_ptr<void, HandleCloser> token(raw_token);
    DWORD user_size = 0;
    if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &user_size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || user_size < sizeof(TOKEN_USER) || user_size > 64 * 1024)
        return false;
    std::vector<std::byte> user_data(user_size);
    if (!GetTokenInformation(token.get(), TokenUser, user_data.data(), user_size, &user_size))
        return false;
    PSID user = reinterpret_cast<TOKEN_USER*>(user_data.data())->User.Sid;
    if (!IsValidSid(user)) return false;
    const DWORD user_size_bytes = GetLengthSid(user);
    if (user_size_bytes == 0 || user_size_bytes > SECURITY_MAX_SID_SIZE) return false;
    std::vector<std::byte> user_sid(user_size_bytes);
    if (!CopySid(user_size_bytes, user_sid.data(), user)) return false;

    std::vector<std::byte> system_sid(SECURITY_MAX_SID_SIZE);
    DWORD system_size = static_cast<DWORD>(system_sid.size());
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &system_size)) return false;
    system_sid.resize(system_size);
    const auto acl_size = static_cast<DWORD>(sizeof(ACL) +
        2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD)) +
        GetLengthSid(user_sid.data()) + GetLengthSid(system_sid.data()));
    std::vector<std::byte> acl_data(acl_size);
    PACL acl = reinterpret_cast<PACL>(acl_data.data());
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeAcl(acl, acl_size, ACL_REVISION) ||
        !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, user_sid.data()) ||
        !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, system_sid.data()) ||
        !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&descriptor, user_sid.data(), FALSE) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return false;
    const auto code = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        user_sid.data(), nullptr, acl, nullptr);
    return code == ERROR_SUCCESS;
}

} // namespace detail
#endif

// The build/driver selects this root explicitly. Never choose a host default
// or silently fall back to a different volume if the selected root is absent.
inline std::filesystem::path scratch_root() { return CKTOOLS_TEST_TEMP_ROOT; }

class ScratchDirectory final {
public:
    explicit ScratchDirectory(std::string_view prefix, std::filesystem::path root = scratch_root()) : root_(std::move(root)) {
        if (prefix.empty() || prefix.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                std::string_view::npos)
            throw std::invalid_argument("invalid scratch-directory prefix");
        std::error_code error;
        if (!root_.is_absolute() || !std::filesystem::is_directory(root_, error) || error)
            throw std::runtime_error("the selected test scratch root is unavailable");
        std::random_device entropy;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto candidate = root_ / (std::string(prefix) + "-" +
                std::to_string(entropy()) + "-" + std::to_string(entropy()));
            if (std::filesystem::create_directory(candidate, error)) {
#if defined(_WIN32)
                if (!detail::qualify_windows_scratch_directory(candidate)) {
                    std::error_code cleanup_error;
                    std::filesystem::remove_all(candidate, cleanup_error);
                    throw std::runtime_error("cannot qualify the selected Windows test scratch directory");
                }
#endif
                path_ = candidate;  // Cleanup authority only after exclusive creation.
                return;
            }
            if (error) throw std::runtime_error("cannot create the selected test scratch directory");
        }
        throw std::runtime_error("cannot choose a unique test scratch directory");
    }

    ~ScratchDirectory() {
        if (path_.empty() || path_.parent_path() != root_) return;
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;
    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path root_;
    std::filesystem::path path_;
};

}  // namespace ck::testing

#if defined(_WIN32)
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif
