// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_todo_repository.hpp"

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

RepositoryErrorCode write_error(ckv::FileWriteStatus status) noexcept {
    return status == ckv::FileWriteStatus::Conflict ? RepositoryErrorCode::Conflict : RepositoryErrorCode::IoFailure;
}

std::string decimal(std::uint64_t value) {
    std::array<char, 20> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return std::string(buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data()));
}

std::string archive_filename(const ArchivedTask& record) {
    std::string filename = record.archived.timestamp.value.substr(0, 19);
    std::replace(filename.begin(), filename.end(), ':', '-');
    filename += "--task-";
    filename += decimal(record.task.id.value);
    filename += ".json";
    return filename;
}

}  // namespace

JsonTodoRepository::JsonTodoRepository(ckv::FileSystem& filesystem, std::string data_directory)
    : filesystem_(filesystem), data_directory_(filesystem.normalize_path(data_directory)) {
    backup_directory_ = filesystem_.join(data_directory_, "backup");
    archive_directory_ = filesystem_.join(data_directory_, "archive");
    workspace_path_ = filesystem_.join(data_directory_, "todo.json");
}

TodoLoadResult JsonTodoRepository::load() {
    if (!filesystem_.exists(workspace_path_)) {
        return failure<LoadedWorkspace>(RepositoryErrorCode::Missing, "TODO workspace does not exist");
    }
    const auto file = filesystem_.read_file(workspace_path_);
    if (!file) return failure<LoadedWorkspace>(RepositoryErrorCode::IoFailure, "TODO workspace could not be read");
    auto decoded = decode_workspace(file->contents);
    if (!decoded) {
        return failure<LoadedWorkspace>(RepositoryErrorCode::InvalidData,
                                        "TODO workspace is invalid: " + decoded.error.diagnostic);
    }
    return TodoLoadResult::success(LoadedWorkspace{std::move(*decoded.value), revision_for(file->fingerprint)});
}

TodoRevisionResult JsonTodoRepository::revision() {
    if (!filesystem_.exists(workspace_path_)) return TodoRevisionResult::success({});
    const auto fingerprint = filesystem_.fingerprint(workspace_path_);
    if (!fingerprint) {
        return failure<RepositoryRevision>(RepositoryErrorCode::IoFailure,
                                           "TODO workspace revision could not be read");
    }
    return TodoRevisionResult::success(revision_for(*fingerprint));
}

TodoCommitResult JsonTodoRepository::commit(const RepositoryRevision& expected,
                                            const TodoWorkspace& workspace,
                                            IsoDate mutation_date) {
    if (!is_valid(mutation_date)) {
        return failure<RepositoryMutation>(RepositoryErrorCode::InvalidDate,
                                           "mutation date must be a real YYYY-MM-DD date");
    }
    if (!ensure_directories()) {
        return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure, "TODO data directories could not be created");
    }
    const bool exists = filesystem_.exists(workspace_path_);
    std::optional<ckv::FileReadResult> current;
    if (exists) {
        current = filesystem_.read_file(workspace_path_);
        if (!current) {
            return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure, "TODO workspace could not be read");
        }
    }
    const RepositoryRevision actual = current ? revision_for(current->fingerprint) : RepositoryRevision{};
    if (actual != expected) {
        return failure<RepositoryMutation>(RepositoryErrorCode::Conflict, "workspace revision changed");
    }

    const auto encoded = encode_workspace(workspace);
    if (!encoded) {
        return failure<RepositoryMutation>(RepositoryErrorCode::InvalidData, encoded.error.diagnostic);
    }
    if (current) {
        auto decoded = decode_workspace(current->contents);
        if (!decoded) {
            return failure<RepositoryMutation>(RepositoryErrorCode::InvalidData,
                                               "existing TODO workspace is invalid: " + decoded.error.diagnostic);
        }
        if (decoded.value->snapshot() == workspace.snapshot()) {
            return TodoCommitResult::success(RepositoryMutation{actual, false});
        }
        const std::string backup_path = filesystem_.join(backup_directory_, mutation_date.value + ".json");
        if (!filesystem_.exists(backup_path)) {
            const ckv::FileWriteResult backup = filesystem_.write_file_atomic(
                backup_path, current->contents, ckv::FileWriteExpectation::must_not_exist());
            if (backup.status == ckv::FileWriteStatus::Conflict && !filesystem_.read_file(backup_path)) {
                return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure,
                                                   "daily TODO backup path is not a readable file");
            }
            if (backup.status != ckv::FileWriteStatus::Ok && backup.status != ckv::FileWriteStatus::Conflict) {
                return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure, "daily TODO backup could not be written");
            }
        }
    }

    const ckv::FileWriteExpectation expectation = current
                                                 ? ckv::FileWriteExpectation::matching(current->fingerprint)
                                                 : ckv::FileWriteExpectation::must_not_exist();
    const ckv::FileWriteResult written = filesystem_.write_file_atomic(workspace_path_, *encoded.value, expectation);
    if (written.status != ckv::FileWriteStatus::Ok || !written.fingerprint) {
        return failure<RepositoryMutation>(write_error(written.status),
                                           written.status == ckv::FileWriteStatus::Conflict
                                               ? "workspace revision changed during commit"
                                               : "TODO workspace could not be written");
    }
    return TodoCommitResult::success(RepositoryMutation{revision_for(*written.fingerprint), true});
}

TodoArchiveResult JsonTodoRepository::store_archive(const ArchivedTask& record) {
    if (!is_valid(record.archived.timestamp)) {
        return failure<RepositoryMutation>(RepositoryErrorCode::InvalidData,
                                           "archive timestamp must be canonical UTC");
    }
    if (!ensure_directories()) {
        return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure, "TODO data directories could not be created");
    }
    const auto encoded = encode_archive_record(record);
    if (!encoded) return failure<RepositoryMutation>(RepositoryErrorCode::InvalidData, encoded.error.diagnostic);
    const std::string path = archive_path(record);
    const ckv::FileWriteResult written =
        filesystem_.write_file_atomic(path, *encoded.value, ckv::FileWriteExpectation::must_not_exist());
    if (written.status == ckv::FileWriteStatus::Ok) {
        const auto current_revision = revision();
        return TodoArchiveResult::success(
            RepositoryMutation{current_revision ? *current_revision.value : RepositoryRevision{}, true});
    }
    if (written.status == ckv::FileWriteStatus::Conflict) {
        const auto existing = filesystem_.read_file(path);
        if (existing && existing->contents == *encoded.value) {
            const auto current_revision = revision();
            return TodoArchiveResult::success(
                RepositoryMutation{current_revision ? *current_revision.value : RepositoryRevision{}, false});
        }
        return failure<RepositoryMutation>(RepositoryErrorCode::ArchiveConflict,
                                           "archive path already contains different task data");
    }
    return failure<RepositoryMutation>(RepositoryErrorCode::IoFailure, "TODO archive could not be written");
}

bool JsonTodoRepository::ensure_directories() {
    return filesystem_.create_directories(data_directory_) && filesystem_.create_directories(backup_directory_) &&
           filesystem_.create_directories(archive_directory_);
}

std::string JsonTodoRepository::archive_path(const ArchivedTask& record) const {
    return filesystem_.join(archive_directory_, archive_filename(record));
}

RepositoryRevision JsonTodoRepository::revision_for(const ckv::FileFingerprint& fingerprint) const {
    return RepositoryRevision{"file:" + fingerprint.value};
}

}  // namespace ck::todo
