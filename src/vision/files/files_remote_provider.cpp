// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_remote_provider.hpp"

#include "directory_cursor.hpp"
#include "files_native_io_helpers.hpp"
#include "native_filesystem.hpp"
#include "opened_entry.hpp"

#include <cvision/term/process_runner.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <utility>

namespace ck::vision {
namespace {

bool has_forbidden_control_chars(std::string_view sv) noexcept {
    for (const char ch : sv) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u == 0 || u < 0x20U || u == 0x7FU)
            return true;
    }
    return false;
}

std::string ascii_lower(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (const unsigned char ch : sv) {
        if (ch >= 'A' && ch <= 'Z')
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        else
            out.push_back(static_cast<char>(ch));
    }
    return out;
}

bool valid_hostname_or_ip(std::string_view host) noexcept {
    if (host.empty() || host.size() > 253 || host.front() == '-')
        return false;
    for (const char ch : host) {
        const unsigned char u = static_cast<unsigned char>(ch);
        const bool ok = std::isalnum(u) || ch == '.' || ch == '-' || ch == ':' || ch == '[' || ch == ']';
        if (!ok)
            return false;
    }
    return true;
}

std::string sftp_executable_path() {
#if defined(_WIN32)
    return "C:\\Windows\\System32\\OpenSSH\\sftp.exe";
#else
    return "/usr/bin/sftp";
#endif
}

