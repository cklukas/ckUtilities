// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_todo_repository.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

#include "cvision/term/headless_terminal.hpp"
#include "cvision/term/posix_filesystem.hpp"
#include "cvision/testing/cktest.hpp"
#include "todo_app.hpp"
#include "scratch_directory.hpp"

namespace {

using namespace ck::todo;

struct ScratchDirectory {
    ck::testing::ScratchDirectory directory{"todo-repository"};
    std::string path = directory.path().string();
};

AuditStamp posix_stamp() { return {IsoTimestamp{"2026-08-25T12:00:00Z"}, "posix-test"}; }

TodoWorkspace posix_workspace() {
    TodoWorkspace workspace = TodoWorkspace::empty();
    TaskDraft task;
    task.title = "Persist on disk";
    task.note = "Archive me after reopen.";
    const auto added = workspace.add_task(LaneId{1}, std::move(task), posix_stamp());
    if (!added) return TodoWorkspace::empty();
    return workspace;
}

int commit_task_from_child_process(const std::string& data) {
    ckv::term::PosixFileSystem filesystem;
    JsonTodoRepository repository(filesystem, data);
    auto loaded = repository.load();
    if (!loaded) return 10;
    TaskDraft task;
    task.title = "Committed by another process";
    if (!loaded.value->workspace.add_task(LaneId{2}, std::move(task), posix_stamp())) return 11;
    if (!repository.commit(loaded.value->revision, loaded.value->workspace, IsoDate{"2026-08-25"})) return 12;
    return 0;
}

}  // namespace

CK_TEST(todo_posix_repository_reopens_backup_and_archive_first_removal) {
    ScratchDirectory scratch;
    CK_CHECK(!scratch.path.empty());
    if (scratch.path.empty()) return;
    ckv::term::PosixFileSystem filesystem;
    const std::string data = scratch.path + "/data";
    JsonTodoRepository repository(filesystem, data);
    auto committed = repository.commit({}, TodoWorkspace::empty(), IsoDate{"2026-08-25"});
    CK_CHECK(committed);
    if (!committed) return;
    auto loaded = repository.load();
    CK_CHECK(loaded);
    if (!loaded) return;
    committed = repository.commit(loaded.value->revision, posix_workspace(), IsoDate{"2026-08-25"});
    CK_CHECK(committed);
    if (!committed) return;
    CK_CHECK(filesystem.exists(data + "/backup/2026-08-25.json"));

    JsonTodoRepository reopened(filesystem, data);
    loaded = reopened.load();
    CK_CHECK(loaded && loaded.value->workspace.find_task(TaskId{1}) != nullptr);
    if (!loaded) return;
    const auto plan = loaded.value->workspace.prepare_task_archive(TaskId{1}, posix_stamp());
    CK_CHECK(plan);
    if (!plan) return;
    CK_CHECK(reopened.store_archive(plan.value->records.front()));
    TodoWorkspace candidate = loaded.value->workspace;
    CK_CHECK(candidate.apply_archive(*plan.value));
    committed = reopened.commit(loaded.value->revision, candidate, IsoDate{"2026-08-25"});
    CK_CHECK(committed);
    if (!committed) return;
    CK_CHECK(filesystem.exists(data + "/archive/2026-08-25T12-00-00--task-1.json"));
    const auto final = reopened.load();
    CK_CHECK(final);
    if (final) CK_CHECK(final.value->workspace.find_task(TaskId{1}) == nullptr);
}

CK_TEST(todo_posix_repository_detects_external_atomic_replacement) {
    ScratchDirectory scratch;
    CK_CHECK(!scratch.path.empty());
    if (scratch.path.empty()) return;
    ckv::term::PosixFileSystem filesystem;
    JsonTodoRepository repository(filesystem, scratch.path + "/data");
    const auto created = repository.commit({}, TodoWorkspace::empty(), IsoDate{"2026-08-25"});
    CK_CHECK(created);
    if (!created) return;
    const auto loaded = repository.load();
    CK_CHECK(loaded);
    if (!loaded) return;
    const auto external = encode_workspace(posix_workspace());
    CK_CHECK(external);
    if (!external) return;
    CK_CHECK(filesystem.write_file_atomic(repository.workspace_path(), *external.value).status ==
             ckv::FileWriteStatus::Ok);
    const auto rejected =
        repository.commit(loaded.value->revision, TodoWorkspace::empty(), IsoDate{"2026-08-25"});
    CK_CHECK(!rejected && rejected.error.code == RepositoryErrorCode::Conflict);
    const auto final = repository.load();
    CK_CHECK(final);
    if (final) CK_CHECK(final.value->workspace.find_task(TaskId{1}) != nullptr);
}

CK_TEST(todo_app_displays_a_task_committed_by_another_process_on_the_next_poll) {
    ScratchDirectory scratch;
    CK_CHECK(!scratch.path.empty());
    if (scratch.path.empty()) return;
    const std::string data = scratch.path + "/data";
    ckv::term::PosixFileSystem filesystem;
    JsonTodoRepository repository(filesystem, data);
    const auto guided = TodoWorkspace::guided(posix_stamp());
    CK_CHECK(guided);
    if (!guided) return;
    CK_CHECK(repository.commit({}, *guided.value, IsoDate{"2026-08-25"}));

    ckv::term::HeadlessTerminal terminal(ckv::Size{100, 30});
    ckv::ManualClock monotonic;
    ckv::ui::Application app(terminal, monotonic);
    FixedCalendarClock calendar(
        {IsoTimestamp{"2026-08-25T12:00:00Z"}, IsoDate{"2026-08-25"}, IsoTime{"14:30"}});
    TodoApp todo(app, repository, calendar, "parent-process");
    CK_CHECK(todo.controller().workspace()->find_task(TaskId{4}) == nullptr);

    const pid_t child = ::fork();
    CK_CHECK(child >= 0);
    if (child < 0) return;
    if (child == 0) ::_exit(commit_task_from_child_process(data));

    int status = 0;
    CK_CHECK(::waitpid(child, &status, 0) == child);
    CK_CHECK(WIFEXITED(status));
    CK_CHECK(WEXITSTATUS(status) == 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return;

    monotonic.advance(1'000'000'000);
    app.step(monotonic.now_nanos());
    CK_CHECK(todo.controller().workspace()->find_task(TaskId{4}) != nullptr);
    CK_CHECK(todo.board_view()->lane_view(LaneId{2})->lane().task_ids.back() == TaskId{4});
}
