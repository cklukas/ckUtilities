// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ancestor_walk_job.hpp"
#include "destination_directory_assessment.hpp"
#include "destination_batch_job.hpp"
#include "directory_inventory_job.hpp"
#include "object_inventory_job.hpp"
#include "preservation_capability_diff.hpp"
#include "recursive_inventory_job.hpp"
#include "source_target_job.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace ck::files {

struct OperationEvidenceJobLimits final {
    RecursiveInventoryLimits recursive{};
    ObjectInventoryLimits object{};
    DirectoryInventoryLimits destination_inventory{};
    DestinationAssessmentLimits destination_assessment{};
    DestinationBatchLimits destination_batch{};
    SourceTargetLimits source_target{};
    AncestorWalkLimits ancestry{};
    std::size_t max_items = 256;
    static constexpr std::size_t hard_max_items = DestinationBatchLimits::hard_rows;
};

// All native owners in this request must already be held by the worker that
// creates it. The job never reconstructs a path from display text and never
// performs a mutation. destination_names are exact native components in the
// same order as the eventual recursive inventory entries; mapping policy is
// supplied explicitly by the caller.
struct OperationEvidenceJobRequest final {
    FileOperation operation = FileOperation::Copy;
    HeldChild::Pointer root_source;
    std::shared_ptr<DirectoryCursor> source_root;
    std::optional<NativeAbsolutePath> source_path;
    std::shared_ptr<const DirectoryCursor> destination;
    std::vector<NativeName> destination_names;
    NameEncoding destination_encoding = NameEncoding::PosixBytes;
    InventoryMountPolicy mount_policy = InventoryMountPolicy::StayOnRootMount;
    InventoryLinkPolicy link_policy = InventoryLinkPolicy::PreserveLinks;
    OperationEvidenceJobLimits limits{};
    std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock();
};

class CompletedOperationEvidence final {
public:
    FileOperation operation() const noexcept { return operation_; }
    const FrozenInventory::Pointer& inventory() const noexcept { return inventory_; }
    const std::vector<ObjectInventoryData>& source_objects() const noexcept { return source_objects_; }
    const DestinationBatchJob::Result& destination_batch() const noexcept { return destination_batch_; }
    const std::vector<SourceTargetInspection>& source_target() const noexcept { return source_target_; }
    const std::vector<Observation<AncestorWalkWitness>>& ancestry() const noexcept { return ancestry_; }
    const std::vector<DestinationCapabilityEvidence>& destination_capabilities() const noexcept {
        return destination_capabilities_;
    }

private:
    friend class OperationEvidenceJob;
    CompletedOperationEvidence(FileOperation operation, FrozenInventory::Pointer inventory,
        std::vector<ObjectInventoryData> source_objects, DestinationBatchJob::Result destination_batch,
        std::vector<SourceTargetInspection> source_target,
        std::vector<Observation<AncestorWalkWitness>> ancestry,
        std::vector<DestinationCapabilityEvidence> destination_capabilities) noexcept
        : operation_(operation), inventory_(std::move(inventory)), source_objects_(std::move(source_objects)),
          destination_batch_(std::move(destination_batch)), source_target_(std::move(source_target)),
          ancestry_(std::move(ancestry)), destination_capabilities_(std::move(destination_capabilities)) {}

    FileOperation operation_;
    FrozenInventory::Pointer inventory_;
    std::vector<ObjectInventoryData> source_objects_;
    DestinationBatchJob::Result destination_batch_;
    std::vector<SourceTargetInspection> source_target_;
    std::vector<Observation<AncestorWalkWitness>> ancestry_;
    std::vector<DestinationCapabilityEvidence> destination_capabilities_;
};

enum class OperationEvidenceJobState { Collecting, Paused, Complete, Failed, Abandoned };

// Serialized worker composition of the native evidence jobs. Each stage
// publishes only after its input job is complete; failures and unavailable
// observations are never converted into absence or a usable plan.
class OperationEvidenceJob final {
public:
    using Result = std::shared_ptr<const CompletedOperationEvidence>;

    explicit OperationEvidenceJob(OperationEvidenceJobRequest request);
    OperationEvidenceJob(const OperationEvidenceJob&) = delete;
    OperationEvidenceJob& operator=(const OperationEvidenceJob&) = delete;

    OperationEvidenceJobState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    OperationEvidenceJobState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;

private:
    enum class Phase { Recursive, Object, DestinationInventory, DestinationAssessment,
        DestinationBatch, SourceTarget, Ancestry, Seal };

    void fail(UnavailableObservation failure) noexcept;
    void release_staging() noexcept;
    void advance_recursive(std::stop_token stop);
    void advance_object(std::stop_token stop);
    void advance_destination_inventory(std::stop_token stop);
    void advance_destination_batch(std::stop_token stop);
    void advance_source_target(std::stop_token stop);
    void advance_ancestry(std::stop_token stop);

    OperationEvidenceJobRequest request_;
    std::unique_ptr<RecursiveInventoryJob> recursive_;
    std::shared_ptr<const CompletedRecursiveInventory> recursive_result_;
    std::unique_ptr<ObjectInventoryJob> object_;
    std::vector<ObjectInventoryData> source_objects_;
    std::unique_ptr<DirectoryInventoryJob> destination_inventory_;
    DirectoryInventoryJob::Result destination_inventory_result_;
    DestinationDirectoryAssessment::Pointer destination_assessment_;
    std::unique_ptr<DestinationBatchJob> destination_batch_;
    DestinationBatchJob::Result destination_batch_result_;
    std::unique_ptr<SourceTargetJob> source_target_;
    SourceTargetJob::Result source_target_result_;
    std::unique_ptr<AncestorWalkJob> ancestry_;
    std::vector<Observation<AncestorWalkWitness>> ancestry_results_;
    std::vector<DestinationCapabilityEvidence> destination_capabilities_;
    std::size_t next_object_ = 0;
    std::size_t next_ancestry_ = 0;
    Phase phase_ = Phase::Recursive;
    OperationEvidenceJobState state_ = OperationEvidenceJobState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};

} // namespace ck::files