std::string sftp_quote_path(std::string_view p) {
    // Prefix relative paths starting with '-' with "./" so sftp never interprets them as options.
    std::string normalized(p);
    if (!normalized.empty() && normalized.front() == '-')
        normalized.insert(0, "./");
    std::string out = "\"";
    for (const char ch : normalized) {
        if (ch == '"' || ch == '\\')
            out.push_back('\\');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

bool parse_port_strict(std::string_view port_sv, std::uint16_t &port_out) noexcept {
    if (port_sv.empty() || port_sv.size() > 5)
        return false;
    std::uint32_t val = 0;
    for (const char ch : port_sv) {
        if (ch < '0' || ch > '9')
            return false;
        val = val * 10U + static_cast<std::uint32_t>(ch - '0');
    }
    if (val == 0 || val > 65535U)
        return false;
    port_out = static_cast<std::uint16_t>(val);
    return true;
}

FilesRemoteStatus classify_sftp_failure(std::string_view stderr_text) {
    const std::string lower = ascii_lower(stderr_text);
    if (lower.find("host key verification failed") != std::string::npos ||
        lower.find("offending") != std::string::npos ||
        lower.find("known_hosts") != std::string::npos)
        return FilesRemoteStatus::HostKeyRejected;
    if (lower.find("permission denied") != std::string::npos ||
        lower.find("authentication failed") != std::string::npos)
        return FilesRemoteStatus::AuthFailed;
    if (lower.find("connection closed") != std::string::npos ||
        lower.find("broken pipe") != std::string::npos ||
        lower.find("timed out") != std::string::npos ||
        lower.find("connection reset") != std::string::npos)
        return FilesRemoteStatus::Disconnected;
    return FilesRemoteStatus::Failed;
}

std::vector<FilesRemoteEntry> parse_sftp_ls_output(std::string_view stdout_text) {
    std::vector<FilesRemoteEntry> entries;
    std::istringstream iss{std::string(stdout_text)};
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() < 10)
            continue;
        const char type_ch = line[0];
        if (type_ch != '-' && type_ch != 'd' && type_ch != 'l')
            continue;
        // Parse standard POSIX `ls -l` columns: perms links owner group size mon day time/yr name...
        std::istringstream ls(line);
        std::string perms, links, owner, group, size_str, mon, day, time_yr;
        if (!(ls >> perms >> links >> owner >> group >> size_str >> mon >> day >> time_yr))
            continue;
        std::string rest;
        std::getline(ls, rest);
        std::size_t first = rest.find_first_not_of(" \t");
        if (first == std::string::npos)
            continue;
        std::string name = rest.substr(first);
        if (name == "." || name == "..")
            continue;
        FilesRemoteEntry entry;
        entry.name = std::move(name);
        entry.is_directory = (type_ch == 'd');
        entry.is_symlink = (type_ch == 'l');
        entry.permissions_text = std::move(perms);
        entry.modified_text = mon + " " + day + " " + time_yr;
        try {
            entry.size = std::stoull(size_str);
        } catch (...) {
            entry.size = 0;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

class SftpProcessTransport final : public FilesRemoteTransport {
  public:
    explicit SftpProcessTransport(const ckv::Clock &clock) noexcept : clock_(clock) {}

    FilesProviderCapabilities capabilities() const override {
        return provider_capabilities_for(FilesProviderKind::Sftp);
    }

    FilesRemoteListResult list(const FilesRemoteUri &uri,
                               FilesHostKeyPolicy host_key_policy,
                               std::stop_token stop_token) override {
        FilesRemoteListResult res;
        if (!uri.valid || uri.kind != FilesProviderKind::Sftp) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Invalid SFTP URI.";
            return res;
        }
        if (stop_token.stop_requested()) {
            res.status = FilesRemoteStatus::Cancelled;
            res.diagnostic = "Cancelled.";
            return res;
        }
        const std::string target_path = uri.path.empty() ? "." : uri.path;
        const std::string batch = "ls -la " + sftp_quote_path(target_path) + "\n";
        auto run = run_sftp_batch(uri, host_key_policy, batch);
        if (!run.successful()) {
            res.status = classify_sftp_failure(run.stderr_capture.bytes);
            res.diagnostic = redact_remote_secrets(
                run.stderr_capture.bytes.empty() ? run.diagnostic : run.stderr_capture.bytes);
            return res;
        }
        res.entries = parse_sftp_ls_output(run.stdout_capture.bytes);
        res.status = FilesRemoteStatus::Ok;
        return res;
    }

    FilesRemoteTransferResult download(const FilesRemoteUri &remote_file,
                                       const ck::files::NativeAbsolutePath &local_dest,
                                       FilesHostKeyPolicy host_key_policy,
                                       std::stop_token stop_token) override {
        FilesRemoteTransferResult res;
        if (!remote_file.valid || !local_dest.is_valid()) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Invalid download arguments.";
            return res;
        }
        if (stop_token.stop_requested()) {
            res.status = FilesRemoteStatus::Cancelled;
            return res;
        }
        auto local_utf8 = detail::host_path_utf8(local_dest);
        if (!local_utf8) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Unable to represent local destination path.";
            return res;
        }
        const std::string batch = "get " + sftp_quote_path(remote_file.path) + " " +
                                  sftp_quote_path(*local_utf8) + "\n";
        auto run = run_sftp_batch(remote_file, host_key_policy, batch);
        if (!run.successful()) {
            res.status = classify_sftp_failure(run.stderr_capture.bytes);
            res.diagnostic = redact_remote_secrets(
                run.stderr_capture.bytes.empty() ? run.diagnostic : run.stderr_capture.bytes);
            return res;
        }
        res.status = FilesRemoteStatus::Ok;
        return res;
    }

    FilesRemoteTransferResult upload(const ck::files::NativeAbsolutePath &local_src,
                                     const FilesRemoteUri &remote_dest,
                                     FilesHostKeyPolicy host_key_policy,
                                     std::stop_token stop_token) override {
        FilesRemoteTransferResult res;
        if (!remote_dest.valid || !local_src.is_valid()) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Invalid upload arguments.";
            return res;
        }
        if (stop_token.stop_requested()) {
            res.status = FilesRemoteStatus::Cancelled;
            return res;
        }
        auto local_utf8 = detail::host_path_utf8(local_src);
        if (!local_utf8) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Unable to represent local source path.";
            return res;
        }
        const std::string batch = "put " + sftp_quote_path(*local_utf8) + " " +
                                  sftp_quote_path(remote_dest.path) + "\n";
        auto run = run_sftp_batch(remote_dest, host_key_policy, batch);
        if (!run.successful()) {
            res.status = classify_sftp_failure(run.stderr_capture.bytes);
            if (res.status == FilesRemoteStatus::Disconnected) {
                res.status = FilesRemoteStatus::UncertainCommit;
                res.commit_uncertain = true;
            }
            res.diagnostic = redact_remote_secrets(
                run.stderr_capture.bytes.empty() ? run.diagnostic : run.stderr_capture.bytes);
            return res;
        }
        res.status = FilesRemoteStatus::Ok;
        return res;
    }

    FilesRemoteTransferResult remove_remote(const FilesRemoteUri &remote_target,
                                            FilesHostKeyPolicy host_key_policy,
                                            std::stop_token stop_token) override {
        FilesRemoteTransferResult res;
        if (!remote_target.valid) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Invalid remote delete target.";
            return res;
        }
        if (stop_token.stop_requested()) {
            res.status = FilesRemoteStatus::Cancelled;
            return res;
        }
        const std::string batch = "rm " + sftp_quote_path(remote_target.path) + "\n";
        auto run = run_sftp_batch(remote_target, host_key_policy, batch);
        if (!run.successful()) {
            res.status = classify_sftp_failure(run.stderr_capture.bytes);
            if (res.status == FilesRemoteStatus::Disconnected) {
                res.status = FilesRemoteStatus::UncertainCommit;
                res.commit_uncertain = true;
            }
            res.diagnostic = redact_remote_secrets(
                run.stderr_capture.bytes.empty() ? run.diagnostic : run.stderr_capture.bytes);
            return res;
        }
        res.status = FilesRemoteStatus::Ok;
        return res;
    }

  private:
    ckv::core::ProcessRunResult run_sftp_batch(const FilesRemoteUri &uri,
                                               FilesHostKeyPolicy policy,
                                               std::string_view batch_input) {
        std::vector<std::string> args;
        args.push_back("-b");
        args.push_back("-");
        args.push_back("-o");
        args.push_back("BatchMode=yes");
        args.push_back("-o");
        args.push_back("ConnectTimeout=10");
        args.push_back("-o");
        args.push_back("ServerAliveInterval=15");
        args.push_back("-o");
        args.push_back("ServerAliveCountMax=3");
        args.push_back("-o");
        args.push_back(policy == FilesHostKeyPolicy::AcceptNew
                           ? "StrictHostKeyChecking=accept-new"
                           : "StrictHostKeyChecking=yes");
        if (uri.port != 0) {
            args.push_back("-P");
            args.push_back(std::to_string(uri.port));
        }
        args.push_back("--");
        const std::string target = uri.username.empty()
            ? uri.host
            : (uri.username + "@" + uri.host);
        args.push_back(target);

        ckv::core::ProcessRunRequest req;
        req.launch = ckv::core::ProcessLaunchSpec::program(sftp_executable_path(), std::move(args));
#if defined(_WIN32)
        req.launch.working_directory = "C:\\";
#else
        req.launch.working_directory = "/";
#endif
        req.input = batch_input;
        req.descendants = ckv::core::ProcessDescendantPolicy::TerminateOnCompletion;
        req.idle_budget_nanos = 15'000'000'000LL;
        req.max_stdout_bytes = 512 * 1024;
        req.max_stderr_bytes = 64 * 1024;

        ckv::term::NativeProcessRunner runner(clock_);
        return runner.run(req);
    }

    const ckv::Clock &clock_;
};

