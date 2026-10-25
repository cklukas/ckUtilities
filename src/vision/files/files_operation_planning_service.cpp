// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_planning_service.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace ck::vision {
namespace {

bool valid(const FilesOperationPlanningRequest& request) noexcept {
    return request.inventory && request.destination_batch &&
           request.source_target.size() == request.inventory->entries().size() &&
           request.ancestry.size() == request.inventory->entries().size() &&
           request.source_objects.size() == request.inventory->entries().size() &&
           request.destination_capabilities.size() <= request.inventory->entries().size() &&
           request.review_limits.max_rows != 0;
}

bool valid(const FilesOperationNativePlanningRequest& request) noexcept {
    const bool recursive = request.evidence.root_source && request.evidence.source_root && request.evidence.source_path &&
        request.evidence.source_path->is_valid() && request.evidence.direct_sources.empty() &&
        request.evidence.direct_source_paths.empty();
    const bool flat = !request.evidence.root_source && !request.evidence.source_root &&
        !request.evidence.source_path && !request.evidence.direct_sources.empty() &&
        request.evidence.direct_sources.size() == request.evidence.direct_source_paths.size();
    return (recursive || flat) && request.evidence.destination && request.evidence.clock &&
           request.evidence.limits.max_items != 0 &&
           request.evidence.limits.max_items <= ck::files::OperationEvidenceJobLimits::hard_max_items &&
           (!flat || request.evidence.direct_sources.size() <= request.evidence.limits.max_items);
}

bool limit_error(const ck::files::UnavailableObservation& detail) noexcept {
    return detail.error.domain == ck::files::ErrorDomain::Adapter &&
           detail.error.code == static_cast<std::int64_t>(ck::files::AdapterError::InventoryLimit);
}

} // namespace

