// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "native_operation_recovery.hpp"
#include "native_filesystem.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace ck::vision {

enum class FilesOperationRecoveryCleanupSubmission { Accepted, Busy, Stopped, Invalid };
enum class FilesOperationRecoveryCleanupState { Complete, Failed, Unsupported, WorkerFault };

struct FilesOperationRecoveryCleanupReply final {
    FilesOperationRecoveryCleanupState state = FilesOperationRecoveryCleanupState::Failed;
    std::optional<ck::files::UnavailableObservation> error;
};

// Worker-owned refusal boundary for restart/value-only cleanup requests.
// The journal and recovered IDs do not contain live staging-creator authority.
// Such requests return UnqualifiedIdentity without native I/O or mutation.
// In-process cleanup belongs to the originating copy job and its retained owner.
class FilesOperationRecoveryCleanupService final {
  public:
    explicit FilesOperationRecoveryCleanupService(std::function<void()> wake = {});
    ~FilesOperationRecoveryCleanupService();
    FilesOperationRecoveryCleanupService(const FilesOperationRecoveryCleanupService &) = delete;
    FilesOperationRecoveryCleanupService &operator=(const FilesOperationRecoveryCleanupService &) = delete;

    FilesOperationRecoveryCleanupSubmission request(
        ck::files::NativeAbsolutePath private_root, ck::files::NativeName journal_name,
        ck::files::OperationJournalId journal_id);
    std::optional<FilesOperationRecoveryCleanupReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ck::vision