class SmbMountedShareTransport final : public FilesRemoteTransport {
  public:
    FilesProviderCapabilities capabilities() const override {
        return provider_capabilities_for(FilesProviderKind::SmbMountedShare);
    }

    FilesRemoteListResult list(const FilesRemoteUri &uri,
                               FilesHostKeyPolicy /*host_key_policy*/,
                               std::stop_token stop_token) override {
        FilesRemoteListResult res;
#if defined(_WIN32)
        if (!uri.valid || uri.host.empty() || uri.share.empty()) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Invalid SMB/UNC share URI.";
            return res;
        }
        std::string unc = "\\\\" + uri.host + "\\" + uri.share;
        if (!uri.path.empty() && uri.path != "/") {
            for (const char ch : uri.path)
                unc.push_back(ch == '/' ? '\\' : ch);
        }
        auto unc_u16 = detail::utf8_to_u16(unc);
        if (!unc_u16) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Invalid UTF-8 in UNC share path.";
            return res;
        }
        auto native_obs = ck::files::NativeAbsolutePath::from_host(std::filesystem::path(*unc_u16));
        if (!native_obs.value()) {
            res.status = FilesRemoteStatus::InvalidRequest;
            res.diagnostic = "Unable to form UNC native path.";
            return res;
        }
        ck::files::NativeFileSystem fs;
        auto dir = fs.open_native_directory(*native_obs.value());
        if (!dir.value()) {
            res.status = FilesRemoteStatus::Failed;
            res.diagnostic = "Unable to open mounted SMB/UNC share.";
            return res;
        }
        for (;;) {
            auto batch = (*dir.value())->read(256, stop_token);
            if (batch.state == ck::files::DirectoryReadState::Cancelled || stop_token.stop_requested()) {
                res.status = FilesRemoteStatus::Cancelled;
                return res;
            }
            if (batch.error)
                break;
            for (const auto &name : batch.names) {
                FilesRemoteEntry entry;
                entry.name = name.display_name();
                auto child = (*dir.value())->open_child(name, ck::files::EntryAccess::MetadataOnly);
                if (child.value()) {
                    auto basic = (*child.value())->inspect_basic();
                    if (basic.value() && basic.value()->kind.value()) {
                        entry.is_directory = (*basic.value()->kind.value() == ck::files::ObjectKind::Directory);
                        entry.is_symlink = (*basic.value()->kind.value() == ck::files::ObjectKind::SymbolicLink);
                        entry.size = basic.value()->logical_size.value()
                            ? *basic.value()->logical_size.value() : 0;
                    }
                }
                res.entries.push_back(std::move(entry));
            }
            if (batch.state == ck::files::DirectoryReadState::End)
                break;
        }
        res.status = FilesRemoteStatus::Ok;
        return res;
