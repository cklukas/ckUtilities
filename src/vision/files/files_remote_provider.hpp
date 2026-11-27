// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_path.hpp"

#include <cvision/core/clock.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace ck::vision {

enum class FilesProviderKind : std::uint8_t {
    Local = 0,
    OrdinaryArchive = 1,
    Sftp = 2,
    SmbMountedShare = 3,
    SmbProtocolClient = 4,
};

struct FilesProviderCapabilities final {
    FilesProviderKind kind = FilesProviderKind::Local;
    bool can_list = false;
    bool can_read = false;
    bool can_write = false;
    bool can_delete = false;
    bool can_rename = false;
    bool requires_host_key_verification = false;
    bool mounted_share = false;
    std::string transport_summary;
};

FilesProviderCapabilities provider_capabilities_for(FilesProviderKind kind);

enum class FilesHostKeyPolicy : std::uint8_t {
    // Rejects unknown or changed host keys (StrictHostKeyChecking=yes).
    StrictVerify = 0,
    // Accepts first-seen host keys, rejects changed keys (StrictHostKeyChecking=accept-new).
    AcceptNew = 1,
};

struct FilesRemoteUri final {
    bool valid = false;
    FilesProviderKind kind = FilesProviderKind::Sftp;
    std::string scheme;
    std::string username;
    bool has_secret = false;
    std::string host;
    std::uint16_t port = 0;
    std::string share; // Non-empty for SMB/UNC
    std::string path;  // Normalized '/'-separated remote path
    std::string error_message;

    // Always redacts any password/secret component; safe for UI/logs/status.
    std::string redacted_display() const;
};

// Parses sftp://, smb://, and UNC \\host\share\path URIs, rejecting control
// characters and NUL bytes and stripping any inline password from display.
FilesRemoteUri parse_files_remote_uri(std::string_view raw_uri);

// Redacts any user:password@ credentials in diagnostic/log strings.
std::string redact_remote_secrets(std::string_view text);

struct FilesRemoteEntry final {
    std::string name;
    bool is_directory = false;
    bool is_symlink = false;
    std::uint64_t size = 0;
    std::string permissions_text;
    std::string modified_text;
};

enum class FilesRemoteStatus : std::uint8_t {
    Ok = 0,
    InvalidRequest = 1,
    HostKeyRejected = 2,
    AuthFailed = 3,
    Disconnected = 4,
    UncertainCommit = 5,
    Unsupported = 6,
    Cancelled = 7,
    Failed = 8,
};

struct FilesRemoteListResult final {
    FilesRemoteStatus status = FilesRemoteStatus::Failed;
    std::vector<FilesRemoteEntry> entries;
    std::string diagnostic;
};

struct FilesRemoteTransferResult final {
    FilesRemoteStatus status = FilesRemoteStatus::Failed;
    std::uint64_t bytes_transferred = 0;
    bool commit_uncertain = false;
    std::string diagnostic;
};

// Pluggable transport seam so SFTP/SMB behavior, host-key refusal, reconnect,
// and uncertain-commit fail-closed guards can be tested deterministically.
class FilesRemoteTransport {
  public:
    virtual ~FilesRemoteTransport() = default;
    virtual FilesProviderCapabilities capabilities() const = 0;
    virtual FilesRemoteListResult list(const FilesRemoteUri &uri,
                                       FilesHostKeyPolicy host_key_policy,
                                       std::stop_token stop_token) = 0;
    virtual FilesRemoteTransferResult download(const FilesRemoteUri &remote_file,
                                               const ck::files::NativeAbsolutePath &local_dest,
                                               FilesHostKeyPolicy host_key_policy,
                                               std::stop_token stop_token) = 0;
    virtual FilesRemoteTransferResult upload(const ck::files::NativeAbsolutePath &local_src,
                                             const FilesRemoteUri &remote_dest,
                                             FilesHostKeyPolicy host_key_policy,
                                             std::stop_token stop_token) = 0;
    virtual FilesRemoteTransferResult remove_remote(const FilesRemoteUri &remote_target,
                                                    FilesHostKeyPolicy host_key_policy,
                                                    std::stop_token stop_token) = 0;
};

// Coordinates remote session state, ensuring that an interrupted destructive
// action (UncertainCommit) blocks blind retry across reconnect until reviewed.
class FilesRemoteSession final {
  public:
    explicit FilesRemoteSession(std::unique_ptr<FilesRemoteTransport> transport,
                                FilesHostKeyPolicy policy = FilesHostKeyPolicy::StrictVerify);

    FilesProviderCapabilities capabilities() const;
    FilesHostKeyPolicy host_key_policy() const noexcept { return policy_; }
    bool has_uncertain_commit() const noexcept { return uncertain_commit_uri_.has_value(); }
    const std::optional<std::string> &uncertain_commit_target() const noexcept {
        return uncertain_commit_uri_;
    }

    // Explicitly acknowledges review of an uncertain commit after inspection.
    void acknowledge_uncertain_commit() noexcept { uncertain_commit_uri_.reset(); }

    FilesRemoteListResult list(const FilesRemoteUri &uri, std::stop_token stop_token = {});
    FilesRemoteTransferResult download(const FilesRemoteUri &remote_file,
                                       const ck::files::NativeAbsolutePath &local_dest,
                                       std::stop_token stop_token = {});
    FilesRemoteTransferResult upload(const ck::files::NativeAbsolutePath &local_src,
                                     const FilesRemoteUri &remote_dest,
                                     std::stop_token stop_token = {});
    // Refuses blind retry while `has_uncertain_commit()` is true.
    FilesRemoteTransferResult remove_remote(const FilesRemoteUri &remote_target,
                                            std::stop_token stop_token = {});

  private:
    std::unique_ptr<FilesRemoteTransport> transport_;
    FilesHostKeyPolicy policy_ = FilesHostKeyPolicy::StrictVerify;
    std::optional<std::string> uncertain_commit_uri_;
};

// Creates a production SFTP transport backed by ckVision's bounded NativeProcessRunner
// with strict BatchMode=yes, ConnectTimeout=10, and StrictHostKeyChecking enforcement.
std::unique_ptr<FilesRemoteTransport> make_sftp_remote_transport(const ckv::Clock &clock);

// Creates a mounted-share SMB/UNC transport using native handle-relative filesystem I/O.
std::unique_ptr<FilesRemoteTransport> make_smb_mounted_transport();

} // namespace ck::vision
