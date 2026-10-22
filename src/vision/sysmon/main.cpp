// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sysmon_app.hpp"
#include "native_monitor.hpp"
#include "host_diagnostics.hpp"
#include "cvision/term/terminal_clipboard.hpp"
#include "host_about.hpp"
#include "native_host.hpp"
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string_view>
#if defined(_WIN32)
#include "cvision/term/windows_clock.hpp"
#include "cvision/term/windows_filesystem.hpp"
#include "cvision/term/windows_terminal.hpp"
#else
#include "cvision/term/posix_clock.hpp"
#include "cvision/term/posix_filesystem.hpp"
#include "cvision/term/posix_terminal.hpp"
#endif
namespace {
// Settings live in the suite's per-user folder, beside every other tool's.
std::string settings_path() {
    const std::string root = ck::host::suite_config_root();
    return root.empty() ? std::string{} : root + "/ck-sysmon.settings";
}
std::tm local_time() {
    const auto now = std::time(nullptr); std::tm local{};
#if defined(_WIN32)
    ::localtime_s(&local, &now);
#else
    ::localtime_r(&now, &local);
#endif
    return local;
}
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::cout << ck::host::version_line("ck-sysmon") << '\n'; return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: ck-sysmon [--version | --help | --sample | --sample-host alias]\n"; return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--sample") {
        std::cout << ck::sysmon::sample_text(ck::sysmon::native_sample()); return 0;
    }
    if (argc == 3 && std::string_view(argv[1]) == "--sample-host") {
#if defined(_WIN32)
        ckv::term::WindowsClock clock;
#else
        ckv::term::PosixClock clock;
#endif
        ck::sysmon::NativeMonitorSource source(clock);
        const auto sample = source.sample(argv[2]);
        if (!sample.diagnostic.empty()) std::cerr << sample.diagnostic << '\n';
        if (!sample.reachable) return 1;
        std::cout << ck::sysmon::sample_text(sample); return 0;
    }
    if (argc != 1) { std::cerr << "Usage: ck-sysmon [--version | --help | --sample | --sample-host alias]\n"; return 2; }
    return ck::host::run_reporting_failure([] {
        ck::host::NativeClock clock;
        const ck::host::HostDiagnostics diagnostics(clock);
        ck::host::NativeTerminal terminal(clock);
        ck::host::NativeFileSystem files;
        ck::sysmon::SettingsStore settings(files, settings_path());
        auto loaded = settings.load();
        ckv::term::TerminalClipboardWriter clipboard(terminal);
        ckv::ui::Application app(terminal, clock, clipboard);
        diagnostics.attach(terminal);
        diagnostics.attach(app);
        ck::sysmon::NativeMonitorSource source(clock);
        ck::sysmon::SysmonApp monitor(app, source, {
            [] { const auto now = local_time(); return ckv::widgets::TimeValue{now.tm_hour, now.tm_min, now.tm_sec}; },
            [] { const auto now = local_time(); return ckv::widgets::DateValue{now.tm_year + 1900, now.tm_mon + 1, now.tm_mday}; }},
            {std::move(loaded.value), [&settings](const ck::sysmon::Settings& value) { return settings.save(value); }, std::move(loaded.diagnostic)});
        app.run();
    });
}