#else
        (void)uri;
        (void)stop_token;
        res.status = FilesRemoteStatus::Unsupported;
        res.diagnostic = "Direct unmounted SMB protocol browsing is not enabled on POSIX; mount the share via OS SMBFS and browse as a mounted volume.";
        return res;
#endif
    }

    FilesRemoteTransferResult download(const FilesRemoteUri & /*remote_file*/,
                                       const ck::files::NativeAbsolutePath & /*local_dest*/,
                                       FilesHostKeyPolicy /*host_key_policy*/,
                                       std::stop_token /*stop_token*/) override {
        FilesRemoteTransferResult res;
        res.status = FilesRemoteStatus::Unsupported;
        res.diagnostic = "Use local pane copy/move operations for mounted SMB shares.";
        return res;
    }

    FilesRemoteTransferResult upload(const ck::files::NativeAbsolutePath & /*local_src*/,
                                     const FilesRemoteUri & /*remote_dest*/,
                                     FilesHostKeyPolicy /*host_key_policy*/,
                                     std::stop_token /*stop_token*/) override {
        FilesRemoteTransferResult res;
        res.status = FilesRemoteStatus::Unsupported;
        res.diagnostic = "Use local pane copy/move operations for mounted SMB shares.";
        return res;
    }

    FilesRemoteTransferResult remove_remote(const FilesRemoteUri & /*remote_target*/,
                                            FilesHostKeyPolicy /*host_key_policy*/,
                                            std::stop_token /*stop_token*/) override {
        FilesRemoteTransferResult res;
        res.status = FilesRemoteStatus::Unsupported;
        res.diagnostic = "Use local pane delete/Trash operations for mounted SMB shares.";
        return res;
    }
};

} // namespace

