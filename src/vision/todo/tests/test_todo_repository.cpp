// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory_todo_repository.hpp"
#include "json_todo_repository.hpp"

#include <utility>

#include "cvision/testing/cktest.hpp"

namespace {

using namespace ck::todo;
using ckv::FileWriteStatus;
using ckv::MemoryFileSystem;

AuditStamp repository_stamp() { return {IsoTimestamp{"2026-08-25T12:00:00Z"}, "repository-test"}; }

TodoWorkspace with_task(std::string title = "Persist me") {
    TodoWorkspace workspace = TodoWorkspace::empty();
    TaskDraft task;
    task.title = std::move(title);
    const auto added = workspace.add_task(LaneId{1}, std::move(task), repository_stamp());
    if (!added) return TodoWorkspace::empty();
    return workspace;
}

ArchivedTask archived_record() {
    TodoWorkspace workspace = with_task();
    return workspace.prepare_task_archive(TaskId{1}, repository_stamp()).value->records.front();
}

}  // namespace

CK_TEST(todo_memory_repository_reports_missing_and_empty_revision) {
    MemoryTodoRepository repository;
    const auto loaded = repository.load();
    CK_CHECK(!loaded && loaded.error.code == RepositoryErrorCode::Missing);
    const auto revision = repository.revision();
    CK_CHECK(revision && !revision.value->exists());
}

CK_TEST(todo_memory_repository_creates_reopens_and_keeps_noop_revision) {
    MemoryTodoRepository repository;
    const TodoWorkspace workspace = with_task();
    const auto committed = repository.commit({}, workspace, IsoDate{"2026-08-25"});
    CK_CHECK(committed && committed.value->changed);
    CK_CHECK(committed.value->revision.exists());
    const auto loaded = repository.load();
    CK_CHECK(loaded && loaded.value->workspace.snapshot() == workspace.snapshot());
    CK_CHECK(loaded.value->revision == committed.value->revision);

    const auto no_op = repository.commit(loaded.value->revision, workspace, IsoDate{"2026-08-25"});
    CK_CHECK(no_op && !no_op.value->changed);
    CK_CHECK(no_op.value->revision == committed.value->revision);
    CK_CHECK(repository.backups().empty());
}

CK_TEST(todo_memory_repository_rejects_stale_revision_without_side_effects) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    const auto before = repository.load();
    const auto rejected = repository.commit(RepositoryRevision{"stale"}, with_task(), IsoDate{"2026-08-25"});
    CK_CHECK(!rejected && rejected.error.code == RepositoryErrorCode::Conflict);
    const auto after = repository.load();
    CK_CHECK(after.value->workspace.snapshot() == before.value->workspace.snapshot());
    CK_CHECK(after.value->revision == before.value->revision);
    CK_CHECK(repository.backups().empty());
}

CK_TEST(todo_memory_repository_writes_previous_workspace_once_per_date) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    auto loaded = repository.load();
    const WorkspaceSnapshot original = loaded.value->workspace.snapshot();
    auto committed = repository.commit(loaded.value->revision, with_task("first"), IsoDate{"2026-08-25"});
    CK_CHECK(committed && repository.backups().size() == 1);
    CK_CHECK(repository.backups().at("2026-08-25") == original);

    loaded = repository.load();
    const WorkspaceSnapshot first = loaded.value->workspace.snapshot();
    committed = repository.commit(loaded.value->revision, with_task("second"), IsoDate{"2026-08-25"});
    CK_CHECK(committed && repository.backups().size() == 1);
    CK_CHECK(repository.backups().at("2026-08-25") == original);

    loaded = repository.load();
    const WorkspaceSnapshot second = loaded.value->workspace.snapshot();
    committed = repository.commit(loaded.value->revision, with_task("third"), IsoDate{"2026-08-26"});
    CK_CHECK(committed && repository.backups().size() == 2);
    CK_CHECK(repository.backups().at("2026-08-26") == second);
}

