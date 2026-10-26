// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_execution_service.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace ck::vision {
namespace {

ck::files::UnavailableObservation error(ck::files::AdapterError code, const char* operation,
                                        ck::files::ObservationState state = ck::files::ObservationState::Failed) {
    return {state, {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

bool valid(const FilesOperationExecutionRequest& request) noexcept {
    return (request.operation == ck::files::FileOperation::Copy ||
            request.operation == ck::files::FileOperation::Move) &&
           request.source_service && request.destination_service &&
           request.source_service != request.destination_service && request.source_navigation.valid() &&
           request.destination_navigation.valid() && request.source_stamp.current() &&
           request.destination_stamp.current() && request.source_limits.held_entries != 0 &&
           request.journal_root.is_valid() && request.open_journal && request.policy.copy_limits.valid() &&
           request.policy.journal_limits.max_items != 0;
}

} // namespace

struct FilesOperationExecutionService::State final {
    struct Handoff final {
        std::mutex mutex;
        std::condition_variable condition;
        std::optional<FilesPaneOperationSources> sources;
        std::optional<std::shared_ptr<const ck::files::DirectoryCursor>> destination_cursor;
        std::optional<ck::files::UnavailableObservation> failure;
        bool source_done = false;
        bool destination_done = false;

        void source(ck::files::Observation<FilesPaneOperationSources> value) {
            std::lock_guard lock(mutex);
            if (failure || source_done)
                return;
            if (value.value())
                sources = std::move(value).take_value();
            else
                failure = value.unavailable_detail()
                    ? *value.unavailable_detail()
                    : error(ck::files::AdapterError::PlanInvalid, "files_execution.source_handoff");
            source_done = true;
            condition.notify_all();
        }

        void destination(ck::files::Observation<std::shared_ptr<const ck::files::DirectoryCursor>> value) {
            std::lock_guard lock(mutex);
            if (failure || destination_done)
                return;
            if (value.value())
                destination_cursor = std::move(value).take_value();
            else
                failure = value.unavailable_detail()
                    ? *value.unavailable_detail()
                    : error(ck::files::AdapterError::PlanInvalid, "files_execution.destination_handoff");
            destination_done = true;
            condition.notify_all();
        }

        bool wait(std::stop_token stop_token) {
            std::unique_lock lock(mutex);
            condition.wait_for(lock, std::chrono::milliseconds(10), [&] {
                return failure || (source_done && destination_done) || stop_token.stop_requested();
            });
            return !stop_token.stop_requested() && !failure && source_done && destination_done;
        }
    };

    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::pair<FilesOperationExecutionTicket, FilesOperationExecutionRequest>> pending;
    std::optional<FilesOperationExecutionReply> reply;
    FilesOperationExecutionTicket latest, cancelled;
    std::uint64_t next = 1;
    std::stop_source stop;
    std::atomic<bool> alive{true};
    bool stopping = false;
    bool busy = false;
    bool signalled = false;
    std::thread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)), worker([this] { run(); }) {}

    ~State() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            stop.request_stop();
            condition.notify_all();
        }
        if (worker.joinable())
            worker.join();
    }

    void signal() {
        if (signalled || !wake)
            return;
        signalled = true;
        try { wake(); } catch (...) {}
    }

    void emit(FilesOperationExecutionReply value) {
        std::lock_guard lock(mutex);
        if (value.request != latest || stopping)
            return;
        reply = std::move(value);
        busy = false;
        signal();
    }

    void run_one(FilesOperationExecutionTicket request, FilesOperationExecutionRequest value,
                 std::stop_token stop_token) {
        FilesOperationExecutionReply result;
        result.request = request;
        auto fail = [&](ck::files::UnavailableObservation detail) {
            result.state = stop_token.stop_requested() ? FilesOperationExecutionState::Cancelled
                                                        : FilesOperationExecutionState::Failed;
            result.error = std::move(detail);
            emit(std::move(result));
        };

        auto handoff = std::make_shared<Handoff>();
        const auto source_submission = value.source_service->acquire_operation_sources(
            value.source_navigation, value.source_stamp, value.source_limits,
            [handoff](ck::files::Observation<FilesPaneOperationSources> sources) {
                handoff->source(std::move(sources));
            });
        const auto destination_submission = value.destination_service->acquire_operation_destination(
            value.destination_navigation, value.destination_stamp,
            [handoff](ck::files::Observation<std::shared_ptr<const ck::files::DirectoryCursor>> destination) {
                handoff->destination(std::move(destination));
            });
        if (source_submission.state != FilesPaneSubmission::Accepted ||
            destination_submission.state != FilesPaneSubmission::Accepted) {
            if (source_submission.state == FilesPaneSubmission::Accepted)
                value.source_service->cancel(source_submission.ticket);
            if (destination_submission.state == FilesPaneSubmission::Accepted)
                value.destination_service->cancel(destination_submission.ticket);
            fail(error(ck::files::AdapterError::InvalidAccess, "files_execution.pane_submission",
                       ck::files::ObservationState::Changed));
            return;
        }
        if (!handoff->wait(stop_token)) {
            value.source_service->cancel(source_submission.ticket);
            value.destination_service->cancel(destination_submission.ticket);
            if (handoff->failure)
                fail(*handoff->failure);
            else
                fail(error(ck::files::AdapterError::InterruptedRead, "files_execution.handoff_cancelled",
                           ck::files::ObservationState::Unknown));
            return;
        }
        if (!handoff->sources || !handoff->destination_cursor) {
            fail(error(ck::files::AdapterError::PlanInvalid, "files_execution.handoff_shape"));
            return;
        }
        const bool recursive = handoff->sources->root_source && handoff->sources->source_root &&
            handoff->sources->source_path && handoff->sources->source_path->is_valid() &&
            handoff->sources->sources.empty() && handoff->sources->paths.empty();
        const bool flat = !handoff->sources->root_source && !handoff->sources->source_root &&
            !handoff->sources->source_path && !handoff->sources->sources.empty() &&
            handoff->sources->sources.size() == handoff->sources->names.size();
        if (!recursive && !flat) {
            fail(error(ck::files::AdapterError::PlanInvalid,
                       "files_execution.source_handoff_shape"));
            return;
        }
        if (value.operation == ck::files::FileOperation::Move) {
            std::vector<ck::files::HeldChild::Pointer> sources;
            std::vector<ck::files::NativeName> names;
            if (handoff->sources->root_source) {
                sources.push_back(std::move(handoff->sources->root_source));
                names.push_back(sources.front()->name());
            } else {
                sources = std::move(handoff->sources->sources);
                names = std::move(handoff->sources->names);
            }
            if (sources.empty() || sources.size() != names.size()) {
                fail(error(ck::files::AdapterError::PlanInvalid,
                           "files_execution.move_source_shape"));
                return;
            }
            ck::files::NativeOperationMoveBatchRequest move;
            move.sources = std::move(sources);
            move.destination = std::move(*handoff->destination_cursor);
            move.destination_names = std::move(names);
            move.journal_root = std::move(value.journal_root);
            move.journal_id = value.journal_id;
            move.journal_limits = value.policy.journal_limits;
            move.open_journal = std::move(value.open_journal);
            auto batch = ck::files::create_native_operation_move_batch(std::move(move));
            if (!batch.value()) {
                fail(*batch.unavailable_detail());
                return;
            }
            auto job = std::move(batch).take_value();
            while (job->state() != ck::files::OperationBatchState::Complete &&
                   job->state() != ck::files::OperationBatchState::Failed &&
                   job->state() != ck::files::OperationBatchState::Cancelled &&
                   job->state() != ck::files::OperationBatchState::NeedsReconciliation)
                job->advance(stop_token);
            result.result = job->result();
            result.state = job->state() == ck::files::OperationBatchState::Complete
                ? FilesOperationExecutionState::Complete
                : stop_token.stop_requested() || job->state() == ck::files::OperationBatchState::Cancelled
                    ? FilesOperationExecutionState::Cancelled : FilesOperationExecutionState::Failed;
            if (result.state != FilesOperationExecutionState::Complete && !result.result.items.empty())
                result.error = result.result.items.back().failure;
            emit(std::move(result));
            return;
        }
        if (recursive) {
            ck::files::RecursiveInventoryLimits limits;
            limits.inventory.max_entries = value.source_limits.held_entries;
            auto inventory_job = std::make_unique<ck::files::RecursiveInventoryJob>(
                std::move(handoff->sources->source_root),
                std::move(*handoff->sources->source_path),
                ck::files::InventoryMountPolicy::StayOnRootMount,
                ck::files::InventoryLinkPolicy::PreserveLinks,
                limits, ck::files::system_monotonic_clock(),
                std::move(handoff->sources->root_source));
            while (inventory_job->state() != ck::files::RecursiveInventoryState::Complete &&
                   inventory_job->state() != ck::files::RecursiveInventoryState::Failed) {
                if (stop_token.stop_requested()) {
                    fail(error(ck::files::AdapterError::InterruptedRead,
                               "files_execution.recursive_inventory_cancelled",
                               ck::files::ObservationState::Unknown));
                    return;
                }
                inventory_job->advance(stop_token);
            }
            if (inventory_job->state() != ck::files::RecursiveInventoryState::Complete) {
                if (inventory_job->failure()) fail(*inventory_job->failure());
                else fail(error(ck::files::AdapterError::InvalidNativeResult,
                                "files_execution.recursive_inventory"));
                return;
            }
            auto completed = inventory_job->completed_sources();
            if (!completed.value()) {
                fail(completed.unavailable_detail() ? *completed.unavailable_detail() :
                     error(ck::files::AdapterError::InvalidNativeResult,
                           "files_execution.recursive_inventory_result"));
                return;
            }
            ck::files::NativeOperationCopyTreeRequest tree_request(
                (*completed.value())->sources().front()->name());
            tree_request.inventory = std::move(completed).take_value();
            tree_request.destination = std::move(*handoff->destination_cursor);
            tree_request.journal_root = std::move(value.journal_root);
            tree_request.journal_id = value.journal_id;
            tree_request.policy = value.policy;
            tree_request.open_journal = std::move(value.open_journal);
            auto tree = ck::files::create_native_operation_copy_tree(std::move(tree_request));
            if (!tree.value()) {
                fail(*tree.unavailable_detail());
                return;
            }
            auto job = std::move(tree).take_value();
            while (job->state() != ck::files::OperationExecutionState::Complete &&
                   job->state() != ck::files::OperationExecutionState::Failed &&
                   job->state() != ck::files::OperationExecutionState::Cancelled &&
                   job->state() != ck::files::OperationExecutionState::NeedsReconciliation)
                job->advance(stop_token);
            const auto tree_state = job->state();
            const auto outcome = tree_state == ck::files::OperationExecutionState::Complete
                ? ck::files::OperationItemOutcome::Complete
                : tree_state == ck::files::OperationExecutionState::Cancelled
                    ? ck::files::OperationItemOutcome::Cancelled
                    : tree_state == ck::files::OperationExecutionState::NeedsReconciliation
                        ? ck::files::OperationItemOutcome::NeedsReconciliation
                        : ck::files::OperationItemOutcome::Failed;
            result.result.items.push_back({0, outcome, job->result().failure});
            result.result.state = outcome == ck::files::OperationItemOutcome::Complete
                ? ck::files::OperationBatchState::Complete
                : outcome == ck::files::OperationItemOutcome::Cancelled
                    ? ck::files::OperationBatchState::Cancelled
                    : outcome == ck::files::OperationItemOutcome::NeedsReconciliation
                        ? ck::files::OperationBatchState::NeedsReconciliation
                        : ck::files::OperationBatchState::Failed;
            result.state = outcome == ck::files::OperationItemOutcome::Complete
                ? FilesOperationExecutionState::Complete
                : stop_token.stop_requested() || outcome == ck::files::OperationItemOutcome::Cancelled
                    ? FilesOperationExecutionState::Cancelled : FilesOperationExecutionState::Failed;
            if (result.state != FilesOperationExecutionState::Complete)
                result.error = job->result().failure;
            emit(std::move(result));
            return;
        }
        ck::files::NativeOperationCopyBatchRequest copy;
        copy.sources = std::move(handoff->sources->sources);
        copy.destination = std::move(*handoff->destination_cursor);
        copy.destination_names = std::move(handoff->sources->names);
        copy.journal_root = std::move(value.journal_root);
        copy.journal_id = value.journal_id;
        copy.policy = value.policy;
        copy.open_journal = std::move(value.open_journal);
        auto batch = ck::files::create_native_operation_copy_batch(std::move(copy));
        if (!batch.value()) {
            fail(*batch.unavailable_detail());
            return;
        }
        auto job = std::move(batch).take_value();
        while (job->state() != ck::files::OperationBatchState::Complete &&
               job->state() != ck::files::OperationBatchState::Failed &&
               job->state() != ck::files::OperationBatchState::Cancelled &&
               job->state() != ck::files::OperationBatchState::RetainedSource &&
               job->state() != ck::files::OperationBatchState::NeedsReconciliation)
            job->advance(stop_token);
        result.result = job->result();
        result.state = job->state() == ck::files::OperationBatchState::Complete
            ? FilesOperationExecutionState::Complete
            : stop_token.stop_requested() || job->state() == ck::files::OperationBatchState::Cancelled
                ? FilesOperationExecutionState::Cancelled : FilesOperationExecutionState::Failed;
        if (result.state != FilesOperationExecutionState::Complete && !result.result.items.empty())
            result.error = result.result.items.back().failure;
        emit(std::move(result));
    }

    void run() noexcept {
        try {
            while (true) {
                std::optional<std::pair<FilesOperationExecutionTicket, FilesOperationExecutionRequest>> item;
                {
                    std::unique_lock lock(mutex);
                    condition.wait(lock, [&] { return stopping || pending.has_value(); });
                    if (stopping && !pending)
                        break;
                    item.emplace(std::move(*pending));
                    pending.reset();
                    busy = true;
                    stop = std::stop_source{};
                }
                run_one(item->first, std::move(item->second), stop.get_token());
            }
        } catch (const std::bad_alloc&) {
            FilesOperationExecutionReply result;
            result.request = latest;
            result.state = FilesOperationExecutionState::WorkerFault;
            result.worker_fault = FilesPaneWorkerFault::Allocation;
            emit(std::move(result));
        } catch (...) {
            FilesOperationExecutionReply result;
            result.request = latest;
            result.state = FilesOperationExecutionState::WorkerFault;
            result.worker_fault = FilesPaneWorkerFault::Exception;
            emit(std::move(result));
        }
        alive.store(false, std::memory_order_release);
        std::lock_guard lock(mutex);
        signal();
    }

    FilesOperationExecutionSubmitted submit(FilesOperationExecutionRequest value) {
        if (!valid(value))
            return {FilesOperationExecutionSubmission::Invalid, {}};
        std::lock_guard lock(mutex);
        if (stopping || !alive.load(std::memory_order_acquire))
            return {FilesOperationExecutionSubmission::Stopped, {}};
        if (pending || busy || reply)
            return {FilesOperationExecutionSubmission::Busy, {}};
        if (!next)
            return {FilesOperationExecutionSubmission::Exhausted, {}};
        const auto request = latest = FilesOperationExecutionService::ticket(next++);
        cancelled = {};
        signalled = false;
        pending.emplace(request, std::move(value));
        condition.notify_one();
        return {FilesOperationExecutionSubmission::Accepted, request};
    }
};

