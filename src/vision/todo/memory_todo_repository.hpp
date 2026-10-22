// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "todo_repository.hpp"

namespace ck::todo {

enum class MemoryRepositoryFailure { Load, Revision, BackupWrite, PrimaryWrite, ArchiveWrite };

class MemoryTodoRepository final : public TodoRepository {
public:
    MemoryTodoRepository() = default;
    explicit MemoryTodoRepository(TodoWorkspace initial);

    TodoLoadResult load() override;
    TodoRevisionResult revision() override;
    TodoCommitResult commit(const RepositoryRevision& expected,
                            const TodoWorkspace& workspace,
                            IsoDate mutation_date) override;
    TodoArchiveResult store_archive(const ArchivedTask& record) override;

    void fail_next(MemoryRepositoryFailure point) noexcept { failure_ = point; }

    const std::map<std::string, WorkspaceSnapshot>& backups() const noexcept { return backups_; }
    const std::map<std::string, ArchivedTask>& archives() const noexcept { return archives_; }

private:
    bool consume_failure(MemoryRepositoryFailure point) noexcept;
    RepositoryRevision next_revision();

    std::optional<TodoWorkspace> current_;
    RepositoryRevision revision_;
    std::uint64_t revision_counter_ = 0;
    std::map<std::string, WorkspaceSnapshot> backups_;
    std::map<std::string, ArchivedTask> archives_;
    std::optional<MemoryRepositoryFailure> failure_;
};

}  // namespace ck::todo
