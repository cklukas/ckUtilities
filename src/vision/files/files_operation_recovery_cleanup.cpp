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
    FilesOperationRecoveryCleanupReply run_one(const Request &) {
        // This service receives restart inspection facts only. Matching names,
        // IDs and journal bytes cannot recreate the original creator's held
        // staging owner. Refuse before reopening or changing any native object.
        auto detail = failure<bool>(AdapterError::UnqualifiedIdentity,
            "operation_recovery_cleanup.live_creator_ownership_required",
            ObservationState::Unknown);
        return make_error(*detail.unavailable_detail());
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
