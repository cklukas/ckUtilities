// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_operation_delete_tree.hpp"

#include <algorithm>
#include <string>

namespace ck::files {
namespace {
template <class T>
Observation<T> rejected(AdapterError code, const char* operation,
                        ObservationState state = ObservationState::Failed) {
    return Observation<T>::unavailable(state,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

UnavailableObservation error(AdapterError code, const char* operation,
                              ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

bool terminal(OperationExecutionState state) noexcept {
    return state == OperationExecutionState::Complete || state == OperationExecutionState::Failed ||
        state == OperationExecutionState::Cancelled || state == OperationExecutionState::NeedsReconciliation;
}

std::optional<NativeName> generated_name(NameEncoding encoding, const char* prefix,
                                         const OperationJournalId& id) {
    std::string text(prefix);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : id.bytes) {
        const auto value = std::to_integer<unsigned char>(byte);
        text.push_back(hex[value >> 4U]);
        text.push_back(hex[value & 0x0FU]);
    }
    if (encoding == NameEncoding::PosixBytes) return NativeName::from_posix(text);
    if (encoding == NameEncoding::WindowsUtf16) {
        std::u16string units;
        units.reserve(text.size());
        for (const auto character : text) units.push_back(static_cast<char16_t>(static_cast<unsigned char>(character)));
        return NativeName::from_windows(std::move(units));
    }
    return std::nullopt;
}
} // namespace

struct NativeOperationDeleteTreeJob::State final {
    explicit State(NativeOperationDeleteTreeRequest value, OperationJournal::Pointer journal_value)
        : request(std::move(value)), journal(std::move(journal_value)), sources(request.inventory->sources()) {}

    NativeOperationDeleteTreeRequest request;
    OperationJournal::Pointer journal;
    std::vector<HeldChild::Pointer> sources;
    std::size_t next = 0;

    const FrozenInventory& inventory() const noexcept { return *request.inventory->inventory(); }

    Observation<bool> append(std::size_t item, OperationJournalState state) {
        std::vector<std::byte> evidence;
        if (state != OperationJournalState::Planned) {
            const auto& source = sources[item];
            if (!source || !source->parent_identity().value() || !source->metadata().identity.value())
                return rejected<bool>(AdapterError::UnqualifiedIdentity,
                                      "native_operation_delete_tree.evidence_identity", ObservationState::Unknown);
            const auto parent = NativeDurableIdentity::from_live(*source->parent_identity().value());
            const auto child = NativeDurableIdentity::from_live(*source->metadata().identity.value());
            if (!parent || !child)
                return rejected<bool>(AdapterError::UnqualifiedIdentity,
                                      "native_operation_delete_tree.evidence_identity", ObservationState::Unknown);
            auto encoded = encode_native_delete_recovery_evidence({source->name(), *parent, *child});
            if (!encoded.value())
                return Observation<bool>::unavailable(encoded.state(), encoded.unavailable_detail()->error);
            evidence = std::move(encoded).take_value();
        }
        const auto sequence = static_cast<std::uint64_t>(journal->records().size()) + 1;
        return append_operation_journal_file(*request.journal_file, *journal,
            {sequence, item, state, std::move(evidence)});
    }

    Observation<bool> source_current(std::size_t item) const {
        const auto& source = sources[item];
        const auto& expected = inventory().entries()[item];
        if (!source || !source->metadata().identity.value() ||
            compare_identity(*source->metadata().identity.value(), expected.identity) != IdentityComparison::Same ||
            !source->metadata().kind.value() || *source->metadata().kind.value() != expected.kind)
            return Observation<bool>::available(false);
        const auto parent = source->parent()->identity();
        if (!parent.value() || !source->parent_identity().value() ||
            compare_identity(*parent.value(), *source->parent_identity().value()) != IdentityComparison::Same)
            return parent.value() ? Observation<bool>::available(false)
                                  : Observation<bool>::unavailable(parent.state(), parent.unavailable_detail()->error);
        auto current = source->inspect_basic();
        if (!current.value())
            return Observation<bool>::unavailable(current.state(), current.unavailable_detail()->error);
        return Observation<bool>::available(
            current.value()->identity.value() && current.value()->kind.value() &&
            compare_identity(*current.value()->identity.value(), expected.identity) == IdentityComparison::Same &&
            *current.value()->kind.value() == expected.kind);
    }
};

Observation<NativeOperationDeleteTreeJob::Pointer> NativeOperationDeleteTreeJob::create(
    NativeOperationDeleteTreeRequest request) {
    if (!request.inventory || !request.inventory->inventory() ||
        (!request.journal_file && (!request.journal_root || !request.journal_root->is_valid() || !request.open_journal)) ||
        !std::any_of(request.journal_id.bytes.begin(), request.journal_id.bytes.end(),
                     [](std::byte value) { return value != std::byte{}; }) ||
        request.journal_limits.max_items == 0 || request.journal_limits.max_records == 0)
        return rejected<Pointer>(AdapterError::PlanInvalid, "native_operation_delete_tree.request");
    const auto& inventory = *request.inventory->inventory();
    const auto& entries = inventory.entries();
    const auto& sources = request.inventory->sources();
    if (entries.empty() || entries.size() != sources.size() || entries.size() > request.journal_limits.max_items ||
        entries.size() > request.journal_limits.max_records / 5 ||
        inventory.link_policy() != InventoryLinkPolicy::PreserveLinks ||
        entries.front().kind != ObjectKind::Directory || !entries.front().relative_components.empty())
        return rejected<Pointer>(AdapterError::PlanInvalid, "native_operation_delete_tree.inventory");

    for (std::size_t index = 0; index != entries.size(); ++index) {
        const auto& source = sources[index];
        const auto& entry = entries[index];
        if (!source || !source->parent_identity().value() || !source->metadata().identity.value() ||
            compare_identity(*source->metadata().identity.value(), entry.identity) != IdentityComparison::Same ||
            !source->metadata().kind.value() || *source->metadata().kind.value() != entry.kind ||
            !NativeDurableIdentity::from_live(entry.identity) ||
            !NativeDurableIdentity::from_live(*source->parent_identity().value()))
            return rejected<Pointer>(AdapterError::PlanDecisionStale,
                                    "native_operation_delete_tree.source_identity", ObservationState::Changed);
        if (!source->name().is_valid())
            return rejected<Pointer>(AdapterError::InvalidIdentifier, "native_operation_delete_tree.source_name");
    }

    auto journal = OperationJournal::create(FileOperation::Delete, request.journal_id, request.journal_limits);
    if (!journal.value())
        return Observation<Pointer>::unavailable(journal.state(), journal.unavailable_detail()->error);
    auto journal_pointer = std::move(journal).take_value();
    if (!request.journal_file) {
        const auto journal_name = generated_name(request.journal_root->encoding(), ".ck-delete-tree-journal-",
                                                 request.journal_id);
        if (!journal_name)
            return rejected<Pointer>(AdapterError::UnrepresentableName,
                                    "native_operation_delete_tree.generated_name");
        auto opened = request.open_journal(*request.journal_root, *journal_name, true);
        if (!opened.value())
            return Observation<Pointer>::unavailable(opened.state(), opened.unavailable_detail()->error);
        request.journal_file = std::shared_ptr<OperationJournalFile>(std::move(opened).take_value());
    }
    for (std::size_t item = 0; item != entries.size(); ++item) {
        const auto sequence = static_cast<std::uint64_t>(journal_pointer->records().size()) + 1;
        auto appended = append_operation_journal_file(*request.journal_file, *journal_pointer,
            {sequence, item, OperationJournalState::Planned, {}});
        if (!appended.value())
            return Observation<Pointer>::unavailable(appended.state(), appended.unavailable_detail()->error);
    }
    auto state = std::make_shared<State>(std::move(request), std::move(journal_pointer));
    state->next = entries.size();
    auto result = Pointer(new NativeOperationDeleteTreeJob(std::move(state)));
    result->result_.state = result->state_;
    return Observation<Pointer>::available(std::move(result));
}

OperationProgress NativeOperationDeleteTreeJob::progress() const noexcept {
    const auto total = state_data_ && state_data_->request.inventory && state_data_->request.inventory->inventory()
        ? std::optional<std::uint64_t>(state_data_->request.inventory->inventory()->logical_bytes()) : std::nullopt;
    return {state_, 0, total};
}

OperationExecutionState NativeOperationDeleteTreeJob::advance(std::stop_token stop) {
    if (terminal(state_)) return state_;
    const auto& entries = state_data_->inventory().entries();
    if (state_data_->next == 0) {
        state_ = result_.state = OperationExecutionState::Complete;
        return state_;
    }
    const auto item = state_data_->next - 1;
    const auto journal_state = state_data_->journal->state(item);
    auto finish = [&](OperationJournalState journal_state_to, UnavailableObservation failure,
                      OperationExecutionState state) {
        auto appended = state_data_->append(item, journal_state_to);
        if (!appended.value()) {
            result_.failure = *appended.unavailable_detail();
            state_ = result_.state = OperationExecutionState::NeedsReconciliation;
        } else {
            result_.failure = std::move(failure);
            state_ = result_.state = state;
        }
    };
    if (!journal_state) {
        finish(OperationJournalState::NeedsReconciliation,
               error(AdapterError::InvalidNativeResult, "native_operation_delete_tree.journal_state"),
               OperationExecutionState::NeedsReconciliation);
        return state_;
    }
    if (stop.stop_requested()) {
        finish(OperationJournalState::Cancelled,
               error(AdapterError::InterruptedRead, "native_operation_delete_tree.cancelled", ObservationState::Unknown),
               OperationExecutionState::Cancelled);
        return state_;
    }
    if (*journal_state == OperationJournalState::Planned) {
        auto current = state_data_->source_current(item);
        if (!current.value()) {
            finish(OperationJournalState::NeedsReconciliation, *current.unavailable_detail(),
                   OperationExecutionState::NeedsReconciliation);
        } else if (!*current.value()) {
            finish(OperationJournalState::Failed,
                   error(AdapterError::MetadataChanged, "native_operation_delete_tree.changed", ObservationState::Changed),
                   OperationExecutionState::Failed);
        } else {
            auto prepared = state_data_->append(item, OperationJournalState::Prepared);
            if (!prepared.value()) {
                result_.failure = *prepared.unavailable_detail();
                state_ = result_.state = OperationExecutionState::NeedsReconciliation;
            }
        }
        return state_;
    }
    if (*journal_state == OperationJournalState::Prepared) {
        auto intended = state_data_->append(item, OperationJournalState::SourceRemovalIntended);
        if (!intended.value()) {
            result_.failure = *intended.unavailable_detail();
            state_ = result_.state = OperationExecutionState::NeedsReconciliation;
        }
        return state_;
    }
    if (*journal_state == OperationJournalState::SourceRemovalIntended) {
        auto current = state_data_->source_current(item);
        if (!current.value()) {
            finish(OperationJournalState::NeedsReconciliation, *current.unavailable_detail(),
                   OperationExecutionState::NeedsReconciliation);
            return state_;
        }
        if (!*current.value()) {
            finish(OperationJournalState::NeedsReconciliation,
                   error(AdapterError::MetadataChanged, "native_operation_delete_tree.changed_before_retire",
                         ObservationState::Changed), OperationExecutionState::NeedsReconciliation);
            return state_;
        }
        auto retired = state_data_->sources[item]->parent()->retire_child(
            state_data_->sources[item]->name(), entries[item].identity);
        if (!retired.value()) {
            finish(retired.state() == ObservationState::Unknown ? OperationJournalState::NeedsReconciliation
                                                                 : OperationJournalState::Failed,
                   *retired.unavailable_detail(), retired.state() == ObservationState::Unknown
                       ? OperationExecutionState::NeedsReconciliation : OperationExecutionState::Failed);
            return state_;
        }
        if (!*retired.value()) {
            finish(OperationJournalState::NeedsReconciliation,
                   error(AdapterError::InvalidNativeResult, "native_operation_delete_tree.retire_result",
                         ObservationState::Unknown), OperationExecutionState::NeedsReconciliation);
            return state_;
        }
        auto completed = state_data_->append(item, OperationJournalState::SourceRetired);
        if (!completed.value()) {
            result_.failure = *completed.unavailable_detail();
            state_ = result_.state = OperationExecutionState::NeedsReconciliation;
            return state_;
        }
        completed = state_data_->append(item, OperationJournalState::Complete);
        if (!completed.value()) {
            result_.failure = *completed.unavailable_detail();
            state_ = result_.state = OperationExecutionState::NeedsReconciliation;
            return state_;
        }
        state_data_->sources[item].reset();
        --state_data_->next;
        if (state_data_->next == 0) state_ = result_.state = OperationExecutionState::Complete;
        return state_;
    }
    if (*journal_state == OperationJournalState::Complete) {
        --state_data_->next;
        if (state_data_->next == 0) state_ = result_.state = OperationExecutionState::Complete;
        return state_;
    }
    finish(OperationJournalState::NeedsReconciliation,
           error(AdapterError::InvalidNativeResult, "native_operation_delete_tree.state"),
           OperationExecutionState::NeedsReconciliation);
    return state_;
}
} // namespace ck::files
