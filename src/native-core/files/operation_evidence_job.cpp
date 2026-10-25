// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_evidence_job.hpp"

#include "preservation_capability_diff.hpp"

#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace ck::files {
namespace {

UnavailableObservation error(AdapterError code, const char* operation,
    ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

template <class T>
Observation<T> unavailable(const UnavailableObservation& detail) {
    return Observation<T>::unavailable(detail.state, detail.error);
}

} // namespace

OperationEvidenceJob::OperationEvidenceJob(OperationEvidenceJobRequest request)
    : request_(std::move(request)) {
    if ((request_.operation != FileOperation::Copy && request_.operation != FileOperation::Move) ||
        !request_.root_source || !request_.source_root || !request_.source_path || !request_.source_path->is_valid() ||
        !request_.destination || !request_.clock || request_.limits.max_items == 0 ||
        request_.limits.max_items > OperationEvidenceJobLimits::hard_max_items ||
        request_.destination_names.size() > request_.limits.max_items) {
        throw std::invalid_argument("invalid operation evidence job");
    }
    recursive_ = std::make_unique<RecursiveInventoryJob>(request_.source_root, *request_.source_path,
        request_.mount_policy, request_.link_policy, request_.limits.recursive, request_.clock,
        request_.root_source);
}

void OperationEvidenceJob::release_staging() noexcept {
    recursive_.reset();
    recursive_result_.reset();
    object_.reset();
    source_objects_.clear();
    destination_inventory_.reset();
    destination_inventory_result_.reset();
    destination_assessment_.reset();
    destination_batch_.reset();
    destination_batch_result_.reset();
    source_target_.reset();
    source_target_result_.reset();
    ancestry_.reset();
    ancestry_results_.clear();
    destination_capabilities_.clear();
    next_object_ = next_ancestry_ = 0;
}

void OperationEvidenceJob::fail(UnavailableObservation failure) noexcept {
    failure_ = std::move(failure);
    completed_.reset();
    release_staging();
    state_ = OperationEvidenceJobState::Failed;
}

void OperationEvidenceJob::advance_recursive(std::stop_token stop) {
    const auto result = recursive_->advance(stop);
    if (result == RecursiveInventoryState::Paused) {
        state_ = OperationEvidenceJobState::Paused;
        return;
    }
    if (result == RecursiveInventoryState::Failed) {
        fail(*recursive_->failure());
        return;
    }
    if (result != RecursiveInventoryState::Complete)
        return;

    auto completed = recursive_->completed_sources();
    if (!completed.value()) {
        fail(completed.unavailable_detail() ? *completed.unavailable_detail()
                                            : error(AdapterError::PlanInvalid, "operation_evidence.recursive_result"));
        return;
    }
    recursive_result_ = std::move(completed).take_value();
    if (!recursive_result_->inventory() ||
        recursive_result_->sources().size() != recursive_result_->inventory()->entries().size() ||
        request_.destination_names.size() != recursive_result_->inventory()->entries().size()) {
        fail(error(AdapterError::PlanInvalid, "operation_evidence.inventory_shape"));
        return;
    }
    if (recursive_result_->inventory()->entries().size() > request_.limits.max_items) {
        fail(error(AdapterError::InventoryLimit, "operation_evidence.item_limit", ObservationState::Unknown));
        return;
    }
    if (request_.limits.object.time_slice <= MonotonicClock::Duration::zero() ||
        request_.limits.object.time_slice > std::chrono::seconds(1) ||
        request_.limits.source_target.rows < recursive_result_->sources().size() ||
        request_.limits.destination_batch.rows < recursive_result_->sources().size()) {
        fail(error(AdapterError::InventoryLimit, "operation_evidence.stage_rows", ObservationState::Unknown));
        return;
    }
    source_objects_.reserve(recursive_result_->sources().size());
    phase_ = Phase::Object;
}

void OperationEvidenceJob::advance_object(std::stop_token stop) {
    if (next_object_ == recursive_result_->sources().size()) {
        destination_inventory_ = std::make_unique<DirectoryInventoryJob>(request_.destination,
            request_.limits.destination_inventory, request_.clock);
        phase_ = Phase::DestinationInventory;
        return;
    }
    if (!object_)
        object_ = ObjectInventoryJob::for_held_source(recursive_result_->sources()[next_object_],
            request_.limits.object, request_.clock);
    const auto result = object_->advance(stop);
    if (result == ObjectInventoryJobState::Paused) {
        state_ = OperationEvidenceJobState::Paused;
        return;
    }
    if (result == ObjectInventoryJobState::Failed) {
        fail(*object_->failure());
        return;
    }
    if (result != ObjectInventoryJobState::Complete)
        return;
    auto completed = object_->completed();
    if (!completed.value()) {
        fail(completed.unavailable_detail() ? *completed.unavailable_detail()
                                            : error(AdapterError::PlanInvalid, "operation_evidence.object_result"));
        return;
    }
    source_objects_.push_back((*completed.value())->data());
    object_.reset();
    ++next_object_;
}

void OperationEvidenceJob::advance_destination_inventory(std::stop_token stop) {
    const auto result = destination_inventory_->advance(stop);
    if (result == DirectoryInventoryState::Paused) {
        state_ = OperationEvidenceJobState::Paused;
        return;
    }
    if (result == DirectoryInventoryState::Failed) {
        fail(*destination_inventory_->failure());
        return;
    }
    if (result != DirectoryInventoryState::Complete)
        return;
    auto completed = destination_inventory_->completed();
    if (!completed.value()) {
        fail(completed.unavailable_detail() ? *completed.unavailable_detail()
                                            : error(AdapterError::PlanInvalid, "operation_evidence.destination_result"));
        return;
    }
    destination_inventory_result_ = std::move(completed).take_value();
    phase_ = Phase::DestinationAssessment;
}

void OperationEvidenceJob::advance_destination_batch(std::stop_token stop) {
    if (!destination_batch_)
        destination_batch_ = std::make_unique<DestinationBatchJob>(request_.destination,
            std::move(request_.destination_names), request_.destination_encoding,
            request_.limits.destination_batch, request_.clock);
    const auto result = destination_batch_->advance(stop);
    if (result == DestinationBatchState::Paused) {
        state_ = OperationEvidenceJobState::Paused;
        return;
    }
    if (result == DestinationBatchState::Failed) {
        fail(*destination_batch_->failure());
        return;
    }
    if (result != DestinationBatchState::Complete)
        return;
    auto completed = destination_batch_->completed();
    if (!completed.value()) {
        fail(completed.unavailable_detail() ? *completed.unavailable_detail()
                                            : error(AdapterError::PlanInvalid, "operation_evidence.batch_result"));
        return;
    }
    destination_batch_result_ = std::move(completed).take_value();
    source_target_ = std::make_unique<SourceTargetJob>(
        std::vector<HeldChild::Pointer>(recursive_result_->sources().begin(), recursive_result_->sources().end()),
        destination_batch_result_, request_.limits.source_target, request_.clock);
    phase_ = Phase::SourceTarget;
}

void OperationEvidenceJob::advance_source_target(std::stop_token stop) {
    const auto result = source_target_->advance(stop);
    if (result == SourceTargetState::Paused) {
        state_ = OperationEvidenceJobState::Paused;
        return;
    }
    if (result == SourceTargetState::Failed) {
        fail(*source_target_->failure());
        return;
    }
    if (result != SourceTargetState::Complete)
        return;
    auto completed = source_target_->completed();
    if (!completed.value()) {
        fail(completed.unavailable_detail() ? *completed.unavailable_detail()
                                            : error(AdapterError::PlanInvalid, "operation_evidence.source_target_result"));
        return;
    }
    source_target_result_ = std::move(completed).take_value();
    ancestry_results_.reserve(recursive_result_->sources().size());
    phase_ = Phase::Ancestry;
}

void OperationEvidenceJob::advance_ancestry(std::stop_token stop) {
    if (next_ancestry_ == recursive_result_->sources().size()) {
        phase_ = Phase::Seal;
        return;
    }
    if (!ancestry_)
        ancestry_ = std::make_unique<AncestorWalkJob>(recursive_result_->sources()[next_ancestry_],
            source_target_result_->destination()->pin_directory(), request_.limits.ancestry, request_.clock);
    const auto result = ancestry_->advance(stop);
    if (result == AncestorWalkState::Paused) {
        state_ = OperationEvidenceJobState::Paused;
        return;
    }
    if (result == AncestorWalkState::Failed) {
        fail(*ancestry_->failure());
        return;
    }
    if (result != AncestorWalkState::Complete)
        return;
    auto completed = ancestry_->completed();
    if (!completed.value()) {
        fail(completed.unavailable_detail() ? *completed.unavailable_detail()
                                            : error(AdapterError::PlanInvalid, "operation_evidence.ancestry_result"));
        return;
    }
    ancestry_results_.push_back((*completed.value())->outcome());
    ancestry_.reset();
    ++next_ancestry_;
}

OperationEvidenceJobState OperationEvidenceJob::advance(std::stop_token stop) {
    if (state_ == OperationEvidenceJobState::Complete || state_ == OperationEvidenceJobState::Failed ||
        state_ == OperationEvidenceJobState::Abandoned)
        return state_;
    if (stop.stop_requested()) {
        state_ = OperationEvidenceJobState::Paused;
        return state_;
    }
    try {
        state_ = OperationEvidenceJobState::Collecting;
        switch (phase_) {
        case Phase::Recursive:
            advance_recursive(stop);
            break;
        case Phase::Object:
            advance_object(stop);
            break;
        case Phase::DestinationInventory:
            advance_destination_inventory(stop);
            break;
        case Phase::DestinationAssessment: {
            auto assessment = DestinationDirectoryAssessment::inspect(std::move(destination_inventory_result_),
                request_.limits.destination_assessment);
            if (!assessment.value()) {
                fail(*assessment.unavailable_detail());
                break;
            }
            destination_assessment_ = std::move(assessment).take_value();
            destination_capabilities_ = PreservationCapabilityDiff::from_destination_assessment(*destination_assessment_);
            phase_ = Phase::DestinationBatch;
            break;
        }
        case Phase::DestinationBatch:
            advance_destination_batch(stop);
            break;
        case Phase::SourceTarget:
            advance_source_target(stop);
            break;
        case Phase::Ancestry:
            advance_ancestry(stop);
            break;
        case Phase::Seal:
            if (!recursive_result_ || !source_target_result_ || source_objects_.size() != recursive_result_->sources().size() ||
                ancestry_results_.size() != recursive_result_->sources().size()) {
                fail(error(AdapterError::PlanInvalid, "operation_evidence.seal"));
                break;
            }
            completed_ = Result(new const CompletedOperationEvidence(request_.operation,
                recursive_result_->inventory(), std::move(source_objects_), source_target_result_->destination(),
                source_target_result_->entries(), std::move(ancestry_results_), std::move(destination_capabilities_)));
            release_staging();
            state_ = OperationEvidenceJobState::Complete;
            break;
        }
        return state_;
    } catch (...) {
        const char* operation = "operation_evidence.exception";
        switch (phase_) {
        case Phase::Recursive: operation = "operation_evidence.recursive_exception"; break;
        case Phase::Object: operation = "operation_evidence.object_exception"; break;
        case Phase::DestinationInventory: operation = "operation_evidence.destination_inventory_exception"; break;
        case Phase::DestinationAssessment: operation = "operation_evidence.destination_assessment_exception"; break;
        case Phase::DestinationBatch: operation = "operation_evidence.destination_batch_exception"; break;
        case Phase::SourceTarget: operation = "operation_evidence.source_target_exception"; break;
        case Phase::Ancestry: operation = "operation_evidence.ancestry_exception"; break;
        case Phase::Seal: operation = "operation_evidence.seal_exception"; break;
        }
        fail(error(AdapterError::InterruptedRead, operation));
        throw;
    }
}

bool OperationEvidenceJob::abandon() noexcept {
    if (state_ == OperationEvidenceJobState::Complete || state_ == OperationEvidenceJobState::Failed ||
        state_ == OperationEvidenceJobState::Abandoned)
        return false;
    release_staging();
    state_ = OperationEvidenceJobState::Abandoned;
    return true;
}

Observation<OperationEvidenceJob::Result> OperationEvidenceJob::completed() const {
    if (completed_) return Observation<Result>::available(completed_);
    if (failure_) return unavailable<Result>(*failure_);
    return {};
}

} // namespace ck::files
