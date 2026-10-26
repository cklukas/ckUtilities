// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_operation_move_job.hpp"
#include "native_operation_copy_job.hpp"
#include "native_operation_recovery_evidence.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace ck::files {
namespace {

template <class T>
Observation<T> rejected(AdapterError code, const char *operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

bool same(const ObjectIdentity &left, const ObjectIdentity &right) noexcept {
    return compare_identity(left, right) == IdentityComparison::Same;
}

std::optional<NativeName> generated_name(NameEncoding encoding, const char *prefix,
                                         const OperationJournalId &id, std::size_t item) {
    std::string text = prefix;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : id.bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        text.push_back(hex[value >> 4U]);
        text.push_back(hex[value & 0x0fU]);
    }
    text.push_back('-');
    text += std::to_string(item);
    if (encoding == NameEncoding::PosixBytes)
        return NativeName::from_posix(std::move(text));
    if (encoding == NameEncoding::WindowsUtf16) {
        std::u16string units;
        units.reserve(text.size());
        for (const auto character : text)
            units.push_back(static_cast<char16_t>(static_cast<unsigned char>(character)));
        return NativeName::from_windows(std::move(units));
    }
    return std::nullopt;
}

bool same_volume(const ObjectIdentity &left, const ObjectIdentity &right) noexcept {
    return left.scope == right.scope && left.scheme == right.scheme && left.volume == right.volume;
}

Observation<OperationMetadataJob::Pointer> move_metadata(OpenedEntry &staging, const HeldChild &source,
                                                          bool allow_unhandled_metadata_loss) {
    OperationMetadataRequest request;
    request.apply_traversal_metadata = [&staging, &source](std::stop_token) {
        return staging.apply_portable_basic_metadata(source.metadata());
    };
    request.apply_security_metadata = [allow_unhandled_metadata_loss](std::stop_token) {
        if (allow_unhandled_metadata_loss)
            return Observation<bool>::available(true);
        return Observation<bool>::unavailable(ObservationState::Unsupported,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::ProviderReviewRequired),
             "native_operation_move.metadata_loss_consent"});
    };
    request.apply_final_times = [](std::stop_token) { return Observation<bool>::available(true); };
    request.flush_destination = [&staging](std::stop_token) { return staging.flush_content(); };
    return OperationMetadataJob::create(std::move(request));
}

OperationJournalId item_id(OperationJournalId value, std::size_t item) noexcept {
    const auto number = static_cast<std::uint64_t>(item);
    for (unsigned byte = 0; byte != sizeof(number); ++byte)
        value.bytes[8 + byte] = std::byte{static_cast<unsigned char>(number >> (byte * 8U))};
    return value;
}

} // namespace

struct NativeOperationMoveJob::State final {
    explicit State(NativeOperationMoveRequest value) : request(std::move(value)) {}

    NativeOperationMoveRequest request;

    Observation<bool> append(OperationJournalState state) {
        const auto sequence = static_cast<std::uint64_t>(request.journal->records().size()) + 1;
        auto result = append_operation_journal_file(*request.journal_file, *request.journal,
                                                    {sequence, request.item, state, {}});
        if (!result.value()) {
            auto detail = *result.unavailable_detail();
            detail.error.operation = "native_operation_move.journal_append_" +
                std::to_string(static_cast<int>(state));
            return Observation<bool>::unavailable(detail.state, std::move(detail.error));
        }
        if (request.journal->state(request.item) != state)
            return rejected<bool>(AdapterError::InvalidNativeResult, "native_operation_move.journal_state",
                                  ObservationState::Unknown);
        return Observation<bool>::available(true);
    }

