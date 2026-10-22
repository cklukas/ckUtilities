// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "todo_host.hpp"

#include "cvision/testing/cktest.hpp"

using namespace ck::todo;

CK_TEST(todo_host_accepts_demo_data_board_and_positional_forms) {
    auto parsed = parse_todo_arguments({"--demo", "--board", "project"});
    CK_CHECK(parsed && parsed.value->demo && parsed.value->board_name == "project");
    parsed = parse_todo_arguments({"--data-dir", "/tmp/todo-data", "work"});
    CK_CHECK(parsed && parsed.value->data_directory == "/tmp/todo-data");
    CK_CHECK(parsed.value->board_name == "work");
}

CK_TEST(todo_host_accepts_help_and_version_without_other_requirements) {
    const auto help = parse_todo_arguments({"--help"});
    const auto version = parse_todo_arguments({"--version"});
    CK_CHECK(help && help.value->show_help);
    CK_CHECK(version && version.value->show_version);
    CK_CHECK(todo_usage().find("--demo") != std::string::npos);
}

CK_TEST(todo_host_rejects_unknown_missing_duplicate_and_conflicting_arguments) {
    CK_CHECK(!parse_todo_arguments({"--unknown"}));
    CK_CHECK(!parse_todo_arguments({"--data-dir"}));
    CK_CHECK(!parse_todo_arguments({"one", "two"}));
    CK_CHECK(!parse_todo_arguments({"--board", "one", "--board", "two"}));
    CK_CHECK(!parse_todo_arguments({"--demo", "--data-dir", "/tmp/data"}));
}