CK_TEST(todo_memory_repository_failure_points_preserve_last_primary_workspace) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    const auto original = repository.load();
    repository.fail_next(MemoryRepositoryFailure::BackupWrite);
    auto result = repository.commit(original.value->revision, with_task(), IsoDate{"2026-08-25"});
    CK_CHECK(!result && result.error.code == RepositoryErrorCode::IoFailure);
    CK_CHECK(repository.load().value->workspace.snapshot() == original.value->workspace.snapshot());
    CK_CHECK(repository.backups().empty());

    repository.fail_next(MemoryRepositoryFailure::PrimaryWrite);
    result = repository.commit(original.value->revision, with_task(), IsoDate{"2026-08-25"});
    CK_CHECK(!result && result.error.code == RepositoryErrorCode::IoFailure);
    CK_CHECK(repository.load().value->workspace.snapshot() == original.value->workspace.snapshot());
    CK_CHECK(repository.backups().size() == 1);
}

CK_TEST(todo_memory_repository_load_and_revision_failures_are_one_shot) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    repository.fail_next(MemoryRepositoryFailure::Load);
    CK_CHECK(repository.load().error.code == RepositoryErrorCode::IoFailure);
    CK_CHECK(repository.load());
    repository.fail_next(MemoryRepositoryFailure::Revision);
    CK_CHECK(repository.revision().error.code == RepositoryErrorCode::IoFailure);
    CK_CHECK(repository.revision());
}

CK_TEST(todo_memory_repository_archive_is_idempotent_and_conflict_checked) {
    MemoryTodoRepository repository(TodoWorkspace::empty());
    const ArchivedTask record = archived_record();
    auto stored = repository.store_archive(record);
    CK_CHECK(stored && stored.value->changed);
    CK_CHECK(repository.archives().size() == 1);
    stored = repository.store_archive(record);
    CK_CHECK(stored && !stored.value->changed);

    ArchivedTask different = record;
    different.origin_lane_title = "Different";
    stored = repository.store_archive(different);
    CK_CHECK(!stored && stored.error.code == RepositoryErrorCode::ArchiveConflict);
    CK_CHECK(repository.archives().begin()->second == record);
}

CK_TEST(todo_memory_repository_archive_failure_does_not_publish_a_record) {
    MemoryTodoRepository repository;
    repository.fail_next(MemoryRepositoryFailure::ArchiveWrite);
    const auto stored = repository.store_archive(archived_record());
    CK_CHECK(!stored && stored.error.code == RepositoryErrorCode::IoFailure);
    CK_CHECK(repository.archives().empty());
}

CK_TEST(todo_memory_repository_rejects_archive_with_invalid_timestamp) {
    MemoryTodoRepository repository;
    ArchivedTask record = archived_record();
    record.archived.timestamp = IsoTimestamp{"not-a-timestamp"};
    const auto stored = repository.store_archive(record);
    CK_CHECK(!stored && stored.error.code == RepositoryErrorCode::InvalidData);
    CK_CHECK(repository.archives().empty());
}

CK_TEST(todo_memory_repository_rejects_invalid_mutation_date) {
    MemoryTodoRepository repository;
    const auto result = repository.commit({}, TodoWorkspace::empty(), IsoDate{"2026-02-30"});
    CK_CHECK(!result && result.error.code == RepositoryErrorCode::InvalidDate);
    CK_CHECK(!repository.revision().value->exists());
}

CK_TEST(todo_json_repository_creates_directories_commits_and_reopens) {
    MemoryFileSystem filesystem;
    JsonTodoRepository repository(filesystem, "/todo-data");
    CK_CHECK(repository.load().error.code == RepositoryErrorCode::Missing);
    const TodoWorkspace workspace = with_task();
    const auto committed = repository.commit({}, workspace, IsoDate{"2026-08-25"});
    CK_CHECK(committed && committed.value->changed);
    CK_CHECK(filesystem.is_directory("/todo-data/backup"));
    CK_CHECK(filesystem.is_directory("/todo-data/archive"));
    CK_CHECK(filesystem.exists("/todo-data/todo.json"));
    const auto loaded = repository.load();
    CK_CHECK(loaded && loaded.value->workspace.snapshot() == workspace.snapshot());
    CK_CHECK(loaded.value->revision == committed.value->revision);
}