    Observation<bool> revalidate() const {
        if (!request.source || !request.destination)
            return rejected<bool>(AdapterError::PlanInvalid, "native_operation_move.revalidate_request");
        const auto destination = request.destination->identity();
        if (!destination.value())
            return Observation<bool>::unavailable(destination.unavailable_detail()->state,
                                                   destination.unavailable_detail()->error);
        if (!same(*destination.value(), request.destination_identity))
            return Observation<bool>::available(false);
        const auto parent = request.source->parent()->identity();
        if (!parent.value())
            return Observation<bool>::unavailable(parent.unavailable_detail()->state,
                                                   parent.unavailable_detail()->error);
        const auto source_parent = request.source->parent_identity();
        if (!source_parent.value() || !same(*parent.value(), *source_parent.value()))
            return Observation<bool>::available(false);
        auto opened = request.source->parent()->open_child(request.source->name(), EntryAccess::MetadataOnly);
        if (!opened.value())
            return Observation<bool>::unavailable(opened.unavailable_detail()->state,
                                                   opened.unavailable_detail()->error);
        const auto metadata = (*opened.value())->inspect_basic();
        if (!metadata.value() || !metadata.value()->identity.value() ||
            !request.source->metadata().identity.value())
            return metadata.value()
                ? rejected<bool>(AdapterError::UnqualifiedIdentity, "native_operation_move.source_identity",
                                 ObservationState::Unknown)
                : Observation<bool>::unavailable(metadata.unavailable_detail()->state,
                                                 metadata.unavailable_detail()->error);
        return Observation<bool>::available(same(*metadata.value()->identity.value(),
                                                   *request.source->metadata().identity.value()));
    }
};

Observation<NativeOperationMoveJob::Pointer> NativeOperationMoveJob::create(
    NativeOperationMoveRequest request) {
    if (!request.journal || !request.journal_file || request.item >= request.journal->limits().max_items ||
        request.journal->operation() != FileOperation::Move ||
        request.journal->state(request.item) != OperationJournalState::Planned || !request.source ||
        !request.destination || !request.destination_name.is_valid() ||
        !request.destination_identity.scope.is_valid() ||
        !NativeDurableIdentity::from_live(request.destination_identity) ||
        !request.source->metadata().identity.value() || !request.source->parent_identity().value() ||
        !NativeDurableIdentity::from_live(*request.source->metadata().identity.value()) ||
        !NativeDurableIdentity::from_live(*request.source->parent_identity().value()))
        return rejected<Pointer>(AdapterError::PlanInvalid, "native_operation_move.request");
    const auto destination = request.destination->identity();
    if (!destination.value() || !same(*destination.value(), request.destination_identity))
        return rejected<Pointer>(AdapterError::PlanDecisionStale, "native_operation_move.destination_identity",
                                ObservationState::Changed);

    auto source_parent_identity = request.source->parent()->identity();
    const auto destination_identity = request.destination->identity();
    if (!source_parent_identity.value() || !destination_identity.value())
        return Observation<Pointer>::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity),
             "native_operation_move.volume_identity"});

    NativeOperationCopyJob::Pointer cross_filesystem_copy;
    if (!same_volume(*source_parent_identity.value(), *destination_identity.value())) {
        const auto staging_name = generated_name(request.destination_name.encoding(), ".ck-move-staging-",
                                                 request.journal->id(), request.item);
        if (!staging_name)
            return rejected<Pointer>(AdapterError::UnrepresentableName,
                                     "native_operation_move.staging_name");
        NativeOperationCopyRequest copy_request{
            request.journal,
            request.journal_file,
            request.item,
            request.source->parent(),
            *source_parent_identity.value(),
            request.source->name(),
            *request.source->metadata().identity.value(),
            request.destination,
            *destination_identity.value(),
            *staging_name,
            request.destination_name,
            request.copy_limits,
            {},
            FileOperation::Move};
        const auto source = request.source;
        copy_request.prepare_metadata = [source, allow = request.allow_unhandled_metadata_loss](
                                            OpenedEntry &staging, std::stop_token) {
            return move_metadata(staging, *source, allow);
        };
        auto copied = NativeOperationCopyJob::create(std::move(copy_request));
        if (!copied.value())
            return Observation<Pointer>::unavailable(copied.unavailable_detail()->state,
                                                     copied.unavailable_detail()->error);
        cross_filesystem_copy = std::move(copied).take_value();
    }
    auto state = std::make_shared<State>(std::move(request));
    return Observation<Pointer>::available(Pointer(new NativeOperationMoveJob(std::move(state),
                                                                                std::move(cross_filesystem_copy))));
}