FilesOperationExecutionService::FilesOperationExecutionService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesOperationExecutionService::~FilesOperationExecutionService() = default;
FilesOperationExecutionSubmitted FilesOperationExecutionService::request(FilesOperationExecutionRequest request) {
    return state_->submit(std::move(request));
}
bool FilesOperationExecutionService::cancel(const FilesOperationExecutionTicket& request) {
    std::lock_guard lock(state_->mutex);
    if (!request.valid() || request != state_->latest || state_->stopping ||
        (!state_->pending && !state_->busy))
        return false;
    state_->cancelled = request;
    state_->stop.request_stop();
    state_->condition.notify_one();
    return true;
}
std::optional<FilesOperationExecutionReply> FilesOperationExecutionService::take() {
    std::lock_guard lock(state_->mutex);
    if (!state_->reply)
        return std::nullopt;
    state_->signalled = false;
    auto result = std::move(state_->reply);
    state_->reply.reset();
    return result;
}
void FilesOperationExecutionService::request_stop() noexcept {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping)
        return;
    state_->stopping = true;
    state_->stop.request_stop();
    state_->condition.notify_all();
}
void FilesOperationExecutionService::join() {
    if (state_->worker.joinable())
        state_->worker.join();
}
bool FilesOperationExecutionService::running() const noexcept {
    return state_->alive.load(std::memory_order_acquire);
}
bool FilesOperationExecutionService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->pending.has_value() || state_->busy || state_->reply.has_value();
}

} // namespace ck::vision