FilesProviderCapabilities provider_capabilities_for(FilesProviderKind kind) {
    FilesProviderCapabilities caps;
    caps.kind = kind;
    switch (kind) {
        case FilesProviderKind::Local:
            caps.can_list = caps.can_read = caps.can_write = caps.can_delete = caps.can_rename = true;
            caps.transport_summary = "Native OS handle-relative filesystem";
            break;
        case FilesProviderKind::OrdinaryArchive:
            caps.can_list = caps.can_read = caps.can_write = caps.can_delete = true;
            caps.can_rename = false;
            caps.transport_summary = "Ordinary archive provider (ZIP/TAR/TAR.GZ)";
            break;
        case FilesProviderKind::Sftp:
            caps.can_list = caps.can_read = caps.can_write = caps.can_delete = true;
            caps.can_rename = false;
            caps.requires_host_key_verification = true;
            caps.transport_summary = "SFTP client (BatchMode + StrictHostKeyChecking)";
            break;
        case FilesProviderKind::SmbMountedShare:
            caps.can_list = caps.can_read = caps.can_write = caps.can_delete = caps.can_rename = true;
            caps.mounted_share = true;
            caps.transport_summary = "Mounted SMB / UNC share";
            break;
        case FilesProviderKind::SmbProtocolClient:
            caps.can_list = false;
            caps.mounted_share = false;
            caps.transport_summary = "Unmounted SMB protocol client (mount via OS SMBFS)";
            break;
    }
    return caps;
}

std::string FilesRemoteUri::redacted_display() const {
    if (!valid)
        return "(invalid remote URI)";
    std::string out = scheme + "://";
    if (!username.empty()) {
        out.append(username);
        if (has_secret)
            out.append(":***");
        out.push_back('@');
    }
    out.append(host);
    if (port != 0) {
        out.push_back(':');
        out.append(std::to_string(port));
    }
    if (!share.empty()) {
        out.push_back('/');
        out.append(share);
    }
    if (!path.empty()) {
        if (path.front() != '/')
            out.push_back('/');
        out.append(path);
    }
    return out;
}

