// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_pane_service.hpp"
#include "operation_evidence_job.hpp"
#include "operation_planning_coordinator.hpp"
#include "operation_review.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ck::vision {

// All native work that produced this request has already completed on a
// worker. The planning service only joins immutable values and never reopens
// paths, probes targets, performs native I/O or mutates the filesystem.
struct FilesOperationPlanningRequest final {
    ck::files::FileOperation operation = ck::files::FileOperation::Copy;
    std::shared_ptr<const ck::files::FrozenInventory> inventory;
    std::vector<ck::files::SourceTargetInspection> source_target;
    std::vector<ck::files::Observation<ck::files::AncestorWalkWitness>> ancestry;
    std::vector<ck::files::ObjectInventoryData> source_objects;
    std::shared_ptr<const ck::files::CompletedDestinationBatch> destination_batch;
    std::vector<ck::files::DestinationCapabilityEvidence> destination_capabilities;
    ck::files::SourceCapabilityProjectionPolicy projection_policy;
    ck::files::OperationPlanningCoordinatorLimits limits;
    ck::files::OperationReviewLimits review_limits;
};

// Native preparation is submitted only by a worker-owned composition root.
// The planning worker performs the read-only native evidence stages, then
// consumes their completed value bundle before any review model is published.
// No native owner crosses into UI state or a consent receipt.
struct FilesOperationNativePlanningRequest final {
    ck::files::OperationEvidenceJobRequest evidence;
    ck::files::SourceCapabilityProjectionPolicy projection_policy;
    ck::files::OperationPlanningCoordinatorLimits limits;
    ck::files::OperationReviewLimits review_limits;
};

// Composition-root request for the real pane operation path. The two pane
// workers acquire their own held source/destination objects and deliver them
// directly to the planning worker. This value contains no native owner.
struct FilesOperationPanePlanningRequest final {
    ck::files::FileOperation operation = ck::files::FileOperation::Copy;
    FilesPaneService *source_service = nullptr;
    FilesPaneService *destination_service = nullptr;
    FilesPaneTicket source_navigation, destination_navigation;
    ck::files::PaneStateStamp source_stamp, destination_stamp;
    ck::files::IndexedSourceLimits source_limits;
    ck::files::InventoryMountPolicy mount_policy = ck::files::InventoryMountPolicy::StayOnRootMount;
    ck::files::InventoryLinkPolicy link_policy = ck::files::InventoryLinkPolicy::PreserveLinks;
    ck::files::OperationEvidenceJobLimits evidence_limits;
    ck::files::SourceCapabilityProjectionPolicy projection_policy;
    ck::files::OperationPlanningCoordinatorLimits limits;
    ck::files::OperationReviewLimits review_limits;
};

// Value evidence accompanies a native review. No cursor, probe or source
// handle crosses the UI boundary. The execution worker collects it again.
struct FilesOperationReviewEvidence final {
    using Pointer = std::shared_ptr<const FilesOperationReviewEvidence>;
    struct SourceTimes final {
        ck::files::Observation<ck::files::FileTimestamp> modified, changed;
    };

    ck::files::OperationPlan::Pointer plan;
    ck::files::FrozenInventory::Pointer inventory;
    ck::files::ObjectIdentity destination;
    std::vector<ck::files::NativeName> destination_names;
    std::vector<SourceTimes> source_times;
    ck::files::InventoryMountPolicy mount_policy;
    ck::files::InventoryLinkPolicy link_policy;
    ck::files::OperationEvidenceJobLimits evidence_limits;
    ck::files::SourceCapabilityProjectionPolicy projection_policy;
    ck::files::OperationPlanningCoordinatorLimits limits;

    bool same_namespace_and_sources(const FilesOperationReviewEvidence& fresh) const noexcept;
};

ck::files::Observation<ck::files::OperationEvidenceJobRequest> files_operation_evidence_request(
    ck::files::FileOperation operation, FilesPaneOperationSources sources,
    const FilesPaneOperationDestination& destination);

ck::files::Observation<FilesOperationReviewEvidence::Pointer> collect_files_operation_evidence(
    FilesOperationNativePlanningRequest request, std::stop_token stop,
    std::shared_ptr<const ck::files::CompletedRecursiveInventory>* sealed_sources = nullptr);

enum class FilesOperationPlanningSubmission { Accepted, Busy, Stopped, Invalid, Exhausted };
enum class FilesOperationPlanningReplyState { Ready, Failed, Cancelled, LimitExceeded, WorkerFault };

struct FilesOperationPlanningTicket final {
    FilesOperationPlanningTicket() = default;
    std::uint64_t number() const noexcept { return identity_ ? *identity_ : 0; }
    bool valid() const noexcept { return identity_ != nullptr; }
    friend bool operator==(const FilesOperationPlanningTicket&, const FilesOperationPlanningTicket&) = default;

private:
    friend class FilesOperationPlanningService;
    explicit FilesOperationPlanningTicket(std::uint64_t number)
        : identity_(std::make_shared<const std::uint64_t>(number)) {}
    std::shared_ptr<const std::uint64_t> identity_;
};

struct FilesOperationPlanningSubmitted final {
    FilesOperationPlanningSubmission state;
    FilesOperationPlanningTicket ticket;
};

struct FilesOperationPlanningReply final {
    FilesOperationPlanningTicket request;
    FilesOperationPlanningReplyState state = FilesOperationPlanningReplyState::Failed;
    ck::files::OperationPlan::Pointer plan;
    ck::files::OperationReviewModel::Pointer review;
    std::optional<ck::files::UnavailableObservation> error;
    FilesPaneWorkerFault worker_fault = FilesPaneWorkerFault::None;
    FilesOperationReviewEvidence::Pointer evidence;
};

// One serialized, value-only planning worker. The request owns every copied
// value needed for assembly and retains the completed destination batch until
// the worker has finished with it. A reply is delivered only when its exact
// ticket is still current. The wake callback carries no reply and must be
// lifetime-checked by the UI composition root.
class FilesOperationPlanningService final {
public:
    explicit FilesOperationPlanningService(std::function<void()> wake = {});
    ~FilesOperationPlanningService();
    FilesOperationPlanningService(const FilesOperationPlanningService&) = delete;
    FilesOperationPlanningService& operator=(const FilesOperationPlanningService&) = delete;

    FilesOperationPlanningSubmitted request(FilesOperationPlanningRequest request);
    FilesOperationPlanningSubmitted request(FilesOperationNativePlanningRequest request);
    FilesOperationPlanningSubmitted request(FilesOperationPanePlanningRequest request);
    bool cancel(const FilesOperationPlanningTicket& request);
    std::optional<FilesOperationPlanningReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;
    FilesPaneWorkerFault fault() const noexcept;

private:
    struct State;
    static FilesOperationPlanningTicket ticket(std::uint64_t number) {
        return FilesOperationPlanningTicket(number);
    }
    std::unique_ptr<State> state_;
};

} // namespace ck::vision
