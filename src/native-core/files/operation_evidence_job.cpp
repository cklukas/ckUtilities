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

bool same_components(const std::vector<NativeName>& left, const std::vector<NativeName>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (left[index] != right[index]) return false;
    return true;
}

} // namespace

OperationEvidenceJob::OperationEvidenceJob(OperationEvidenceJobRequest request)
    : request_(std::move(request)) {
    const bool recursive = request_.root_source && request_.source_root && request_.source_path &&
        request_.source_path->is_valid() && request_.direct_sources.empty() && request_.direct_source_paths.empty();
    const bool flat = !request_.root_source && !request_.source_root && !request_.source_path &&
        !request_.direct_sources.empty() && request_.direct_sources.size() == request_.direct_source_paths.size();
    if ((request_.operation != FileOperation::Copy && request_.operation != FileOperation::Move) ||
        (!recursive && !flat) ||
        !request_.destination || !request_.clock || request_.limits.max_items == 0 ||
        request_.limits.max_items > OperationEvidenceJobLimits::hard_max_items ||
        request_.destination_names.size() > request_.limits.max_items) {
        throw std::invalid_argument("invalid operation evidence job");
    }
    if (recursive) {
        recursive_ = std::make_unique<RecursiveInventoryJob>(request_.source_root, *request_.source_path,
            request_.mount_policy, request_.link_policy, request_.limits.recursive, request_.clock,
            request_.root_source);
    } else {
        flat_builder_ = std::make_unique<FrozenInventoryBuilder>(InventoryMountPolicy::DirectObjects, request_.link_policy,
            request_.limits.recursive.inventory);
        phase_ = Phase::FlatInventory;
    }
}

void OperationEvidenceJob::release_staging() noexcept {
    recursive_.reset();
    recursive_result_.reset();
    flat_builder_.reset();
    object_.reset();
    source_objects_.clear();
    destination_inventory_.reset();
    destination_inventory_result_.reset();
    destination_assessment_.reset();
    destination_parent_directories_.clear();
    destination_directory_children_.clear();
    destination_leaf_names_.clear();
    destination_batch_.reset();
    destination_batch_result_.reset();
    source_target_.reset();
    source_target_result_.reset();
    ancestry_.reset();
    ancestry_results_.clear();
    destination_capabilities_.clear();
    next_object_ = next_flat_ = next_destination_parent_ = next_ancestry_ = 0;
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
        request_.destination_names.size() != 1) {
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
    destination_leaf_names_.reserve(recursive_result_->inventory()->entries().size());
    for (std::size_t item = 0; item < recursive_result_->inventory()->entries().size(); ++item) {
        const auto& relative = recursive_result_->inventory()->entries()[item].relative_components;
        if (item == 0) {
            if (!relative.empty()) {
                fail(error(AdapterError::PlanInvalid, "operation_evidence.root_components"));
                return;
            }
            destination_leaf_names_.push_back(request_.destination_names.front());
        } else if (relative.empty() || !relative.back().is_valid()) {
            fail(error(AdapterError::PlanInvalid, "operation_evidence.destination_components"));
            return;
        } else {
            destination_leaf_names_.push_back(relative.back());
        }
    }
    destination_parent_directories_.assign(destination_leaf_names_.size(), {});
    destination_directory_children_.assign(destination_leaf_names_.size(), {});
    source_objects_.reserve(recursive_result_->sources().size());
    phase_ = Phase::Object;
}

