// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery_cleanup.hpp"

#include "native_operation_recovery.hpp"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace ck::vision {
namespace {
using namespace ck::files;

template <class T>
Observation<T> failure(AdapterError code, const char *operation,
                       ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

FilesOperationRecoveryCleanupReply make_error(const UnavailableObservation &detail) {
    return {detail.state == ObservationState::Unsupported
                ? FilesOperationRecoveryCleanupState::Unsupported
                : FilesOperationRecoveryCleanupState::Failed,
            detail};
}

} // namespace

struct FilesOperationRecoveryCleanupService::State final {
    struct Request final {
        NativeAbsolutePath root;
        NativeName journal_name;
        OperationJournalId journal_id;
    };
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Request> pending;
    std::optional<FilesOperationRecoveryCleanupReply> reply;
    bool stopping = false;
    bool busy = false;
    bool signalled = false;
    std::thread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)), worker([this] { run(); }) {}
    ~State() {
        request_stop();
        join();
    }
    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        condition.notify_all();
    }
    void join() {
        if (worker.joinable()) worker.join();
    }
    void signal() {
        if (signalled || !wake) return;
        signalled = true;
        try { wake(); } catch (...) {}
    }
    void emit(FilesOperationRecoveryCleanupReply value) {
        std::lock_guard lock(mutex);
        if (stopping) return;
        reply = std::move(value);
        busy = false;
        signal();
    }
    FilesOperationRecoveryCleanupReply run_one(Request request) {
        auto file = open_native_operation_journal_file(request.root, request.journal_name, false);
        if (!file.value()) return make_error(*file.unavailable_detail());
        auto loaded = load_operation_journal(**file.value());
        if (!loaded.value()) return make_error(*loaded.unavailable_detail());
        if (loaded.value()->journal->id() != request.journal_id ||
            loaded.value()->journal->operation() != FileOperation::Copy ||
            loaded.value()->tail.has_value()) {
            auto detail = failure<bool>(AdapterError::PlanDecisionStale,
                                        "operation_recovery_cleanup.journal_changed",
                                        ObservationState::Changed);
            return make_error(*detail.unavailable_detail());
        }
        auto inspection = inspect_native_operation_recovery_evidence(
            **file.value(), {}, {}, {});
        if (!inspection.value()) return make_error(*inspection.unavailable_detail());
        if (inspection.value()->report.items.size() != 1 || inspection.value()->artifacts.size() != 1 ||
            inspection.value()->report.items.front().action != OperationRecoveryAction::InspectUnpublishedStaging ||
            (inspection.value()->report.items.front().state != OperationJournalState::Copied &&
             inspection.value()->report.items.front().state != OperationJournalState::Verified)) {
            auto detail = failure<bool>(AdapterError::PlanDecisionStale,
                                        "operation_recovery_cleanup.item_not_eligible",
                                        ObservationState::Unknown);
            return make_error(*detail.unavailable_detail());
        }
        const auto &artifact = inspection.value()->artifacts.front();
        if (!artifact.evidence || !artifact.evidence->staging_identity ||
            !artifact.evidence->destination_address) {
            auto detail = failure<bool>(AdapterError::UnqualifiedIdentity,
                                        "operation_recovery_cleanup.receipt_missing",
                                        ObservationState::Unknown);
            return make_error(*detail.unavailable_detail());
        }
        NativeFileSystem filesystem;
        auto destination = filesystem.open_native_directory(*artifact.evidence->destination_address);
        if (!destination.value()) return make_error(*destination.unavailable_detail());
        auto held_destination = std::shared_ptr<const DirectoryCursor>(std::move(destination).take_value());
        const auto destination_identity = held_destination->identity();
        if (!destination_identity.value() ||
            !artifact.evidence->destination_identity.matches_live(*destination_identity.value())) {
            auto detail = failure<bool>(AdapterError::PlanDecisionStale,
                                        "operation_recovery_cleanup.destination_changed",
                                        ObservationState::Changed);
            return make_error(*detail.unavailable_detail());
        }
        NativeOperationRecoveryCleanupRequest cleanup{
            artifact, std::move(held_destination), *destination_identity.value()};
        auto discarded = cleanup_native_unpublished_staging(cleanup);
        if (!discarded.value()) return make_error(*discarded.unavailable_detail());
        if (!*discarded.value()) {
            auto detail = failure<bool>(AdapterError::InvalidNativeResult,
                                        "operation_recovery_cleanup.discard_result");
            return make_error(*detail.unavailable_detail());
        }
        auto cancelled = append_operation_journal_file(
            **file.value(), *loaded.value()->journal,
            {static_cast<std::uint64_t>(loaded.value()->journal->records().size() + 1),
             inspection.value()->report.items.front().item, OperationJournalState::Cancelled, {}});
        if (!cancelled.value()) return make_error(*cancelled.unavailable_detail());
        return {FilesOperationRecoveryCleanupState::Complete, std::nullopt};
    }
    void run() {
        for (;;) {
            std::optional<Request> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) { pending.reset(); return; }
                job = std::move(*pending);
                pending.reset();
                busy = true;
                signalled = false;
            }
            try {
                emit(run_one(std::move(*job)));
            } catch (...) {
                emit({FilesOperationRecoveryCleanupState::WorkerFault,
                      UnavailableObservation{ObservationState::Failed,
                          {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::PlanInvalid),
                           "operation_recovery_cleanup.worker_fault"}}});
            }
        }
    }
};

FilesOperationRecoveryCleanupService::FilesOperationRecoveryCleanupService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesOperationRecoveryCleanupService::~FilesOperationRecoveryCleanupService() = default;

FilesOperationRecoveryCleanupSubmission FilesOperationRecoveryCleanupService::request(
    NativeAbsolutePath private_root, NativeName journal_name, OperationJournalId journal_id) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return FilesOperationRecoveryCleanupSubmission::Stopped;
    if (!private_root.is_valid() || !journal_name.is_valid())
        return FilesOperationRecoveryCleanupSubmission::Invalid;
    if (state_->busy || state_->pending) return FilesOperationRecoveryCleanupSubmission::Busy;
    state_->pending.emplace(State::Request{std::move(private_root), std::move(journal_name), journal_id});
    state_->condition.notify_one();
    return FilesOperationRecoveryCleanupSubmission::Accepted;
}
std::optional<FilesOperationRecoveryCleanupReply> FilesOperationRecoveryCleanupService::take() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->reply);
    state_->reply.reset();
    state_->signalled = false;
    return result;
}
void FilesOperationRecoveryCleanupService::request_stop() noexcept { state_->request_stop(); }
void FilesOperationRecoveryCleanupService::join() { state_->join(); }
bool FilesOperationRecoveryCleanupService::running() const noexcept {
    std::lock_guard lock(state_->mutex);
    return !state_->stopping;
}
bool FilesOperationRecoveryCleanupService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

} // namespace ck::vision