OperationExecutionState NativeOperationMoveJob::advance(std::stop_token stop) {
    if (cross_filesystem_copy_) {
        const auto state = cross_filesystem_copy_->advance(stop);
        switch (state) {
        case OperationExecutionState::Complete:
            phase_ = result_.state = OperationExecutionState::Complete;
            break;
        case OperationExecutionState::Failed:
        case OperationExecutionState::Cancelled:
        case OperationExecutionState::RetainedSource:
        case OperationExecutionState::NeedsReconciliation:
            phase_ = result_.state = state;
            if (cross_filesystem_copy_->result().failure)
                result_.failure = cross_filesystem_copy_->result().failure;
            break;
        default:
            phase_ = state;
            break;
        }
        return phase_;
    }
    if (phase_ == OperationExecutionState::Complete || phase_ == OperationExecutionState::Failed ||
        phase_ == OperationExecutionState::Cancelled || phase_ == OperationExecutionState::NeedsReconciliation)
        return phase_;

    const auto journal_state = state_->request.journal->state(state_->request.item);
    if (!journal_state) {
        result_.failure = *rejected<bool>(AdapterError::InvalidNativeResult,
                                         "native_operation_move.journal_state").unavailable_detail();
        phase_ = result_.state = OperationExecutionState::NeedsReconciliation;
        return phase_;
    }
    auto terminal_failure = [&](UnavailableObservation detail, bool reconcile) {
        result_.failure = std::move(detail);
        const auto appended = state_->append(reconcile ? OperationJournalState::NeedsReconciliation
                                                        : OperationJournalState::Failed);
        phase_ = result_.state = !appended.value() || reconcile
            ? OperationExecutionState::NeedsReconciliation : OperationExecutionState::Failed;
    };
    if (stop.stop_requested()) {
        if (*journal_state == OperationJournalState::CommitIntended) {
            terminal_failure({ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead),
                 "native_operation_move.cancel_commit"}}, true);
        } else {
            const auto appended = state_->append(OperationJournalState::Cancelled);
            phase_ = result_.state = appended.value() ? OperationExecutionState::Cancelled
                                                       : OperationExecutionState::NeedsReconciliation;
        }
        return phase_;
    }

    if (*journal_state == OperationJournalState::Planned) {
        const auto valid = state_->revalidate();
        if (!valid.value()) {
            terminal_failure(*valid.unavailable_detail(), valid.state() == ObservationState::Unknown);
            return phase_;
        }
        if (!*valid.value()) {
            terminal_failure({ObservationState::Changed,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::PlanDecisionStale),
                 "native_operation_move.source_changed"}}, false);
            return phase_;
        }
        const auto appended = state_->append(OperationJournalState::Prepared);
        if (!appended.value()) {
            terminal_failure(*appended.unavailable_detail(), true);
            return phase_;
        }
        return phase_;
    }
    if (*journal_state == OperationJournalState::Prepared) {
        const auto appended = state_->append(OperationJournalState::CommitIntended);
        if (!appended.value()) {
            terminal_failure(*appended.unavailable_detail(), true);
            return phase_;
        }
        return phase_;
    }
    if (*journal_state == OperationJournalState::CommitIntended) {
        auto moved = state_->request.source->parent()->rename_child_noreplace(
            state_->request.source->name(), *state_->request.source->metadata().identity.value(),
            *state_->request.destination, state_->request.destination_name);
        if (!moved.value()) {
            terminal_failure(*moved.unavailable_detail(), moved.state() == ObservationState::Unknown);
            return phase_;
        }
        if (!*moved.value()) {
            terminal_failure({ObservationState::Unknown,
                {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult),
                 "native_operation_move.rename_result"}}, true);
            return phase_;
        }
        const std::array<std::pair<OperationJournalState, const char *>, 4> transitions{
            std::pair{OperationJournalState::DestinationCommitted, "native_operation_move.append_destination_committed"},
            std::pair{OperationJournalState::SourceRemovalIntended, "native_operation_move.append_source_removal_intended"},
            std::pair{OperationJournalState::SourceRetired, "native_operation_move.append_source_retired"},
            std::pair{OperationJournalState::Complete, "native_operation_move.append_complete"}};
        for (const auto &[next, diagnostic] : transitions) {
            const auto appended = state_->append(next);
            if (!appended.value()) {
                result_.failure = *appended.unavailable_detail();
                result_.failure->error.operation = diagnostic;
                phase_ = result_.state = OperationExecutionState::NeedsReconciliation;
                return phase_;
            }
        }
        phase_ = result_.state = OperationExecutionState::Complete;
        return phase_;
    }
    terminal_failure({ObservationState::Unknown,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult),
         "native_operation_move.state"}}, true);
    return phase_;
}