void OperationEvidenceJob::advance_flat_inventory() {
    if (next_flat_ < request_.direct_sources.size()) {
        const auto& source = request_.direct_sources[next_flat_];
        if (!source || !request_.direct_source_paths[next_flat_].is_valid()) {
            fail(error(AdapterError::InvalidIdentifier, "operation_evidence.direct_source"));
            return;
        }
        const auto& metadata = source->metadata();
        if (!metadata.identity.value() || !metadata.kind.value() || !metadata.logical_size.value()) {
            fail(error(AdapterError::UnqualifiedIdentity, "operation_evidence.direct_metadata", ObservationState::Unknown));
            return;
        }
        if (!flat_builder_->add({request_.direct_source_paths[next_flat_], *metadata.identity.value(),
                *metadata.kind.value(), metadata.mount_id, *metadata.logical_size.value(),
                LinkTraversalEvidence::NotLink, {}, {}})) {
            if (flat_builder_->failure()) fail(*flat_builder_->failure());
            else fail(error(AdapterError::PlanInvalid, "operation_evidence.direct_inventory"));
            return;
        }
        ++next_flat_;
        return;
    }
    if (!flat_builder_->seal()) {
        if (flat_builder_->failure()) fail(*flat_builder_->failure());
        else fail(error(AdapterError::PlanInvalid, "operation_evidence.direct_seal"));
        return;
    }
    auto inventory = flat_builder_->completed();
    if (!inventory.value()) {
        fail(inventory.unavailable_detail() ? *inventory.unavailable_detail()
                                            : error(AdapterError::PlanInvalid, "operation_evidence.direct_result"));
        return;
    }
    if (request_.destination_names.size() != request_.direct_sources.size()) {
        fail(error(AdapterError::PlanInvalid, "operation_evidence.inventory_shape"));
        return;
    }
    if (request_.direct_sources.size() > request_.limits.max_items ||
        request_.limits.object.time_slice <= MonotonicClock::Duration::zero() ||
        request_.limits.object.time_slice > std::chrono::seconds(1) ||
        request_.limits.source_target.rows < request_.direct_sources.size() ||
        request_.limits.destination_batch.rows < request_.direct_sources.size()) {
        fail(error(AdapterError::InventoryLimit, "operation_evidence.stage_rows", ObservationState::Unknown));
        return;
    }
    recursive_result_ = std::shared_ptr<const CompletedRecursiveInventory>(
        new const CompletedRecursiveInventory(std::move(inventory).take_value(), std::move(request_.direct_sources)));
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

void OperationEvidenceJob::advance_destination_parent_mapping(std::stop_token stop) {
    if (next_destination_parent_ == destination_leaf_names_.size()) {
        destination_batch_ = std::make_unique<DestinationBatchJob>(std::move(destination_parent_directories_),
            std::move(destination_leaf_names_), request_.destination_encoding,
            request_.limits.destination_batch, request_.clock);
        phase_ = Phase::DestinationBatch;
        return;
    }
    if (stop.stop_requested()) {
        state_ = OperationEvidenceJobState::Paused;
        return;
    }

    const auto& entries = recursive_result_->inventory()->entries();
    const auto item = next_destination_parent_;
    const auto& relative = entries[item].relative_components;
    std::shared_ptr<const DirectoryCursor> parent;
    if (item == 0) {
        parent = request_.destination;
    } else {
        if (relative.empty()) {
            fail(error(AdapterError::PlanInvalid, "operation_evidence.destination_parent"));
            return;
        }
        std::vector<NativeName> parent_components(relative.begin(), relative.end() - 1);
        std::optional<std::size_t> parent_item;
        for (std::size_t candidate = 0; candidate < item; ++candidate) {
            if (entries[candidate].kind == ObjectKind::Directory &&
                same_components(entries[candidate].relative_components, parent_components)) {
                parent_item = candidate;
                break;
            }
        }
        if (parent_item) parent = destination_directory_children_[*parent_item];
    }
    destination_parent_directories_[item] = parent;

    if (parent && entries[item].kind == ObjectKind::Directory) {
        auto mapped = map_native_name(destination_leaf_names_[item], request_.destination_encoding,
            {request_.limits.destination_batch.component_bytes});
        if (mapped.value()) {
            auto probe = DirectoryEntryProbe::inspect(parent, *mapped.value(), ChildLookupMode::NativeDefault,
                {request_.limits.destination_batch.component_bytes}, stop);
            if (!probe.value() && stop.stop_requested()) {
                state_ = OperationEvidenceJobState::Paused;
                return;
            }
            const auto* inspected = probe.value() ? probe.value()->get() : nullptr;
            if (inspected && inspected->lookup().value() &&
                inspected->lookup().value()->state() == ChildLookupState::Found &&
                inspected->lookup().value()->entry() && inspected->existing_metadata().value() &&
                inspected->existing_metadata().value()->kind.value() &&
                *inspected->existing_metadata().value()->kind.value() == ObjectKind::Directory) {
                auto child = parent->open_directory_child(*mapped.value());
                if (child.value() && (*child.value())->identity().value() &&
                    inspected->existing_metadata().value()->identity.value() &&
                    compare_identity(*(*child.value())->identity().value(),
                        *inspected->existing_metadata().value()->identity.value()) == IdentityComparison::Same) {
                    destination_directory_children_[item] = std::shared_ptr<const DirectoryCursor>(std::move(child).take_value());
                }
            }
        }
    }
    ++next_destination_parent_;
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
    if (!ancestry_) {
        const auto& entries = recursive_result_->inventory()->entries();
        std::size_t source_index = next_ancestry_;
        if (request_.root_source && entries[next_ancestry_].kind != ObjectKind::Directory) {
            const auto& relative = entries[next_ancestry_].relative_components;
            if (relative.empty()) {
                fail(error(AdapterError::PlanInvalid, "operation_evidence.file_parent"));
                return;
            }
            std::vector<NativeName> parent_components(relative.begin(), relative.end() - 1);
            std::optional<std::size_t> parent_index;
            for (std::size_t candidate = 0; candidate < next_ancestry_; ++candidate) {
                if (entries[candidate].kind == ObjectKind::Directory &&
                    same_components(entries[candidate].relative_components, parent_components)) {
                    parent_index = candidate;
                    break;
                }
            }
            if (!parent_index) {
                fail(error(AdapterError::PlanInvalid, "operation_evidence.file_parent"));
                return;
            }
            source_index = *parent_index;
        }
        ancestry_ = std::make_unique<AncestorWalkJob>(recursive_result_->sources()[source_index],
            source_target_result_->destination()->pin_directory(), request_.limits.ancestry, request_.clock);
    }
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
        case Phase::FlatInventory:
            advance_flat_inventory();
            break;
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
            phase_ = request_.root_source ? Phase::DestinationParentMapping : Phase::DestinationBatch;
            break;
        }
        case Phase::DestinationParentMapping:
            advance_destination_parent_mapping(stop);
            break;
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
        case Phase::FlatInventory: operation = "operation_evidence.flat_exception"; break;
        case Phase::Recursive: operation = "operation_evidence.recursive_exception"; break;
        case Phase::Object: operation = "operation_evidence.object_exception"; break;
        case Phase::DestinationInventory: operation = "operation_evidence.destination_inventory_exception"; break;
        case Phase::DestinationAssessment: operation = "operation_evidence.destination_assessment_exception"; break;
        case Phase::DestinationParentMapping: operation = "operation_evidence.destination_parent_exception"; break;
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
