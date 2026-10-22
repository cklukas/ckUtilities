// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ck-todo — interactive native host. Environment, user records, wall
// time, and the real filesystem are resolved here and injected inward.
#include "native_host.hpp"
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>


#include "host_diagnostics.hpp"
#include "host_about.hpp"
#include "cvision/term/terminal_clipboard.hpp"
#include "cvision/ui/application.hpp"

#include "json_todo_repository.hpp"
#include "memory_todo_repository.hpp"
#include "todo_app.hpp"
#include "todo_host.hpp"

namespace {

std::vector<std::string_view> arguments(const std::vector<std::string>& args) {
    std::vector<std::string_view> result;
    for (std::size_t index = 1; index < args.size(); ++index) result.emplace_back(args[index]);
    return result;
}

int run_demo(const ck::todo::TodoLaunchOptions& options) {
    ck::host::NativeClock clock;
    const ck::host::HostDiagnostics diagnostics(clock);
    ck::host::NativeTerminal terminal(clock);
    diagnostics.attach(terminal);
    ckv::term::TerminalClipboardWriter clipboard(terminal);
    ckv::ui::Application app(terminal, clock, clipboard);
    diagnostics.attach(app);
    ck::todo::FixedCalendarClock calendar(
        {ck::todo::IsoTimestamp{"2026-08-25T12:00:00Z"},
         ck::todo::IsoDate{"2026-08-25"},
         ck::todo::IsoTime{"14:30"}});
    const auto guided = ck::todo::TodoWorkspace::guided(
        {ck::todo::IsoTimestamp{"2026-08-25T12:00:00Z"}, "demo"});
    if (!guided) {
        std::cerr << "ck-todo: cannot create the demo workspace: " << guided.error.diagnostic << '\n';
        return 1;
    }
    ck::todo::MemoryTodoRepository repository(*guided.value);
    ck::todo::TodoApp todo(app, repository, calendar, "demo",
                            {.initial_board_name = options.board_name,
                             .workspace_description = "in-memory demo workspace"});
    app.run();
    return 0;
}

int run_persistent(const ck::todo::TodoLaunchOptions& options) {
    const auto user = ck::host::current_user();
    const std::string root = ck::host::suite_config_root();
    if (!options.data_directory && root.empty()) {
        std::cerr << "ck-todo: cannot resolve a configuration directory; use --data-dir PATH\n";
        return 2;
    }
    const std::string directory = options.data_directory ? *options.data_directory : root + "/todo";
    ck::host::NativeClock clock;
    const ck::host::HostDiagnostics diagnostics(clock);
    ck::host::NativeTerminal terminal(clock);
    diagnostics.attach(terminal);
    ckv::term::TerminalClipboardWriter clipboard(terminal);
    ck::host::NativeFileSystem filesystem;
    ckv::ui::Application app(terminal, clock, clipboard);
    diagnostics.attach(app);
    ck::todo::SystemCalendarClock calendar;
    ck::todo::JsonTodoRepository repository(filesystem, directory);
    ck::todo::TodoApp todo(app, repository, calendar, user.name,
                            {.initial_board_name = options.board_name,
                             .workspace_description = "persistent workspace: " + directory});
    app.run();
    return 0;
}

}  // namespace

int run_main(const std::vector<std::string>& args) {
    const auto parsed = ck::todo::parse_todo_arguments(arguments(args));
    if (!parsed) {
        std::cerr << "ck-todo: " << parsed.diagnostic << "\n\n" << ck::todo::todo_usage();
        return 2;
    }
    if (parsed.value->show_help) {
        std::cout << ck::todo::todo_usage();
        return 0;
    }
    if (parsed.value->show_version) {
        std::cout << ck::host::version_line("ck-todo") << "\n";
        return 0;
    }
    return ck::host::run_reporting_failure(
        [&] { return parsed.value->demo ? run_demo(*parsed.value) : run_persistent(*parsed.value); });
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    return ck::host::run_reporting_failure([&] { return run_main(ck::host::native_arguments(argc, argv)); });
}
