// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_terminal_session.hpp"

#include <cvision/term/process_runner.hpp>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace ck::vision {
namespace {

bool contains_nul(std::string_view sv) noexcept {
    return sv.find('\0') != std::string_view::npos;
}

std::string default_shell_executable() {
#if defined(_WIN32)
    if (const char *comspec = std::getenv("COMSPEC"); comspec && *comspec != '\0')
        return std::string(comspec);
    return "C:\\Windows\\System32\\cmd.exe";
#else
    if (const char *shell = std::getenv("SHELL"); shell && *shell == '/')
        return std::string(shell);
    return "/bin/sh";
#endif
}

std::string quote_for_preview(std::string_view arg) {
    bool needs_quote = arg.empty();
    for (const char ch : arg) {
        if (ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' || ch == ';' ||
            ch == '$' || ch == '`' || ch == '&' || ch == '|') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote)
        return std::string(arg);
    std::string out = "'";
    for (const char ch : arg) {
        if (ch == '\'')
            out.append("'\\''");
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

std::string expand_scalar_placeholders(std::string_view tmpl,
                                       const FilesCommandPlaceholderContext &ctx) {
    std::string out;
    out.reserve(tmpl.size() + 32);
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '%' && i + 1 < tmpl.size()) {
            const char code = tmpl[i + 1];
            if (code == '%') {
                out.push_back('%');
                ++i;
                continue;
            }
            if (code == 'f') {
                out.append(ctx.current_name);
                ++i;
                continue;
            }
            if (code == 'p') {
                out.append(ctx.current_path);
                ++i;
                continue;
            }
            if (code == 'd') {
                out.append(ctx.active_directory);
                ++i;
                continue;
            }
            if (code == 'D') {
                out.append(ctx.opposite_directory);
                ++i;
                continue;
            }
        }
        out.push_back(tmpl[i]);
    }
    return out;
}

bool contains_shell_control_chars(std::string_view sv) noexcept {
    for (const char ch : sv) {
        if (ch == '\0' || ch == '\r' || ch == '\n')
            return true;
    }
    return false;
}

std::string shell_quote_data(std::string_view arg) {
#if defined(_WIN32)
    std::string out = "\"";
    for (const char ch : arg) {
        if (ch == '"')
            out.append("\"\"");
        else if (ch == '%')
            out.append("%%");
        else
            out.push_back(ch);
    }
    out.push_back('"');
    return out;
#else
    std::string out = "'";
    for (const char ch : arg) {
        if (ch == '\'')
            out.append("'\\''");
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
#endif
}

bool expand_shell_placeholders(std::string_view tmpl,
                               const FilesCommandPlaceholderContext &ctx,
                               std::string &out,
                               std::string &error_out) {
    out.clear();
    out.reserve(tmpl.size() + 64);
    const auto append_quoted = [&](std::string_view val) -> bool {
        if (contains_shell_control_chars(val)) {
            error_out = "Placeholder value contains forbidden control characters for shell mode.";
            return false;
        }
        out.append(shell_quote_data(val));
        return true;
    };
    const auto append_list = [&](const std::vector<std::string> &items,
                                 std::string_view fallback) -> bool {
        if (!items.empty()) {
            for (std::size_t idx = 0; idx < items.size(); ++idx) {
                if (idx > 0)
                    out.push_back(' ');
                if (!append_quoted(items[idx]))
                    return false;
            }
            return true;
        }
        if (!fallback.empty())
            return append_quoted(fallback);
        return true;
    };

    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '%' && i + 1 < tmpl.size()) {
            const char code = tmpl[i + 1];
            if (code == '%') {
                out.push_back('%');
                ++i;
                continue;
            }
            if (code == 'f') {
                if (!append_quoted(ctx.current_name))
                    return false;
                ++i;
                continue;
            }
            if (code == 'p') {
                if (!append_quoted(ctx.current_path))
                    return false;
                ++i;
                continue;
            }
            if (code == 'd') {
                if (!append_quoted(ctx.active_directory))
                    return false;
                ++i;
                continue;
            }
            if (code == 'D') {
                if (!append_quoted(ctx.opposite_directory))
                    return false;
                ++i;
                continue;
            }
            if (code == 'F') {
                if (!append_list(ctx.marked_names, ctx.current_name))
                    return false;
                ++i;
                continue;
            }
            if (code == 'P') {
                if (!append_list(ctx.marked_paths, ctx.current_path))
                    return false;
                ++i;
                continue;
            }
        }
        out.push_back(tmpl[i]);
    }
    return true;
}

