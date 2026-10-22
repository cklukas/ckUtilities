// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_host.hpp"

#include <string>
#include <utility>

namespace ck::todo {
namespace {

TodoLaunchParseResult failure(std::string diagnostic) { return {std::nullopt, std::move(diagnostic)}; }

}  // namespace

TodoLaunchParseResult parse_todo_arguments(const std::vector<std::string_view>& arguments) {
    TodoLaunchOptions options;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--demo") {
            options.demo = true;
        } else if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--version") {
            options.show_version = true;
        } else if (argument == "--data-dir" || argument == "--board") {
            if (index + 1 >= arguments.size() || arguments[index + 1].empty())
                return failure(std::string(argument) + " requires a non-empty value");
            std::optional<std::string>& target = argument == "--data-dir" ? options.data_directory : options.board_name;
            if (target) return failure(std::string(argument) + " may be specified only once");
            target = std::string(arguments[++index]);
        } else if (!argument.empty() && argument.front() == '-') {
            return failure("unknown option: " + std::string(argument));
        } else {
            if (argument.empty()) return failure("board name must not be empty");
            if (options.board_name) return failure("only one board name may be supplied");
            options.board_name = std::string(argument);
        }
    }
    if (options.demo && options.data_directory) return failure("--demo cannot be combined with --data-dir");
    return {std::move(options), {}};
}

std::string todo_usage() {
    return "Usage: ck-todo [--demo] [--data-dir PATH] [--board NAME | NAME]\n"
           "               [--help] [--version]\n\n"
           "  --demo           Open a fixed in-memory tour and write nothing.\n"
           "  --data-dir PATH  Store todo.json, backups, and archives under PATH.\n"
           "  --board NAME     Open NAME after loading the workspace.\n";
}

}  // namespace ck::todo
