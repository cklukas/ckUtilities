// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_path.hpp"

#include <cvision/core/clock.hpp>
#include <cvision/core/process_runner.hpp>
#include <cvision/core/terminal_subsession.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ck::vision {

enum class FilesCommandMode : std::uint8_t {
    // Direct argv execution: filenames are passed as literal argv elements and
    // are NEVER parsed by a shell.
    DirectArgv = 0,
    // Explicit opt-in shell command: requires user review before launch.
    ExplicitShell = 1,
};

struct FilesCommandPlaceholderContext final {
    std::string active_directory;
    std::string opposite_directory;
    std::string current_name;
    std::string current_path;
    std::vector<std::string> marked_names;
    std::vector<std::string> marked_paths;
};

struct FilesUserMenuEntry final {
    std::string label;
    FilesCommandMode mode = FilesCommandMode::DirectArgv;
    std::string executable;
    std::vector<std::string> argument_templates;
    std::string shell_template;
};

struct FilesExpandedCommand final {
    bool valid = false;
    FilesCommandMode mode = FilesCommandMode::DirectArgv;
    ckv::core::ProcessLaunchSpec launch;
    std::string review_preview;
    std::string error_message;
};

// Expands a DirectArgv template (%f, %p, %d, %D, %F, %P) into literal argv
// entries with zero shell interpretation. Rejects embedded NUL bytes.
FilesExpandedCommand expand_files_user_menu_entry(
    const FilesUserMenuEntry &entry,
    const FilesCommandPlaceholderContext &context);

// Parses a simple direct-argv command string (whitespace + single/double quotes,
// no shell metacharacter expansion) into executable + argv.
FilesExpandedCommand parse_files_direct_command(
    std::string_view command_line,
    std::string_view working_directory);

// Builds an explicit shell command launch spec (opt-in only; previewed to user).
FilesExpandedCommand build_files_shell_command(
    std::string_view shell_command,
    std::string_view working_directory);

// Builds a ckVision TerminalLaunchSpec for an interactive shell in `working_dir`.
ckv::core::TerminalLaunchSpec build_files_interactive_terminal_spec(
    std::string_view working_directory);

// Executes a validated DirectArgv or ExplicitShell command using ckVision's
// bounded NativeProcessRunner (never a PTY, bounded stdout/stderr, process-group cleanup).
ckv::core::ProcessRunResult execute_files_command(
    const FilesExpandedCommand &expanded,
    const ckv::Clock &clock,
    std::int64_t idle_budget_nanos = 10'000'000'000LL,
    std::size_t max_output_bytes = 256 * 1024);

// Returns starter user-menu entries (read-only / inspection safe by default).
std::vector<FilesUserMenuEntry> default_files_user_menu();

// Bounded command history with deduplication.
class FilesCommandHistory final {
  public:
    explicit FilesCommandHistory(std::size_t max_entries = 64) noexcept
        : max_entries_(max_entries) {}

    void record(std::string command);
    const std::vector<std::string> &entries() const noexcept { return entries_; }
    void clear() noexcept { entries_.clear(); }

  private:
    std::size_t max_entries_ = 64;
    std::vector<std::string> entries_;
};

} // namespace ck::vision