std::string build_argv_preview(const ckv::core::ProcessLaunchSpec &spec) {
    std::string preview = quote_for_preview(spec.executable);
    for (const auto &arg : spec.arguments) {
        preview.push_back(' ');
        preview.append(quote_for_preview(arg));
    }
    return preview;
}

} // namespace

FilesExpandedCommand expand_files_user_menu_entry(
    const FilesUserMenuEntry &entry,
    const FilesCommandPlaceholderContext &context) {
    FilesExpandedCommand out;
    out.mode = entry.mode;
    if (context.active_directory.empty() || contains_nul(context.active_directory)) {
        out.error_message = "Working directory is invalid.";
        return out;
    }

    if (entry.mode == FilesCommandMode::DirectArgv) {
        if (entry.executable.empty() || contains_nul(entry.executable)) {
            out.error_message = "Executable path is empty or invalid.";
            return out;
        }
        std::vector<std::string> args;
        for (const auto &tmpl : entry.argument_templates) {
            if (tmpl == "%F") {
                if (!context.marked_names.empty()) {
                    for (const auto &name : context.marked_names)
                        args.push_back(name);
                } else if (!context.current_name.empty()) {
                    args.push_back(context.current_name);
                }
            } else if (tmpl == "%P") {
                if (!context.marked_paths.empty()) {
                    for (const auto &p : context.marked_paths)
                        args.push_back(p);
                } else if (!context.current_path.empty()) {
                    args.push_back(context.current_path);
                }
            } else {
                args.push_back(expand_scalar_placeholders(tmpl, context));
            }
        }
        out.launch = ckv::core::ProcessLaunchSpec::program(entry.executable, std::move(args));
        out.launch.working_directory = context.active_directory;
        if (ckv::core::validate_process_launch(out.launch) !=
            ckv::core::ProcessLaunchValidation::Valid) {
            out.error_message = "Expanded command failed launch validation.";
            return out;
        }
        out.review_preview = "[argv] " + build_argv_preview(out.launch);
        out.valid = true;
        return out;
    }

    // ExplicitShell mode: expand all placeholders with mandatory shell quoting so names remain data.
    std::string expanded;
    if (!expand_shell_placeholders(entry.shell_template, context, expanded, out.error_message))
        return out;
    return build_files_shell_command(expanded, context.active_directory);
}

FilesExpandedCommand parse_files_direct_command(
    std::string_view command_line,
    std::string_view working_directory) {
    FilesExpandedCommand out;
    out.mode = FilesCommandMode::DirectArgv;
    if (working_directory.empty() || contains_nul(working_directory)) {
        out.error_message = "Working directory is invalid.";
        return out;
    }
    if (contains_nul(command_line)) {
        out.error_message = "Command contains NUL byte.";
        return out;
    }

    std::vector<std::string> tokens;
    std::string current;
    bool in_token = false;
    char quote = '\0';
    for (std::size_t i = 0; i < command_line.size(); ++i) {
        const char ch = command_line[i];
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            } else if (quote == '"' && ch == '\\' && i + 1 < command_line.size()) {
                current.push_back(command_line[++i]);
            } else {
                current.push_back(ch);
            }
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            in_token = true;
        } else if (ch == ' ' || ch == '\t') {
            if (in_token) {
                tokens.push_back(std::move(current));
                current.clear();
                in_token = false;
            }
        } else {
            current.push_back(ch);
            in_token = true;
        }
    }
    if (quote != '\0') {
        out.error_message = "Unterminated quote in command.";
        return out;
    }
    if (in_token)
        tokens.push_back(std::move(current));
    if (tokens.empty()) {
        out.error_message = "Command is empty.";
        return out;
    }

    std::string exe = std::move(tokens.front());
    tokens.erase(tokens.begin());
    out.launch = ckv::core::ProcessLaunchSpec::program(std::move(exe), std::move(tokens));
    out.launch.working_directory = std::string(working_directory);
    if (ckv::core::validate_process_launch(out.launch) !=
        ckv::core::ProcessLaunchValidation::Valid) {
        out.error_message = "Parsed command failed launch validation.";
        return out;
    }
    out.review_preview = "[argv] " + build_argv_preview(out.launch);
    out.valid = true;
    return out;
}

