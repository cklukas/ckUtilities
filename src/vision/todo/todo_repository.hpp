// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <optional>
#include <string>
#include <utility>

#include "todo_model.hpp"

namespace ck::todo {

struct RepositoryRevision {
    std::string value;
    bool exists() const noexcept { return !value.empty(); }
    friend bool operator==(const RepositoryRevision&, const RepositoryRevision&) = default;
};

enum class RepositoryErrorCode {
    None,
    Missing,
    Conflict,
    InvalidDate,
    IoFailure,
    ArchiveConflict,
    InvalidData,
};

struct RepositoryError {
    RepositoryErrorCode code = RepositoryErrorCode::None;
    std::string diagnostic;
    friend bool operator==(const RepositoryError&, const RepositoryError&) = default;
};

template <class T>
struct RepositoryResult {
    std::optional<T> value;
    RepositoryError error;
    explicit operator bool() const noexcept { return value.has_value(); }

    static RepositoryResult success(T result) {
        RepositoryResult out;
        out.value.emplace(std::move(result));
        return out;
    }

    static RepositoryResult failure(RepositoryErrorCode code, std::string diagnostic) {
        RepositoryResult out;
        out.error = RepositoryError{code, std::move(diagnostic)};
        return out;
    }
};

struct LoadedWorkspace {
    TodoWorkspace workspace;
    RepositoryRevision revision;
};

struct RepositoryMutation {
    RepositoryRevision revision;
    bool changed = false;
};

using TodoLoadResult = RepositoryResult<LoadedWorkspace>;
using TodoRevisionResult = RepositoryResult<RepositoryRevision>;
using TodoCommitResult = RepositoryResult<RepositoryMutation>;
using TodoArchiveResult = RepositoryResult<RepositoryMutation>;

// ckvision-doc: todo-repository
class TodoRepository {
public:
    virtual ~TodoRepository() = default;

    virtual TodoLoadResult load() = 0;
    virtual TodoRevisionResult revision() = 0;
    virtual TodoCommitResult commit(const RepositoryRevision& expected,
                                    const TodoWorkspace& workspace,
                                    IsoDate mutation_date) = 0;
    virtual TodoArchiveResult store_archive(const ArchivedTask& record) = 0;
};
// ckvision-doc-end: todo-repository

}  // namespace ck::todo