CK_TEST(todo_json_repository_daily_backup_preserves_previous_file_bytes_once) {
    MemoryFileSystem filesystem;
    JsonTodoRepository repository(filesystem, "/todo-data");
    CK_CHECK(repository.commit({}, TodoWorkspace::empty(), IsoDate{"2026-08-25"}));
    auto loaded = repository.load();
    const std::string original = filesystem.read_file(repository.workspace_path())->contents;
    CK_CHECK(repository.commit(loaded.value->revision, with_task("first"), IsoDate{"2026-08-25"}));
    const auto backup = filesystem.read_file("/todo-data/backup/2026-08-25.json");
    CK_CHECK(backup && backup->contents == original);

    loaded = repository.load();
    CK_CHECK(repository.commit(loaded.value->revision, with_task("second"), IsoDate{"2026-08-25"}));
    CK_CHECK(filesystem.read_file("/todo-data/backup/2026-08-25.json")->contents == original);
}

CK_TEST(todo_json_repository_noop_keeps_fingerprint_and_does_not_create_backup) {
    MemoryFileSystem filesystem;
    JsonTodoRepository repository(filesystem, "/todo-data");
    const TodoWorkspace workspace = with_task();
    const auto first = repository.commit({}, workspace, IsoDate{"2026-08-25"});
    const auto second = repository.commit(first.value->revision, workspace, IsoDate{"2026-08-25"});
    CK_CHECK(second && !second.value->changed);
    CK_CHECK(second.value->revision == first.value->revision);
    CK_CHECK(!filesystem.exists("/todo-data/backup/2026-08-25.json"));
}

CK_TEST(todo_json_repository_detects_external_revision_change) {
    MemoryFileSystem filesystem;
    JsonTodoRepository repository(filesystem, "/todo-data");
    CK_CHECK(repository.commit({}, TodoWorkspace::empty(), IsoDate{"2026-08-25"}));
    const auto loaded = repository.load();
    const auto external = encode_workspace(with_task("external"));
    CK_CHECK(filesystem.write_file_atomic(repository.workspace_path(), *external.value).status == FileWriteStatus::Ok);
    const auto rejected = repository.commit(loaded.value->revision, with_task("local"), IsoDate{"2026-08-25"});
    CK_CHECK(!rejected && rejected.error.code == RepositoryErrorCode::Conflict);
    CK_CHECK(repository.load().value->workspace.find_task(TaskId{1})->title == "external");
}

CK_TEST(todo_json_repository_rejects_invalid_existing_data_without_rewrite) {
    MemoryFileSystem filesystem;
    filesystem.add_file("/todo-data/todo.json", "{broken");
    JsonTodoRepository repository(filesystem, "/todo-data");
    const auto loaded = repository.load();
    CK_CHECK(!loaded && loaded.error.code == RepositoryErrorCode::InvalidData);
    const auto revision = repository.revision();
    const auto committed = repository.commit(*revision.value, TodoWorkspace::empty(), IsoDate{"2026-08-25"});
    CK_CHECK(!committed && committed.error.code == RepositoryErrorCode::InvalidData);
    CK_CHECK(filesystem.read_file(repository.workspace_path())->contents == "{broken");
}

CK_TEST(todo_json_repository_archive_is_canonical_idempotent_and_conflict_checked) {
    MemoryFileSystem filesystem;
    JsonTodoRepository repository(filesystem, "/todo-data");
    const ArchivedTask record = archived_record();
    auto stored = repository.store_archive(record);
    CK_CHECK(stored && stored.value->changed);
    const std::string path = "/todo-data/archive/2026-08-25T12-00-00--task-1.json";
    const auto archived = filesystem.read_file(path);
    CK_CHECK(archived);
    CK_CHECK(archived->contents == *encode_archive_record(record).value);
    stored = repository.store_archive(record);
    CK_CHECK(stored && !stored.value->changed);

    CK_CHECK(filesystem.write_file_atomic(path, "different").status == FileWriteStatus::Ok);
    stored = repository.store_archive(record);
    CK_CHECK(!stored && stored.error.code == RepositoryErrorCode::ArchiveConflict);
    CK_CHECK(filesystem.read_file(path)->contents == "different");
}