FilesExpandedCommand build_files_shell_command(
    std::string_view shell_command,
    std::string_view working_directory) {
    FilesExpandedCommand out;
    out.mode = FilesCommandMode::ExplicitShell;
    if (shell_command.empty() || contains_nul(shell_command)) {
        out.error_message = "Shell command is empty or invalid.";
        return out;
    }
    if (working_directory.empty() || contains_nul(working_directory)) {
        out.error_message = "Working directory is invalid.";
        return out;
    }

#if defined(_WIN32)
    out.launch = ckv::core::ProcessLaunchSpec::windows_command_processor(
        default_shell_executable(), std::string(shell_command));
#else
    out.launch = ckv::core::ProcessLaunchSpec::program(
        "/bin/sh", {"-c", std::string(shell_command)});
#endif
    out.launch.working_directory = std::string(working_directory);
    if (ckv::core::validate_process_launch(out.launch) !=
        ckv::core::ProcessLaunchValidation::Valid) {
        out.error_message = "Shell command failed launch validation.";
        return out;
    }
    out.review_preview = "[shell] " + std::string(shell_command);
    out.valid = true;
    return out;
}

ckv::core::TerminalLaunchSpec build_files_interactive_terminal_spec(
    std::string_view working_directory) {
#if defined(_WIN32)
    auto spec = ckv::core::TerminalLaunchSpec::windows_command_processor(
        default_shell_executable(), std::nullopt);
#else
    auto spec = ckv::core::TerminalLaunchSpec::program(default_shell_executable(), {});
#endif
    spec.working_directory = working_directory.empty()
#if defined(_WIN32)
        ? std::string("C:\\")
#else
        ? std::string("/")
#endif
        : std::string(working_directory);
    spec.exit_policy = ckv::core::TerminalExitPolicy::TerminateAfterGrace;
    return spec;
}

ckv::core::ProcessRunResult execute_files_command(
    const FilesExpandedCommand &expanded,
    const ckv::Clock &clock,
    std::int64_t idle_budget_nanos,
    std::size_t max_output_bytes) {
    ckv::core::ProcessRunResult result;
    if (!expanded.valid) {
        result.state = ckv::core::ProcessRunState::InvalidRequest;
        result.diagnostic = expanded.error_message.empty()
            ? "Invalid command."
            : expanded.error_message;
        return result;
    }
    ckv::core::ProcessRunRequest req;
    req.launch = expanded.launch;
    req.descendants = ckv::core::ProcessDescendantPolicy::TerminateOnCompletion;
    req.idle_budget_nanos = idle_budget_nanos;
    req.max_stdout_bytes = max_output_bytes;
    req.max_stderr_bytes = max_output_bytes;
    ckv::term::NativeProcessRunner runner(clock);
    return runner.run(req);
}

std::vector<FilesUserMenuEntry> default_files_user_menu() {
    std::vector<FilesUserMenuEntry> entries;
#if defined(_WIN32)
    entries.push_back(FilesUserMenuEntry{
        .label = "List current directory (cmd)",
        .mode = FilesCommandMode::ExplicitShell,
        .executable = {},
        .argument_templates = {},
        .shell_template = "dir",
    });
#else
    entries.push_back(FilesUserMenuEntry{
        .label = "Inspect file type (file %f)",
        .mode = FilesCommandMode::DirectArgv,
        .executable = "/usr/bin/file",
        .argument_templates = {"--", "%f"},
        .shell_template = {},
    });
    entries.push_back(FilesUserMenuEntry{
        .label = "Word/line count (wc %F)",
        .mode = FilesCommandMode::DirectArgv,
        .executable = "/usr/bin/wc",
        .argument_templates = {"--", "%F"},
        .shell_template = {},
    });
    entries.push_back(FilesUserMenuEntry{
        .label = "Show git status in active folder",
        .mode = FilesCommandMode::DirectArgv,
        .executable = "/usr/bin/git",
        .argument_templates = {"status", "--short"},
        .shell_template = {},
    });
#endif
    return entries;
}

void FilesCommandHistory::record(std::string command) {
    if (command.empty())
        return;
    entries_.erase(std::remove(entries_.begin(), entries_.end(), command), entries_.end());
    entries_.push_back(std::move(command));
    if (entries_.size() > max_entries_)
        entries_.erase(entries_.begin(),
                       entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - max_entries_));
}

} // namespace ck::vision