struct FilesOperationPlanningService::State final {
    using Input = std::variant<FilesOperationPlanningRequest, FilesOperationNativePlanningRequest>;
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Input> pending;
    std::optional<FilesOperationPlanningReply> reply;
    FilesOperationPlanningTicket latest, cancelled;
    std::optional<std::stop_source> active_stop;
    bool busy = false;
    bool stopping = false;
    bool signalled = false;
    std::uint64_t next = 1;
    std::atomic<bool> alive{true};
    std::atomic<FilesPaneWorkerFault> worker_fault{FilesPaneWorkerFault::None};
    std::jthread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)) {}

    void notify(bool needed) {
        if (needed && wake)
            wake();
    }

    void emit(FilesOperationPlanningReply result) {
        bool needed = false;
        {
            std::lock_guard lock(mutex);
            busy = false;
            active_stop.reset();
            if (stopping || result.request != latest)
                return;
            if (result.request == cancelled && result.state == FilesOperationPlanningReplyState::Ready) {
                result.state = FilesOperationPlanningReplyState::Cancelled;
                result.plan.reset();
                result.review.reset();
            }
            reply = std::move(result);
            needed = !std::exchange(signalled, true);
        }
        notify(needed);
    }

    void finish(FilesPaneWorkerFault reason) noexcept {
        bool needed = false;
        {
            std::lock_guard lock(mutex);
            worker_fault.store(reason, std::memory_order_release);
            busy = false;
            pending.reset();
            active_stop.reset();
            if (!stopping && reason != FilesPaneWorkerFault::None) {
                FilesOperationPlanningReply fatal;
                fatal.request = latest;
                fatal.state = FilesOperationPlanningReplyState::WorkerFault;
                fatal.worker_fault = reason;
                reply = std::move(fatal);
                signalled = true;
            }
            alive.store(false, std::memory_order_release);
            needed = reason != FilesPaneWorkerFault::None || !signalled;
            signalled = true;
        }
        try {
            notify(needed);
        } catch (...) {
            if (reason == FilesPaneWorkerFault::None)
                worker_fault.store(FilesPaneWorkerFault::Exception, std::memory_order_release);
        }
    }

    void run() noexcept {
        try {
            loop();
            finish(FilesPaneWorkerFault::None);
        } catch (const std::bad_alloc&) {
            finish(FilesPaneWorkerFault::Allocation);
        } catch (...) {
            finish(FilesPaneWorkerFault::Exception);
        }
    }

    FilesOperationPlanningSubmitted submit(Input request) {
        const bool valid_request = std::visit([](const auto& value) { return valid(value); }, request);
        std::lock_guard lock(mutex);
        if (!valid_request)
            return {FilesOperationPlanningSubmission::Invalid, {}};
        if (stopping || !alive.load(std::memory_order_acquire))
            return {FilesOperationPlanningSubmission::Stopped, {}};
        if (pending || busy || reply)
            return {FilesOperationPlanningSubmission::Busy, {}};
        if (!next)
            return {FilesOperationPlanningSubmission::Exhausted, {}};
        const auto request_ticket = latest = FilesOperationPlanningService::ticket(next++);
        cancelled = {};
        signalled = false;
        pending.emplace(std::move(request));
        condition.notify_one();
        return {FilesOperationPlanningSubmission::Accepted, request_ticket};
    }

    void loop() {
        for (;;) {
            std::optional<Input> request;
            FilesOperationPlanningTicket ticket;
            std::stop_source stop;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping || pending.has_value(); });
                if (stopping)
                    return;
                request = std::move(pending);
                pending.reset();
                ticket = latest;
                busy = true;
                active_stop = stop;
                if (ticket == cancelled)
                    stop.request_stop();
            }

            FilesOperationPlanningReply result;
            result.request = ticket;
            if (stop.stop_requested()) {
                result.state = FilesOperationPlanningReplyState::Cancelled;
            } else {
                // A provisional non-terminal state lets the shared completion
                // path distinguish an uncomputed plan from a native-stage
                // terminal reply.
                result.state = FilesOperationPlanningReplyState::Ready;
                ck::files::OperationReviewLimits review_limits;
                ck::files::Observation<ck::files::OperationPlan::Pointer> plan;
                if (auto* value = std::get_if<FilesOperationPlanningRequest>(&*request)) {
                    review_limits = value->review_limits;
                    plan = ck::files::OperationPlanningCoordinator::assemble(
                        value->operation, std::move(value->inventory), value->source_target,
                        value->ancestry, value->source_objects, *value->destination_batch,
                        value->destination_capabilities, value->projection_policy, value->limits);
                } else {
                    auto native = std::move(std::get<FilesOperationNativePlanningRequest>(*request));
                    review_limits = native.review_limits;
                    try {
                        ck::files::OperationEvidenceJob evidence(std::move(native.evidence));
                        for (;;) {
                            const auto state = evidence.advance(stop.get_token());
                            if (state == ck::files::OperationEvidenceJobState::Paused || stop.stop_requested()) {
                                result.state = FilesOperationPlanningReplyState::Cancelled;
                                break;
                            }
                            if (state == ck::files::OperationEvidenceJobState::Failed) {
                                if (evidence.failure()) result.error = *evidence.failure();
                                result.state = result.error && limit_error(*result.error)
                                    ? FilesOperationPlanningReplyState::LimitExceeded
                                    : FilesOperationPlanningReplyState::Failed;
                                break;
                            }
                            if (state == ck::files::OperationEvidenceJobState::Complete) {
                                auto completed = evidence.completed();
                                if (!completed.value()) {
                                    result.error = completed.unavailable_detail()
                                        ? *completed.unavailable_detail()
                                        : ck::files::UnavailableObservation{ck::files::ObservationState::Failed,
                                            {ck::files::ErrorDomain::Adapter,
                                                static_cast<std::int64_t>(ck::files::AdapterError::PlanInvalid),
                                                "files_planning.native_result"}};
                                    result.state = FilesOperationPlanningReplyState::Failed;
                                    break;
                                }
                                const auto& native_result = *completed.value();
                                plan = ck::files::OperationPlanningCoordinator::assemble(
                                    native_result->operation(), native_result->inventory(), native_result->source_target(),
                                    native_result->ancestry(), native_result->source_objects(),
                                    *native_result->destination_batch(), native_result->destination_capabilities(),
                                    native.projection_policy, native.limits);
                                break;
                            }
                        }
                    } catch (const std::invalid_argument&) {
                        result.error = ck::files::UnavailableObservation{ck::files::ObservationState::Failed,
                            {ck::files::ErrorDomain::Adapter,
                                static_cast<std::int64_t>(ck::files::AdapterError::PlanInvalid),
                                "files_planning.native_request"}};
                        result.state = FilesOperationPlanningReplyState::Failed;
                    } catch (const std::length_error&) {
                        result.error = ck::files::UnavailableObservation{ck::files::ObservationState::Unknown,
                            {ck::files::ErrorDomain::Adapter,
                                static_cast<std::int64_t>(ck::files::AdapterError::InventoryLimit),
                                "files_planning.native_request_limit"}};
                        result.state = FilesOperationPlanningReplyState::LimitExceeded;
                    }
                }
                if (result.state == FilesOperationPlanningReplyState::Cancelled ||
                    result.state == FilesOperationPlanningReplyState::Failed ||
                    result.state == FilesOperationPlanningReplyState::LimitExceeded)
                    ;
                else if (!plan.value()) {
                    result.error = *plan.unavailable_detail();
                    result.state = limit_error(*result.error) ? FilesOperationPlanningReplyState::LimitExceeded
                                                                : FilesOperationPlanningReplyState::Failed;
                } else if (stop.stop_requested()) {
                    result.state = FilesOperationPlanningReplyState::Cancelled;
                } else {
                    auto review = ck::files::OperationReviewModel::create(*plan.value(), review_limits);
                    if (!review.value()) {
                        result.error = *review.unavailable_detail();
                        result.state = limit_error(*result.error) ? FilesOperationPlanningReplyState::LimitExceeded
                                                                    : FilesOperationPlanningReplyState::Failed;
                    } else {
                        result.state = FilesOperationPlanningReplyState::Ready;
                        result.plan = std::move(plan).take_value();
                        result.review = std::move(review).take_value();
                    }
                }
            }
            emit(std::move(result));
        }
    }
};

