// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_planning_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace ck::vision {
namespace {

template<class T>
ck::files::Observation<T> evidence_error(ck::files::AdapterError code, const char* operation,
    ck::files::ObservationState state = ck::files::ObservationState::Failed) {
    return ck::files::Observation<T>::unavailable(state,
        {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

template<class T>
bool same_observation(const ck::files::Observation<T>& left, const ck::files::Observation<T>& right) noexcept {
    if (left.value() && right.value())
        return *left.value() == *right.value();
    return left.unavailable_detail() && right.unavailable_detail() && left.state() == right.state() &&
        left.unavailable_detail()->error == right.unavailable_detail()->error;
}

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

bool valid(const FilesOperationPanePlanningRequest& request) noexcept {
    return (request.operation == ck::files::FileOperation::Copy ||
            request.operation == ck::files::FileOperation::Move) &&
           request.source_service && request.destination_service &&
           request.source_service != request.destination_service && request.source_navigation.valid() &&
           request.destination_navigation.valid() && request.source_stamp.current() &&
           request.destination_stamp.current() && request.source_limits.held_entries != 0 &&
           request.evidence_limits.max_items != 0 &&
           request.source_limits.held_entries <= request.evidence_limits.max_items &&
           request.evidence_limits.max_items <= ck::files::OperationEvidenceJobLimits::hard_max_items;
}

bool limit_error(const ck::files::UnavailableObservation& detail) noexcept {
    return detail.error.domain == ck::files::ErrorDomain::Adapter &&
           detail.error.code == static_cast<std::int64_t>(ck::files::AdapterError::InventoryLimit);
}

} // namespace

bool FilesOperationReviewEvidence::same_namespace_and_sources(const FilesOperationReviewEvidence& fresh) const noexcept {
    using namespace ck::files;
    if (!inventory || !fresh.inventory || compare_identity(destination, fresh.destination) != IdentityComparison::Same ||
        destination_names != fresh.destination_names || inventory->mount_policy() != fresh.inventory->mount_policy() ||
        inventory->link_policy() != fresh.inventory->link_policy() ||
        source_times.size() != inventory->entries().size() || fresh.source_times.size() != fresh.inventory->entries().size())
        return false;
    const bool same_entries = std::equal(inventory->entries().begin(), inventory->entries().end(),
        fresh.inventory->entries().begin(), fresh.inventory->entries().end(), [](const auto& left, const auto& right) {
            const bool same_target = left.link_target && right.link_target
                ? compare_identity(*left.link_target, *right.link_target) == IdentityComparison::Same
                : !left.link_target && !right.link_target;
            return left.source_path == right.source_path && left.relative_components == right.relative_components &&
                compare_identity(left.identity, right.identity) == IdentityComparison::Same && left.kind == right.kind &&
                left.logical_size == right.logical_size && left.link == right.link && same_target &&
                same_observation(left.mount_id, right.mount_id);
        });
    return same_entries && std::equal(source_times.begin(), source_times.end(), fresh.source_times.begin(),
        [](const auto& left, const auto& right) {
            return same_observation(left.modified, right.modified) && same_observation(left.changed, right.changed);
        });
}

ck::files::Observation<ck::files::OperationEvidenceJobRequest> files_operation_evidence_request(
    ck::files::FileOperation operation, FilesPaneOperationSources sources,
    const FilesPaneOperationDestination& destination) {
    using namespace ck::files;
    const bool recursive = sources.root_source && sources.source_root && sources.source_path &&
        sources.source_path->is_valid() && sources.sources.empty() && sources.paths.empty() && sources.names.size() == 1;
    const bool flat = !sources.root_source && !sources.source_root && !sources.source_path &&
        !sources.sources.empty() && sources.sources.size() == sources.paths.size() &&
        sources.sources.size() == sources.names.size();
    if (!destination.directory || !destination.provider_root || (!recursive && !flat))
        return evidence_error<OperationEvidenceJobRequest>(AdapterError::PlanInvalid, "files_planning.pane_handoff_shape");
    const auto encoding = sources.names.front().encoding();
    for (const auto& name : sources.names)
        if (name.encoding() != encoding)
            return evidence_error<OperationEvidenceJobRequest>(AdapterError::InvalidIdentifier, "files_planning.pane_name_encoding");
    OperationEvidenceJobRequest request;
    request.operation = operation;
    request.root_source = std::move(sources.root_source);
    request.source_root = std::move(sources.source_root);
    request.source_path = std::move(sources.source_path);
    request.direct_sources = std::move(sources.sources);
    request.direct_source_paths = std::move(sources.paths);
    request.destination = destination.directory;
    if (recursive)
        request.provider_root = destination.provider_root;
    request.destination_names = std::move(sources.names);
    request.destination_encoding = encoding;
    return Observation<OperationEvidenceJobRequest>::available(std::move(request));
}

ck::files::Observation<FilesOperationReviewEvidence::Pointer> collect_files_operation_evidence(
    FilesOperationNativePlanningRequest request, std::stop_token stop,
    std::shared_ptr<const ck::files::CompletedRecursiveInventory>* sealed_sources) {
    using namespace ck::files;
    using Result = FilesOperationReviewEvidence::Pointer;
    if (sealed_sources)
        sealed_sources->reset();
    if (!valid(request))
        return evidence_error<Result>(AdapterError::PlanInvalid, "files_planning.native_request");
    const auto destination = request.evidence.destination->identity();
    if (!destination.value())
        return Observation<Result>::unavailable(destination.state(), destination.unavailable_detail()->error);
    if (compare_identity(*destination.value(), *destination.value()) != IdentityComparison::Same)
        return evidence_error<Result>(AdapterError::UnqualifiedIdentity, "files_planning.destination_identity", ObservationState::Unknown);
    auto names = request.evidence.destination_names;
    const auto mount_policy = request.evidence.mount_policy;
    const auto link_policy = request.evidence.link_policy;
    const auto evidence_limits = request.evidence.limits;
    std::shared_ptr<const CompletedRecursiveInventory> sources;
    OperationEvidenceJob evidence(std::move(request.evidence));
    for (;;) {
        const auto state = evidence.advance(stop, sealed_sources ? &sources : nullptr);
        if (state == OperationEvidenceJobState::Paused || stop.stop_requested())
            return evidence_error<Result>(AdapterError::InterruptedRead, "files_planning.native_cancelled", ObservationState::Unknown);
        if (state == OperationEvidenceJobState::Failed)
            return Observation<Result>::unavailable(evidence.failure()->state, evidence.failure()->error);
        if (state == OperationEvidenceJobState::Complete)
            break;
    }
    const auto completed = evidence.completed();
    if (!completed.value())
        return Observation<Result>::unavailable(completed.state(), completed.unavailable_detail()->error);
    const auto& facts = **completed.value();
    auto plan = OperationPlanningCoordinator::assemble(facts.operation(), facts.inventory(), facts.source_target(),
        facts.ancestry(), facts.source_objects(), *facts.destination_batch(), facts.destination_capabilities(),
        request.projection_policy, request.limits);
    if (!plan.value())
        return Observation<Result>::unavailable(plan.state(), plan.unavailable_detail()->error);
    std::vector<FilesOperationReviewEvidence::SourceTimes> times;
    times.reserve(facts.source_objects().size());
    for (const auto& object : facts.source_objects()) {
        if (!object.after.value())
            return Observation<Result>::unavailable(object.after.state(), object.after.unavailable_detail()->error);
        const auto& after = *object.after.value();
        times.push_back({after.modified, after.changed});
    }
    auto result = std::make_shared<const FilesOperationReviewEvidence>(FilesOperationReviewEvidence{
        std::move(plan).take_value(), facts.inventory(), *destination.value(), std::move(names), std::move(times),
        mount_policy, link_policy, evidence_limits, request.projection_policy, request.limits});
    if (sealed_sources)
        *sealed_sources = std::move(sources);
    return Observation<Result>::available(std::move(result));
}

struct FilesOperationPlanningService::State final {
    struct PaneHandoff final {
        std::mutex mutex;
        std::condition_variable condition;
        std::optional<FilesPaneOperationSources> sources;
        std::optional<FilesPaneOperationDestination> destination_result;
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
                    : ck::files::UnavailableObservation{ck::files::ObservationState::Failed,
                        {ck::files::ErrorDomain::Adapter,
                         static_cast<std::int64_t>(ck::files::AdapterError::PlanInvalid),
                         "files_planning.source_handoff"}};
            source_done = true;
            condition.notify_all();
        }

        void accept_destination(ck::files::Observation<FilesPaneOperationDestination> value) {
            std::lock_guard lock(mutex);
            if (failure || destination_done)
                return;
            if (value.value())
                destination_result = std::move(value).take_value();
            else
                failure = value.unavailable_detail()
                    ? *value.unavailable_detail()
                    : ck::files::UnavailableObservation{ck::files::ObservationState::Failed,
                        {ck::files::ErrorDomain::Adapter,
                         static_cast<std::int64_t>(ck::files::AdapterError::PlanInvalid),
                         "files_planning.destination_handoff"}};
            destination_done = true;
            condition.notify_all();
        }

        bool wait(std::stop_token stop) {
            std::unique_lock lock(mutex);
            while (!failure && !(source_done && destination_done) && !stop.stop_requested())
                condition.wait_for(lock, std::chrono::milliseconds(10));
            return !stop.stop_requested() && !failure && source_done && destination_done;
        }
    };
    struct PaneInput final {
        FilesOperationPanePlanningRequest request;
        std::shared_ptr<PaneHandoff> handoff;
        FilesPaneTicket source_request, destination_request;
    };
    using Input = std::variant<FilesOperationPlanningRequest, FilesOperationNativePlanningRequest, PaneInput>;
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Input> pending;
    std::optional<FilesOperationPlanningReply> reply;
    FilesOperationPlanningTicket latest, cancelled;
    std::optional<FilesPaneTicket> pane_source_request, pane_destination_request;
    FilesPaneService *pane_source_service = nullptr;
    FilesPaneService *pane_destination_service = nullptr;
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
            pane_source_request.reset();
            pane_destination_request.reset();
            pane_source_service = nullptr;
            pane_destination_service = nullptr;
            if (stopping || result.request != latest)
                return;
            if (result.request == cancelled && result.state == FilesOperationPlanningReplyState::Ready) {
                result.state = FilesOperationPlanningReplyState::Cancelled;
                result.plan.reset();
                result.review.reset();
                result.evidence.reset();
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
            pane_source_request.reset();
            pane_destination_request.reset();
            pane_source_service = nullptr;
            pane_destination_service = nullptr;
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
        const bool valid_request = std::visit([](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, PaneInput>)
                return false;
            else
                return valid(value);
        }, request);
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

    FilesOperationPlanningSubmitted submit_pane(FilesOperationPanePlanningRequest request) {
        if (!valid(request))
            return {FilesOperationPlanningSubmission::Invalid, {}};
        auto handoff = std::make_shared<PaneHandoff>();
        FilesPaneService *source_service = request.source_service;
        FilesPaneService *destination_service = request.destination_service;
        const auto source_navigation = request.source_navigation;
        const auto destination_navigation = request.destination_navigation;
        const auto source_stamp = request.source_stamp;
        const auto destination_stamp = request.destination_stamp;
        const auto source_limits = request.source_limits;
        FilesOperationPlanningTicket request_ticket;
        {
            std::lock_guard lock(mutex);
            if (stopping || !alive.load(std::memory_order_acquire))
                return {FilesOperationPlanningSubmission::Stopped, {}};
            if (pending || busy || reply)
                return {FilesOperationPlanningSubmission::Busy, {}};
            if (!next)
                return {FilesOperationPlanningSubmission::Exhausted, {}};
            request_ticket = latest = FilesOperationPlanningService::ticket(next++);
            cancelled = {};
            signalled = false;
            pending.emplace(PaneInput{std::move(request), handoff, {}, {}});
        }

        const auto source = source_service->acquire_operation_sources(
            source_navigation, source_stamp, source_limits,
            [handoff](ck::files::Observation<FilesPaneOperationSources> value) {
                handoff->source(std::move(value));
            });
        const auto destination = destination_service->acquire_operation_destination(
            destination_navigation, destination_stamp,
            [handoff](ck::files::Observation<FilesPaneOperationDestination> value) {
                handoff->accept_destination(std::move(value));
            });
        {
            std::lock_guard lock(mutex);
            if (source.state == FilesPaneSubmission::Accepted)
                pane_source_request = source.ticket;
            if (destination.state == FilesPaneSubmission::Accepted)
                pane_destination_request = destination.ticket;
            pane_source_service = source_service;
            pane_destination_service = destination_service;
        }
        if (source.state != FilesPaneSubmission::Accepted)
            handoff->source(ck::files::Observation<FilesPaneOperationSources>::unavailable(
                ck::files::ObservationState::Unknown,
                {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(ck::files::AdapterError::InvalidAccess),
                 "files_planning.source_submission"}));
        if (destination.state != FilesPaneSubmission::Accepted)
            handoff->accept_destination(ck::files::Observation<FilesPaneOperationDestination>::unavailable(
                ck::files::ObservationState::Unknown,
                {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(ck::files::AdapterError::InvalidAccess),
                 "files_planning.destination_submission"}));
        if (source.state != FilesPaneSubmission::Accepted && destination.state == FilesPaneSubmission::Accepted)
            destination_service->cancel(destination.ticket);
        if (destination.state != FilesPaneSubmission::Accepted && source.state == FilesPaneSubmission::Accepted)
            source_service->cancel(source.ticket);
        condition.notify_one();
        return {FilesOperationPlanningSubmission::Accepted, request_ticket};
    }

    void compose_native(FilesOperationNativePlanningRequest native, std::stop_token stop,
                        FilesOperationPlanningReply &result,
                        ck::files::Observation<ck::files::OperationPlan::Pointer> &plan) {
        try {
            auto collected = collect_files_operation_evidence(std::move(native), stop);
            if (stop.stop_requested()) {
                result.state = FilesOperationPlanningReplyState::Cancelled;
            } else if (const auto* failure = collected.unavailable_detail()) {
                result.error = *failure;
                result.state = limit_error(*failure) ? FilesOperationPlanningReplyState::LimitExceeded
                                                     : FilesOperationPlanningReplyState::Failed;
            } else {
                result.evidence = std::move(collected).take_value();
                plan = ck::files::Observation<ck::files::OperationPlan::Pointer>::available(result.evidence->plan);
            }
        } catch (const std::invalid_argument &) {
            result.error = ck::files::UnavailableObservation{ck::files::ObservationState::Failed,
                {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(ck::files::AdapterError::PlanInvalid),
                 "files_planning.native_request"}};
            result.state = FilesOperationPlanningReplyState::Failed;
        } catch (const std::length_error &) {
            result.error = ck::files::UnavailableObservation{ck::files::ObservationState::Unknown,
                {ck::files::ErrorDomain::Adapter,
                 static_cast<std::int64_t>(ck::files::AdapterError::InventoryLimit),
                 "files_planning.native_request_limit"}};
            result.state = FilesOperationPlanningReplyState::LimitExceeded;
        }
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
                } else if (auto *pane_input = std::get_if<PaneInput>(&*request)) {
                    review_limits = pane_input->request.review_limits;
                    if (!pane_input->handoff->wait(stop.get_token())) {
                        std::lock_guard lock(pane_input->handoff->mutex);
                        if (pane_input->handoff->failure) {
                            result.error = *pane_input->handoff->failure;
                            result.state = limit_error(*result.error)
                                ? FilesOperationPlanningReplyState::LimitExceeded
                                : FilesOperationPlanningReplyState::Failed;
                        } else {
                            result.state = FilesOperationPlanningReplyState::Cancelled;
                        }
                    } else {
                        FilesPaneOperationSources sources;
                        FilesPaneOperationDestination destination;
                        {
                            std::lock_guard lock(pane_input->handoff->mutex);
                            sources = std::move(*pane_input->handoff->sources);
                            destination = std::move(*pane_input->handoff->destination_result);
                        }
                        auto evidence = files_operation_evidence_request(
                            pane_input->request.operation, std::move(sources), destination);
                        if (const auto* failure = evidence.unavailable_detail()) {
                            result.error = *failure;
                            result.state = FilesOperationPlanningReplyState::Failed;
                        } else {
                            FilesOperationNativePlanningRequest native;
                            native.evidence = std::move(evidence).take_value();
                            native.evidence.mount_policy = pane_input->request.mount_policy;
                            native.evidence.link_policy = pane_input->request.link_policy;
                            native.evidence.limits = pane_input->request.evidence_limits;
                            native.projection_policy = pane_input->request.projection_policy;
                            native.limits = pane_input->request.limits;
                            compose_native(std::move(native), stop.get_token(), result, plan);
                        }
                    }
                } else {
                    auto native = std::move(std::get<FilesOperationNativePlanningRequest>(*request));
                    review_limits = native.review_limits;
                    compose_native(std::move(native), stop.get_token(), result, plan);
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
            if (result.state != FilesOperationPlanningReplyState::Ready)
                result.evidence.reset();
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

FilesOperationPlanningSubmitted FilesOperationPlanningService::request(FilesOperationPanePlanningRequest request) {
    return state_->submit_pane(std::move(request));
}

bool FilesOperationPlanningService::cancel(const FilesOperationPlanningTicket& request) {
    std::optional<std::stop_source> stop;
    FilesPaneService *source_service = nullptr, *destination_service = nullptr;
    std::optional<FilesPaneTicket> source_request, destination_request;
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
        source_service = state_->pane_source_service;
        destination_service = state_->pane_destination_service;
        source_request = state_->pane_source_request;
        destination_request = state_->pane_destination_request;
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
    if (source_service && source_request)
        source_service->cancel(*source_request);
    if (destination_service && destination_request)
        destination_service->cancel(*destination_request);
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
    FilesPaneService *source_service = nullptr, *destination_service = nullptr;
    std::optional<FilesPaneTicket> source_request, destination_request;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->pending.reset();
        state_->reply.reset();
        source_service = state_->pane_source_service;
        destination_service = state_->pane_destination_service;
        source_request = state_->pane_source_request;
        destination_request = state_->pane_destination_request;
        if (state_->active_stop)
            state_->active_stop->request_stop();
    }
    if (source_service && source_request)
        source_service->cancel(*source_request);
    if (destination_service && destination_request)
        destination_service->cancel(*destination_request);
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