NativeOperationMoveBatchJob::NativeOperationMoveBatchJob(std::vector<NativeOperationMoveJob::Pointer> jobs)
    : jobs_(std::move(jobs)), started_(jobs_.size(), false) {
    result_.items.reserve(jobs_.size());
    for (std::size_t index = 0; index != jobs_.size(); ++index)
        result_.items.push_back({index, OperationItemOutcome::Pending, std::nullopt});
}

Observation<NativeOperationMoveBatchJob::Pointer> NativeOperationMoveBatchJob::create(
    std::vector<NativeOperationMoveJob::Pointer> jobs) {
    if (jobs.empty() || jobs.size() > OperationJournalLimits::hard_max_items)
        return rejected<Pointer>(AdapterError::InventoryLimit, "native_operation_move_batch.jobs");
    for (const auto &job : jobs)
        if (!job)
            return rejected<Pointer>(AdapterError::PlanInvalid, "native_operation_move_batch.null_job");
    return Observation<Pointer>::available(Pointer(new NativeOperationMoveBatchJob(std::move(jobs))));
}

OperationBatchState NativeOperationMoveBatchJob::advance(std::stop_token stop) {
    if (result_.state == OperationBatchState::Complete || result_.state == OperationBatchState::Failed ||
        result_.state == OperationBatchState::Cancelled || result_.state == OperationBatchState::NeedsReconciliation)
        return result_.state;
    if (result_.state == OperationBatchState::Ready)
        result_.state = OperationBatchState::Running;
    while (current_ < jobs_.size()) {
        const auto job_state = jobs_[current_]->state();
        if (job_state != OperationExecutionState::Complete && job_state != OperationExecutionState::Failed &&
            job_state != OperationExecutionState::Cancelled && job_state != OperationExecutionState::NeedsReconciliation)
            break;
        auto &item = result_.items[current_];
        item.outcome = job_state == OperationExecutionState::Complete ? OperationItemOutcome::Complete
            : job_state == OperationExecutionState::Cancelled ? OperationItemOutcome::Cancelled
            : job_state == OperationExecutionState::NeedsReconciliation ? OperationItemOutcome::NeedsReconciliation
            : OperationItemOutcome::Failed;
        item.failure = jobs_[current_]->result().failure;
        ++current_;
    }
    if (current_ >= jobs_.size()) {
        bool failed = false, cancelled = false, reconciliation = false;
        for (const auto &item : result_.items) {
            failed = failed || item.outcome == OperationItemOutcome::Failed;
            cancelled = cancelled || item.outcome == OperationItemOutcome::Cancelled;
            reconciliation = reconciliation || item.outcome == OperationItemOutcome::NeedsReconciliation;
        }
        result_.state = reconciliation ? OperationBatchState::NeedsReconciliation
            : failed ? OperationBatchState::Failed
            : cancelled ? OperationBatchState::Cancelled : OperationBatchState::Complete;
        return result_.state;
    }
    if (stop.stop_requested()) {
        if (started_[current_])
            jobs_[current_]->advance(stop);
        result_.state = OperationBatchState::Cancelled;
        return result_.state;
    }
    started_[current_] = true;
    jobs_[current_]->advance(stop);
    return result_.state;
}