FilesOperationPlanningService::FilesOperationPlanningService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {
    state_->worker = std::jthread([state = state_.get()] { state->run(); });
}

FilesOperationPlanningService::~FilesOperationPlanningService() { join(); }

FilesOperationPlanningSubmitted FilesOperationPlanningService::request(FilesOperationPlanningRequest request) {
    return state_->submit(FilesOperationPlanningService::State::Input(std::move(request)));
}

FilesOperationPlanningSubmitted FilesOperationPlanningService::request(FilesOperationNativePlanningRequest request) {
    return state_->submit(FilesOperationPlanningService::State::Input(std::move(request)));
}

bool FilesOperationPlanningService::cancel(const FilesOperationPlanningTicket& request) {
    std::optional<std::stop_source> stop;
    bool notify = false;
    {
        std::lock_guard lock(state_->mutex);
        if (!request.valid() || request != state_->latest || state_->stopping || !state_->alive.load() ||
            (!state_->pending && !state_->busy && !state_->reply))
            return false;
        if (state_->reply && state_->reply->state != FilesOperationPlanningReplyState::Ready)
            return false;
        state_->cancelled = request;
        stop = state_->active_stop;
        if (state_->reply) {
            state_->reply->state = FilesOperationPlanningReplyState::Cancelled;
            state_->reply->plan.reset();
            state_->reply->review.reset();
            state_->reply->error.reset();
            notify = !std::exchange(state_->signalled, true);
        }
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
    state_->notify(notify);
    return true;
}

std::optional<FilesOperationPlanningReply> FilesOperationPlanningService::take() {
    std::lock_guard lock(state_->mutex);
    if (!state_->reply)
        return std::nullopt;
    auto result = std::move(state_->reply);
    state_->reply.reset();
    state_->signalled = false;
    return result;
}

void FilesOperationPlanningService::request_stop() noexcept {
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->pending.reset();
        state_->reply.reset();
        if (state_->active_stop)
            state_->active_stop->request_stop();
    }
    state_->condition.notify_one();
}

void FilesOperationPlanningService::join() {
    request_stop();
    if (state_->worker.joinable())
        state_->worker.join();
}

bool FilesOperationPlanningService::running() const noexcept {
    return state_->alive.load(std::memory_order_acquire);
}

bool FilesOperationPlanningService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

FilesPaneWorkerFault FilesOperationPlanningService::fault() const noexcept {
    return state_->worker_fault.load(std::memory_order_acquire);
}

} // namespace ck::vision
