// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include "cvision/core/filesystem.hpp"
#include "todo_codec.hpp"
#include "todo_repository.hpp"

namespace ck::todo {

class JsonTodoRepository final : public TodoRepository {
public:
    JsonTodoRepository(ckv::FileSystem& filesystem, std::string data_directory);

    TodoLoadResult load() override;
    TodoRevisionResult revision() override;
    TodoCommitResult commit(const RepositoryRevision& expected,
                            const TodoWorkspace& workspace,
                            IsoDate mutation_date) override;
    TodoArchiveResult store_archive(const ArchivedTask& record) override;

    const std::string& data_directory() const noexcept { return data_directory_; }
    const std::string& workspace_path() const noexcept { return workspace_path_; }

private:
    bool ensure_directories();
    std::string archive_path(const ArchivedTask& record) const;
    RepositoryRevision revision_for(const ckv::FileFingerprint& fingerprint) const;

    ckv::FileSystem& filesystem_;
    std::string data_directory_;
    std::string backup_directory_;
    std::string archive_directory_;
    std::string workspace_path_;
};

}  // namespace ck::todo