FilesRemoteUri parse_files_remote_uri(std::string_view raw_uri) {
    FilesRemoteUri uri;
    if (raw_uri.empty() || has_forbidden_control_chars(raw_uri)) {
        uri.error_message = "URI is empty or contains control characters.";
        return uri;
    }

    // Handle Windows UNC form \\host\share\path
    if (raw_uri.size() >= 4 && raw_uri[0] == '\\' && raw_uri[1] == '\\') {
        const std::string_view rest = raw_uri.substr(2);
        const std::size_t sep1 = rest.find('\\');
        if (sep1 == std::string_view::npos || sep1 == 0) {
            uri.error_message = "UNC path must specify \\\\host\\share.";
            return uri;
        }
        const std::string_view host = rest.substr(0, sep1);
        const std::string_view after_host = rest.substr(sep1 + 1);
        const std::size_t sep2 = after_host.find('\\');
        const std::string_view share = sep2 == std::string_view::npos
            ? after_host
            : after_host.substr(0, sep2);
        if (!valid_hostname_or_ip(host) || share.empty()) {
            uri.error_message = "Invalid UNC host or share name.";
            return uri;
        }
        uri.valid = true;
        uri.kind = FilesProviderKind::SmbMountedShare;
        uri.scheme = "smb";
        uri.host = std::string(host);
        uri.share = std::string(share);
        if (sep2 != std::string_view::npos) {
            uri.path = "/";
            for (const char ch : after_host.substr(sep2 + 1))
                uri.path.push_back(ch == '\\' ? '/' : ch);
        } else {
            uri.path = "/";
        }
        return uri;
    }

    const std::size_t scheme_sep = raw_uri.find("://");
    if (scheme_sep == std::string_view::npos || scheme_sep == 0) {
        uri.error_message = "Remote URI must start with sftp://, smb://, or \\\\host\\share.";
        return uri;
    }
    uri.scheme = ascii_lower(raw_uri.substr(0, scheme_sep));
    if (uri.scheme == "sftp") {
        uri.kind = FilesProviderKind::Sftp;
    } else if (uri.scheme == "smb") {
#if defined(_WIN32)
        uri.kind = FilesProviderKind::SmbMountedShare;
#else
        uri.kind = FilesProviderKind::SmbProtocolClient;
#endif
    } else {
        uri.error_message = "Unsupported remote scheme '" + uri.scheme + "'. Supported: sftp://, smb://.";
        return uri;
    }

    const std::string_view after_scheme = raw_uri.substr(scheme_sep + 3);
    const std::size_t path_pos = after_scheme.find('/');
    const std::string_view authority = path_pos == std::string_view::npos
        ? after_scheme
        : after_scheme.substr(0, path_pos);
    const std::string_view raw_path = path_pos == std::string_view::npos
        ? std::string_view("/")
        : after_scheme.substr(path_pos);

    std::string_view hostport = authority;
    const std::size_t at = authority.rfind('@');
    if (at != std::string_view::npos) {
        const std::string_view userinfo = authority.substr(0, at);
        hostport = authority.substr(at + 1);
        const std::size_t colon = userinfo.find(':');
        if (colon != std::string_view::npos) {
            uri.username = std::string(userinfo.substr(0, colon));
            uri.has_secret = true; // Never store the inline password in FilesRemoteUri!
        } else {
            uri.username = std::string(userinfo);
        }
    }

    std::string_view host = hostport;
    if (!hostport.empty() && hostport.front() == '[') {
        const std::size_t rb = hostport.find(']');
        if (rb == std::string_view::npos) {
            uri.error_message = "Malformed IPv6 host in URI.";
            return uri;
        }
        host = hostport.substr(0, rb + 1);
        if (rb + 1 < hostport.size() && hostport[rb + 1] == ':') {
            const std::string_view port_sv = hostport.substr(rb + 2);
            if (!parse_port_strict(port_sv, uri.port)) {
                uri.error_message = "Invalid or out-of-range port number.";
                return uri;
            }
        }
    } else {
        const std::size_t colon = hostport.rfind(':');
        if (colon != std::string_view::npos) {
            host = hostport.substr(0, colon);
            const std::string_view port_sv = hostport.substr(colon + 1);
            if (!parse_port_strict(port_sv, uri.port)) {
                uri.error_message = "Invalid or out-of-range port number.";
                return uri;
            }
        }
    }

    if (!valid_hostname_or_ip(host)) {
        uri.error_message = "Invalid remote hostname.";
        return uri;
    }
    uri.host = std::string(host);

    if (uri.scheme == "smb") {
        std::string_view p = raw_path;
        if (!p.empty() && p.front() == '/')
            p.remove_prefix(1);
        const std::size_t slash = p.find('/');
        const std::string_view share = slash == std::string_view::npos ? p : p.substr(0, slash);
        if (share.empty()) {
            uri.error_message = "SMB URI must specify a share name (smb://host/share).";
            return uri;
        }
        uri.share = std::string(share);
        uri.path = slash == std::string_view::npos ? std::string("/") : std::string(p.substr(slash));
    } else {
        uri.path = std::string(raw_path);
    }

    uri.valid = true;
    return uri;
}

