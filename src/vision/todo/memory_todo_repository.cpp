// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "memory_todo_repository.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string_view>
#include <utility>

namespace ck::todo {
namespace {

template <class T>
RepositoryResult<T> failure(RepositoryErrorCode code, std::string diagnostic) {
    return RepositoryResult<T>::failure(code, std::move(diagnostic));
}

std::string decimal(std::uint64_t value) {
    std::array<char, 20> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data()));
}

std::string archive_key(const ArchivedTask& record) {
    std::string key = record.archived.timestamp.value.substr(0, 19);
    std::replace(key.begin(), key.end(), ':', '-');
    key += "--task-";
    key += decimal(record.task.id.value);
    return key;
}

}  // namespace

MemoryTodoRepository::MemoryTodoRepository(TodoWorkspace initial) : current_(std::move(initial)) {
    revision_ = next_revision();
}

TodoLoadResult MemoryTodoRepository::load() {
    if (consume_failure(MemoryRepositoryFailure::Load)) {
        return failure<LoadedWorkspace>(RepositoryErrorCode::IoFailure, "injected workspace load failure");
    }
    if (!current_) return failure<LoadedWorkspace>(RepositoryErrorCode::Missing, "TODO workspace does not exist");
    return TodoLoadResult::success(LoadedWorkspace{*current_, revision_});
}

TodoRevisionResult MemoryTodoRepository::revision() {
    if (consume_failure(MemoryRepositoryFailure::Revision)) {
        return failure<RepositoryRevision>(RepositoryErrorCode::IoFailure, "injected revision read failure");
    }
    return TodoRevisionResult::success(revision_);
}

TodoCommitResult MemoryTodoRepository::commit(const RepositoryRevision& expected,
                                              const TodoWorkspace& workspace,
                                              IsoDate mutation_date) {
    if (!is_valid(mutation_date)) {
        return failure<RepositoryMutation>(RepositoryErrorCode::InvalidDate,
                                           "mutation date must be a real YYYY-MM-DD date");
    }
    if (expected != revision_) {
        return failure<RepositoryMutation>(RepositoryErrorCode::Conflict, "workspace revision changed");
    }
    if (current_ && current_->snapshot() == workspace.snapshot()) {
        return TodoCommitResult::success(RepositoryMutation{revision_, false});
    }
    if (current_ && !backups_.contains(mutation_date.value)) {
        if (consume_failure(MemoryRepositoryFailure::BackupWrite)) {
            return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure, "injected daily backup failure");
        }
        backups_.emplace(mutation_date.value, current_->snapshot());
    }
    if (consume_failure(MemoryRepositoryFailure::PrimaryWrite)) {
        return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure, "injected primary write failure");
    }
    current_ = workspace;
    revision_ = next_revision();
    return TodoCommitResult::success(RepositoryMutation{revision_, true});
}

TodoArchiveResult MemoryTodoRepository::store_archive(const ArchivedTask& record) {
    if (!is_valid(record.archived.timestamp)) {
        return failure<RepositoryMutation>(RepositoryErrorCode::InvalidData,
                                           "archive timestamp must be canonical UTC");
    }
    if (consume_failure(MemoryRepositoryFailure::ArchiveWrite)) {
        return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure, "injected archive write failure");
    }
    const std::string key = archive_key(record);
    const auto existing = archives_.find(key);
    if (existing != archives_.end()) {
        if (existing->second == record) {
            return TodoArchiveResult::success(RepositoryMutation{revision_, false});
        }
        return failure<RepositoryMutation>(RepositoryErrorCode::ArchiveConflict,
                                           "archive key already contains different task data");
    }
    archives_.emplace(key, record);
    return TodoArchiveResult::success(RepositoryMutation{revision_, true});
}

bool MemoryTodoRepository::consume_failure(MemoryRepositoryFailure point) noexcept {
    if (failure_ != point) return false;
    failure_.reset();
    return true;
}

RepositoryRevision MemoryTodoRepository::next_revision() {
    ++revision_counter_;
    return RepositoryRevision{"memory:" + decimal(revision_counter_)};
}

}  // namespace ck::todo
