// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_monitor.hpp"
#include "remote_scripts.hpp"
#include "cvision/core/base64.hpp"
#include "cvision/term/process_runner.hpp"
#include <cstdlib>
#include <map>
#include <mutex>
#include <stdexcept>

namespace ck::sysmon {
namespace {
std::string environment(const char* key, std::string fallback) {
    const auto* value = std::getenv(key); return value && *value ? value : std::move(fallback);
}
std::string powershell_command(std::string_view script) {
    // OpenSSH on Windows uses the account's default shell. EncodedCommand
    // carries trusted UTF-16LE script bytes without depending on its quoting.
    std::string utf16; utf16.reserve(script.size() * 2);
    for (const char c : script) { utf16 += c; utf16 += '\0'; }
    return "powershell.exe -NoLogo -NoProfile -NonInteractive -EncodedCommand " + ckv::base64::encode(utf16);
}
std::string posix_command(std::string_view script) {
    std::string out = "sh -c '";
    for (const char c : script) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
}
struct NativeMonitorSource::Impl {
    explicit Impl(const ckv::Clock& value) : clock(value) {
#if defined(_WIN32)
        home = environment("USERPROFILE", "C:/");
        system = environment("ProgramData", "C:/ProgramData") + "/ssh/ssh_config";
        ssh = environment("SystemRoot", "C:/Windows") + "/System32/OpenSSH/ssh.exe";
        cwd = environment("SystemRoot", "C:/Windows");
#else
        home = environment("HOME", "/"); system = "/etc/ssh/ssh_config"; ssh = "/usr/bin/ssh"; cwd = "/";
#endif
    }
    const ckv::Clock& clock;
    std::string home, system, ssh, cwd;
    std::mutex mutex;
    std::map<std::string, std::string> commands;
    ckv::core::ProcessRunResult run(const std::string& host, const std::string& command) {
        ckv::term::NativeProcessRunner runner(clock);
        ckv::core::ProcessRunRequest request;
        request.launch = ckv::core::ProcessLaunchSpec::program(ssh, {
            "-T", "-oBatchMode=yes", "-oNumberOfPasswordPrompts=0", "-oStrictHostKeyChecking=yes", "-oUpdateHostKeys=no",
            "-oConnectTimeout=3", "-oConnectionAttempts=1", "-oServerAliveInterval=2", "-oServerAliveCountMax=1",
            "-oClearAllForwardings=yes", "-oForwardAgent=no", "-oForwardX11=no", "-oPermitLocalCommand=no",
            "-oRemoteCommand=none", "-oControlMaster=no", "-oControlPath=none", "--", host, command});
        request.launch.working_directory = cwd;
        request.descendants = ckv::core::ProcessDescendantPolicy::TerminateOnCompletion;
        request.idle_budget_nanos = 10'000'000'000;
        request.max_stdout_bytes = 32768; request.max_stderr_bytes = 2048;
        return runner.run(request);
    }
    Sample remote(const std::string& endpoint) {
        std::string command;
        { std::lock_guard lock(mutex); if (commands.contains(endpoint)) command = commands.at(endpoint); }
        if (command.empty()) {
            const auto ready = run(endpoint, "echo CK_SYSMON_READY");
            if (!ready.successful() || ready.stdout_capture.bytes.find("CK_SYSMON_READY") == std::string::npos)
                return failure(ready);
            const auto helper = run(endpoint, "ck-sysmon --sample");
            if (helper.successful()) {
                const auto sample = decode(helper);
                if (sample) { std::lock_guard lock(mutex); commands[endpoint] = "ck-sysmon --sample"; return *sample; }
            }
            const auto os = run(endpoint, "uname -s");
            if (os.successful() && os.stdout_capture.bytes.find("Linux") != std::string::npos) command = posix_command(linux_script);
            else if (os.successful() && os.stdout_capture.bytes.find("Darwin") != std::string::npos) command = posix_command(darwin_script);
            else command = powershell_command(windows_script);
            { std::lock_guard lock(mutex); commands[endpoint] = command; }
        }
        const auto result = run(endpoint, command);
        if (!result.successful()) return failure(result);
        const auto sample = decode(result);
        if (sample) return *sample;
        Sample invalid; invalid.reachable = true; invalid.system = "SSH connected";
        invalid.diagnostic = "Unsupported or invalid metrics response"; return invalid;
    }
    static std::optional<Sample> decode(const ckv::core::ProcessRunResult& result) {
        if (result.stdout_capture.truncated) return {};
        std::string text;
        for (const char c : result.stdout_capture.bytes) if (c != '\r') text += c;
        return parse_sample(text);
    }
    static Sample failure(const ckv::core::ProcessRunResult& result) {
        Sample sample;
        sample.reachable = result.stdout_capture.bytes.starts_with("CKSYSMON 2");
        if (sample.reachable) sample.system = "SSH connected";
        sample.diagnostic = result.state == ckv::core::ProcessRunState::IdleTimeout ? "SSH timed out" : "Passwordless SSH unavailable";
        if (!result.stderr_capture.bytes.empty()) {
            std::string reason = result.stderr_capture.bytes.substr(0, 180);
            for (char& c : reason) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
            sample.diagnostic += ": " + reason;
        }
        return sample;
    }
};
NativeMonitorSource::NativeMonitorSource(const ckv::Clock& clock) : impl_(std::make_unique<Impl>(clock)) {}
NativeMonitorSource::~NativeMonitorSource() = default;
std::vector<std::string> NativeMonitorSource::hosts() { return configured_hosts(impl_->home + "/.ssh/config", impl_->system, impl_->home); }
Sample NativeMonitorSource::sample(const std::string& endpoint) { return endpoint.empty() ? native_sample() : impl_->remote(endpoint); }
} // namespace ck::sysmon