std::string redact_remote_secrets(std::string_view text) {
    std::string out(text);
    std::size_t pos = 0;
    while (pos < out.size()) {
        const std::size_t scheme = out.find("://", pos);
        if (scheme == std::string::npos)
            break;
        const std::size_t auth_start = scheme + 3;
        const std::size_t auth_end = out.find_first_of("/ \t\r\n", auth_start);
        const std::size_t bound = auth_end == std::string::npos ? out.size() : auth_end;
        const std::size_t at = bound > auth_start ? out.rfind('@', bound - 1) : std::string::npos;
        if (at != std::string::npos && at >= auth_start && at < bound) {
            const std::size_t colon = out.find(':', auth_start);
            if (colon != std::string::npos && colon < at) {
                out.replace(colon + 1, at - (colon + 1), "***");
                pos = colon + 4;
                continue;
            }
        }
        pos = bound;
    }
    return out;
}

FilesRemoteSession::FilesRemoteSession(std::unique_ptr<FilesRemoteTransport> transport,
                                       FilesHostKeyPolicy policy)
    : transport_(std::move(transport)), policy_(policy) {}

FilesProviderCapabilities FilesRemoteSession::capabilities() const {
    return transport_ ? transport_->capabilities() : FilesProviderCapabilities{};
}

FilesRemoteListResult FilesRemoteSession::list(const FilesRemoteUri &uri,
                                               std::stop_token stop_token) {
    if (!transport_)
        return {FilesRemoteStatus::Unsupported, {}, "No remote transport configured."};
    return transport_->list(uri, policy_, stop_token);
}

FilesRemoteTransferResult FilesRemoteSession::download(
    const FilesRemoteUri &remote_file,
    const ck::files::NativeAbsolutePath &local_dest,
    std::stop_token stop_token) {
    if (!transport_)
        return {FilesRemoteStatus::Unsupported, 0, false, "No remote transport configured."};
    return transport_->download(remote_file, local_dest, policy_, stop_token);
}

FilesRemoteTransferResult FilesRemoteSession::upload(
    const ck::files::NativeAbsolutePath &local_src,
    const FilesRemoteUri &remote_dest,
    std::stop_token stop_token) {
    if (!transport_)
        return {FilesRemoteStatus::Unsupported, 0, false, "No remote transport configured."};
    if (uncertain_commit_uri_) {
        return {FilesRemoteStatus::UncertainCommit, 0, true,
                "Refused blind retry after uncertain remote commit on " + *uncertain_commit_uri_ +
                    "; inspect remote state and acknowledge before retrying."};
    }
    auto res = transport_->upload(local_src, remote_dest, policy_, stop_token);
    if (res.commit_uncertain || res.status == FilesRemoteStatus::UncertainCommit)
        uncertain_commit_uri_ = remote_dest.redacted_display();
    return res;
}

FilesRemoteTransferResult FilesRemoteSession::remove_remote(
    const FilesRemoteUri &remote_target,
    std::stop_token stop_token) {
    if (!transport_)
        return {FilesRemoteStatus::Unsupported, 0, false, "No remote transport configured."};
    if (uncertain_commit_uri_) {
        return {FilesRemoteStatus::UncertainCommit, 0, true,
                "Refused blind destructive retry after uncertain remote commit on " +
                    *uncertain_commit_uri_ + "; inspect remote state first."};
    }
    auto res = transport_->remove_remote(remote_target, policy_, stop_token);
    if (res.commit_uncertain || res.status == FilesRemoteStatus::UncertainCommit)
        uncertain_commit_uri_ = remote_target.redacted_display();
    return res;
}

std::unique_ptr<FilesRemoteTransport> make_sftp_remote_transport(const ckv::Clock &clock) {
    return std::make_unique<SftpProcessTransport>(clock);
}

std::unique_ptr<FilesRemoteTransport> make_smb_mounted_transport() {
    return std::make_unique<SmbMountedShareTransport>();
}

} // namespace ck::vision