Observation<NativeOperationMoveBatchJob::Pointer> create_native_operation_move_batch(
    NativeOperationMoveBatchRequest request) {
    if (request.sources.empty() || request.sources.size() != request.destination_names.size() ||
        request.sources.size() > request.journal_limits.max_items || !request.destination ||
        !request.journal_root || !request.journal_root->is_valid() || !request.open_journal ||
        request.journal_limits.max_items == 0)
        return rejected<NativeOperationMoveBatchJob::Pointer>(AdapterError::PlanInvalid,
                                                    "native_operation_move_batch.request");
    const auto destination_identity = request.destination->identity();
    if (!destination_identity.value() || !NativeDurableIdentity::from_live(*destination_identity.value()))
        return rejected<NativeOperationMoveBatchJob::Pointer>(AdapterError::UnqualifiedIdentity,
                                                    "native_operation_move_batch.destination");

    std::vector<NativeOperationMoveJob::Pointer> jobs;
    jobs.reserve(request.sources.size());
    for (std::size_t item = 0; item != request.sources.size(); ++item) {
        const auto &source = request.sources[item];
        if (!source || !request.destination_names[item].is_valid() || !source->parent() ||
            !source->metadata().identity.value() || !source->parent_identity().value())
            return rejected<NativeOperationMoveBatchJob::Pointer>(AdapterError::PlanInvalid,
                                                        "native_operation_move_batch.source");
        const auto id = item_id(request.journal_id, item);
        const auto journal_name = generated_name(request.journal_root->encoding(), ".ck-move-journal-", id, item);
        if (!journal_name)
            return rejected<NativeOperationMoveBatchJob::Pointer>(AdapterError::UnrepresentableName,
                                                        "native_operation_move_batch.journal_name");
        auto file = request.open_journal(*request.journal_root, *journal_name, true);
        if (!file.value())
            return Observation<NativeOperationMoveBatchJob::Pointer>::unavailable(file.unavailable_detail()->state,
                                                                          file.unavailable_detail()->error);
        auto journal = OperationJournal::create(FileOperation::Move, id, request.journal_limits);
        if (!journal.value())
            return Observation<NativeOperationMoveBatchJob::Pointer>::unavailable(journal.unavailable_detail()->state,
                                                                          journal.unavailable_detail()->error);
        auto journal_pointer = std::move(journal).take_value();
        auto appended = append_operation_journal_file(**file.value(), *journal_pointer,
                                                       {1, 0, OperationJournalState::Planned, {}});
        if (!appended.value())
            return Observation<NativeOperationMoveBatchJob::Pointer>::unavailable(appended.unavailable_detail()->state,
                                                                          appended.unavailable_detail()->error);
        NativeOperationMoveRequest move_request{
            journal_pointer, std::shared_ptr<OperationJournalFile>(std::move(file).take_value()), 0,
            source, request.destination, *destination_identity.value(), request.destination_names[item],
            request.copy_limits, request.allow_unhandled_metadata_loss};
        auto job = NativeOperationMoveJob::create(std::move(move_request));
        if (!job.value())
            return Observation<NativeOperationMoveBatchJob::Pointer>::unavailable(job.unavailable_detail()->state,
                                                                          job.unavailable_detail()->error);
        jobs.push_back(std::move(job).take_value());
    }
    return NativeOperationMoveBatchJob::create(std::move(jobs));
}

} // namespace ck::files
