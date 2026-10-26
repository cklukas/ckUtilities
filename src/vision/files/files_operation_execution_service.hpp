// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_pane_service.hpp"
#include "native_operation_copy_job.hpp"
#include "native_operation_move_job.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace ck::vision {

// The execution request contains only process-local pane tickets and explicit
// native configuration. Native owners are acquired on pane workers and are
// consumed on the execution worker; none cross into FilesApp state.
struct FilesOperationExecutionRequest final {
    ck::files::FileOperation operation = ck::files::FileOperation::Copy;
    FilesPaneService *source_service = nullptr;
    FilesPaneService *destination_service = nullptr;
    FilesPaneTicket source_navigation, destination_navigation;
    ck::files::PaneStateStamp source_stamp, destination_stamp;
    ck::files::IndexedSourceLimits source_limits;
    ck::files::NativeAbsolutePath journal_root;
    ck::files::OperationJournalId journal_id{};
    ck::files::NativeOperationCopyBatchPolicy policy{};
    std::function<ck::files::Observation<std::unique_ptr<ck::files::OperationJournalFile>>(
        const ck::files::NativeAbsolutePath&, const ck::files::NativeName&, bool)> open_journal;
};

enum class FilesOperationExecutionSubmission { Accepted, Busy, Stopped, Invalid, Exhausted };
enum class FilesOperationExecutionState { Complete, Failed, Cancelled, WorkerFault };

struct FilesOperationExecutionTicket final {
    FilesOperationExecutionTicket() = default;
    std::uint64_t number() const noexcept { return identity_ ? *identity_ : 0; }
    bool valid() const noexcept { return identity_ != nullptr; }
    friend bool operator==(const FilesOperationExecutionTicket&, const FilesOperationExecutionTicket&) = default;

private:
    friend class FilesOperationExecutionService;
    explicit FilesOperationExecutionTicket(std::uint64_t number)
        : identity_(std::make_shared<const std::uint64_t>(number)) {}
    std::shared_ptr<const std::uint64_t> identity_;
};

struct FilesOperationExecutionSubmitted final {
    FilesOperationExecutionSubmission state;
    FilesOperationExecutionTicket ticket;
};

struct FilesOperationExecutionReply final {
    FilesOperationExecutionTicket request;
    FilesOperationExecutionState state = FilesOperationExecutionState::Failed;
    ck::files::OperationBatchResult result;
    std::optional<ck::files::UnavailableObservation> error;
    FilesPaneWorkerFault worker_fault = FilesPaneWorkerFault::None;
};

class FilesOperationExecutionService final {
public:
    explicit FilesOperationExecutionService(std::function<void()> wake = {});
    ~FilesOperationExecutionService();
    FilesOperationExecutionService(const FilesOperationExecutionService&) = delete;
    FilesOperationExecutionService& operator=(const FilesOperationExecutionService&) = delete;

    FilesOperationExecutionSubmitted request(FilesOperationExecutionRequest request);
    bool cancel(const FilesOperationExecutionTicket& request);
    std::optional<FilesOperationExecutionReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;

private:
    struct State;
    static FilesOperationExecutionTicket ticket(std::uint64_t number) {
        return FilesOperationExecutionTicket(number);
    }
    std::unique_ptr<State> state_;
};

} // namespace ck::vision
