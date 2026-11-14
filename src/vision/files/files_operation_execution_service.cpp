// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_execution_service.hpp"
#include "native_operation_journal_id.hpp"

#include "native_operation_delete_tree.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace ck::vision {
namespace {

using ck::files::ConflictChoice;

bool replacement_choice(ConflictChoice choice) noexcept {
    return choice == ConflictChoice::UseSource || choice == ConflictChoice::Newest;
}

bool row_can_replace(const ck::files::ReviewedTreeItem& item) noexcept {
    using namespace ck::files;
    return item.facts.conflict_class == ConflictClass::ExistingRegular &&
        item.facts.retained_target_available && item.facts.source_preservation_qualified &&
        item.row.relation != ComparisonRelation::Unknown && item.row.relation != ComparisonRelation::Protected;
}

std::optional<ck::files::UnavailableObservation> first_batch_failure(
    const ck::files::OperationBatchResult &result) {
    for (const auto &item : result.items)
        if (item.failure)
            return item.failure;
    return std::nullopt;
}

ck::files::UnavailableObservation error(ck::files::AdapterError code, const char* operation,
                                        ck::files::ObservationState state = ck::files::ObservationState::Failed) {
    return {state, {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

bool keep_both_supported(const FilesOperationReviewEvidence& evidence) noexcept {
    using namespace ck::files;
    if (evidence.inventory->mount_policy() != InventoryMountPolicy::DirectObjects ||
        evidence.plan->conflicts().empty() || !evidence.plan->all_losses_reviewable())
        return false;
    for (const auto& item : evidence.plan->items())
        if (item.kind != ObjectKind::RegularFile)
            return false;
    for (const auto& conflict : evidence.plan->conflicts())
        if (conflict.kind != DestinationConflictKind::ExistingFile &&
            conflict.kind != DestinationConflictKind::TypeMismatch)
            return false;
    return true;
}

std::optional<ck::files::UnavailableObservation> prepare_keep_both_names(
    const FilesOperationReviewEvidence& evidence, const ck::files::ConsentReceipt& receipt,
    const std::shared_ptr<const ck::files::DirectoryCursor>& destination,
    std::vector<ck::files::NativeName>& names, std::stop_token stop) {
    using namespace ck::files;
    // Validate all edited names before handing any item to a journal-writing job.
    // Native no-replace still arbitrates aliases and races at publication.
    std::set<NativeName, NativeNameLess> used(names.begin(), names.end());
    std::set<std::size_t> renamed;
    for (std::size_t i = 0; i < receipt.keep_both_names.size(); ++i) {
        const auto item = evidence.plan->conflicts()[receipt.resolved_conflicts[i]].item;
        const auto& name = receipt.keep_both_names[i];
        if (name.encoding() != names[item].encoding() ||
            (name.windows_units() && !check_win32_component(*name.windows_units()).ordinary_component()))
            return error(AdapterError::UnrepresentableName, "files_execution.keep_both_name");
        if (!renamed.insert(item).second || !used.insert(name).second)
            return error(AdapterError::InvalidIdentifier, "files_execution.keep_both_duplicate");
        const auto probe = DirectoryEntryProbe::inspect(destination, name, ChildLookupMode::NativeDefault,
            {evidence.evidence_limits.destination_batch.component_bytes}, stop);
        if (const auto* failure = probe.unavailable_detail())
            return *failure;
        if (const auto& length = (*probe.value())->length(); length.value() && length.value()->exceeds_reported_maximum())
            return error(AdapterError::UnrepresentableName, "files_execution.keep_both_name");
        const auto& lookup = (*probe.value())->lookup();
        if (const auto* failure = lookup.unavailable_detail())
            return *failure;
        if (lookup.value()->state() != ChildLookupState::Missing)
            return error(AdapterError::PlanDecisionStale, "files_execution.keep_both_occupied", ObservationState::Changed);
        names[item] = name;
    }
    return {};
}

bool valid(const FilesOperationExecutionRequest& request) noexcept {
    const bool deletion = request.operation == ck::files::FileOperation::Delete;
    const bool copy_or_move = request.operation == ck::files::FileOperation::Copy ||
        request.operation == ck::files::FileOperation::Move;
    if (request.review_existing_tree &&
        (request.operation != ck::files::FileOperation::Copy || request.receipt || !request.reviewed ||
         !request.reviewed->plan || !request.reviewed->inventory ||
         request.reviewed->inventory->mount_policy() == ck::files::InventoryMountPolicy::DirectObjects ||
         request.reviewed->destination_names.size() != 1))
        return false;
    return (deletion || copy_or_move) && request.source_service && request.source_navigation.valid() &&
           request.source_stamp.current() && request.source_limits.held_entries != 0 &&
           request.journal_root.is_valid() && request.open_journal && request.policy.journal_limits.max_items != 0 &&
           (deletion ? !request.destination_service && !request.reviewed && !request.receipt
                     : request.destination_service && request.destination_service != request.source_service &&
                           request.destination_navigation.valid() && request.destination_stamp.current() &&
                           request.policy.copy_limits.valid() && request.reviewed && request.reviewed->inventory &&
                           request.reviewed->plan && request.reviewed->plan->operation() == request.operation &&
                           (request.review_existing_tree ||
                           (request.receipt && request.reviewed->plan->receipt_matches(*request.receipt) &&
                           (request.receipt->action == ck::files::ConsentAction::Proceed ||
                            request.receipt->action == ck::files::ConsentAction::ContinueWithListedLosses ||
                            (request.receipt->action == ck::files::ConsentAction::KeepBoth &&
                             keep_both_supported(*request.reviewed) &&
                             request.receipt->resolved_conflicts.size() == request.reviewed->plan->conflicts().size()) ||
                            (request.receipt->action == ck::files::ConsentAction::SkipConflict &&
                             request.reviewed->inventory->mount_policy() == ck::files::InventoryMountPolicy::DirectObjects &&
                             request.reviewed->plan->skips_conflicts_with_reviewed_losses(
                                 request.receipt->reviewed_issues, request.receipt->resolved_conflicts))))));
}

FilesOperationExecutionState execution_state(ck::files::OperationBatchState state,
                                             bool stop_requested) noexcept {
    using BatchState = ck::files::OperationBatchState;
    switch (state) {
    case BatchState::Complete:
        return FilesOperationExecutionState::Complete;
    case BatchState::Cancelled:
        return FilesOperationExecutionState::Cancelled;
    case BatchState::RetainedSource:
        return FilesOperationExecutionState::RetainedSource;
    case BatchState::NeedsReconciliation:
        return FilesOperationExecutionState::NeedsReconciliation;
    case BatchState::Ready:
    case BatchState::Running:
    case BatchState::Failed:
        return stop_requested ? FilesOperationExecutionState::Cancelled
                               : FilesOperationExecutionState::Failed;
    }
    return stop_requested ? FilesOperationExecutionState::Cancelled
                          : FilesOperationExecutionState::Failed;
}

} // namespace

bool files_transfer_action_supported(const FilesOperationReviewEvidence& evidence, ck::files::ConsentAction action) {
    using namespace ck::files;
    if (action == ConsentAction::Cancel || action == ConsentAction::ChooseAnotherDestination ||
        action == ConsentAction::Proceed || action == ConsentAction::ContinueWithListedLosses)
        return true;
    if (!evidence.plan || !evidence.inventory)
        return false;
    if (action == ConsentAction::KeepBoth)
        return keep_both_supported(evidence);
    if (action != ConsentAction::SkipConflict ||
        evidence.inventory->mount_policy() != InventoryMountPolicy::DirectObjects)
        return false;
    // Ask the same plan validator used by admission, with all rows
    // acknowledged, to see whether this action can ever be submitted.
    std::vector<std::size_t> issues(evidence.plan->issues().size()), conflicts(evidence.plan->conflicts().size());
    std::iota(issues.begin(), issues.end(), 0);
    std::iota(conflicts.begin(), conflicts.end(), 0);
    return evidence.plan->skips_conflicts_with_reviewed_losses(issues, conflicts);
}

struct FilesOperationExecutionService::State final {
    struct TreeChoice final {
        ConflictChoice default_choice;
        std::vector<FilesTreeConflictDecision> overrides;
    };
    struct Handoff final {
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
                    : error(ck::files::AdapterError::PlanInvalid, "files_execution.source_handoff");
            source_done = true;
            condition.notify_all();
        }

        void destination(ck::files::Observation<FilesPaneOperationDestination> value) {
            std::lock_guard lock(mutex);
            if (failure || destination_done)
                return;
            if (value.value())
                destination_result = std::move(value).take_value();
            else
                failure = value.unavailable_detail()
                    ? *value.unavailable_detail()
                    : error(ck::files::AdapterError::PlanInvalid, "files_execution.destination_handoff");
            destination_done = true;
            condition.notify_all();
        }

        bool wait(std::stop_token stop_token, bool destination_required) {
            std::unique_lock lock(mutex);
            while (!failure && !(source_done && (!destination_required || destination_done)) &&
                   !stop_token.stop_requested())
                condition.wait_for(lock, std::chrono::milliseconds(10));
            return !stop_token.stop_requested() && !failure && source_done &&
                   (!destination_required || destination_done);
        }
    };

    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::pair<FilesOperationExecutionTicket, FilesOperationExecutionRequest>> pending;
    std::optional<FilesOperationExecutionReply> reply;
    std::optional<FilesTreeCopyReview> tree_review;
    ck::files::ComparisonPlanIdentity tree_review_plan;
    std::optional<TreeChoice> tree_choice;
    bool tree_can_use_source = false;
    bool tree_can_apply = false;
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
        tree_review.reset();
        tree_review_plan = {};
        tree_choice.reset();
        reply = std::move(value);
        busy = false;
        signal();
    }

    ck::files::Observation<TreeChoice> wait_for_tree_choice(
        FilesTreeCopyReview review, std::stop_token stop_token) {
        using namespace ck::files;
        using Result = Observation<TreeChoice>;
        std::unique_lock lock(mutex);
        tree_review_plan = review.plan;
        tree_can_use_source = review.can_use_source;
        tree_can_apply = review.can_apply;
        tree_review = std::move(review);
        signal();
        condition.wait(lock, [&] { return stopping || stop_token.stop_requested() || tree_choice.has_value(); });
        tree_review.reset();
        tree_review_plan = {};
        auto choice = std::exchange(tree_choice, std::nullopt);
        if (choice && !stopping && !stop_token.stop_requested())
            return Result::available(std::move(*choice));
        const auto detail = error(AdapterError::InterruptedRead, "files_execution.tree_cancelled", ObservationState::Unknown);
        return Result::unavailable(detail.state, detail.error);
    }

    void run_tree_copy(FilesOperationExecutionReply& result, const FilesOperationExecutionRequest& request,
        const FilesPaneOperationSources& sources, const FilesPaneOperationDestination& destination,
        std::stop_token stop_token) {
        using namespace ck::files;
        const auto fail = [&](const UnavailableObservation& detail) {
            result.state = stop_token.stop_requested() ? FilesOperationExecutionState::Cancelled
                                                       : FilesOperationExecutionState::Failed;
            result.error = detail;
        };
        const auto current = [&] { return request.source_stamp.current() && request.destination_stamp.current(); };
        const auto stale = [&] {
            fail(error(AdapterError::PlanDecisionStale, "files_execution.tree_roots_changed", ObservationState::Changed));
        };
        const auto parent = destination.directory->identity();
        const auto& entries = request.reviewed->inventory->entries();
        if (!current() || !parent.value() ||
            entries.empty() ||
            compare_identity(*parent.value(), request.reviewed->destination) != IdentityComparison::Same ||
            !sources.root_source || !sources.root_source->metadata().identity.value() ||
            compare_identity(*sources.root_source->metadata().identity.value(), entries.front().identity) != IdentityComparison::Same ||
            sources.names != request.reviewed->destination_names) {
            stale();
            return;
        }
        const auto& name = sources.names.front();
        auto target = HeldChild::observe(destination.directory, name);
        if (const auto* failure = target.unavailable_detail()) {
            fail(*failure);
            return;
        }
        if (!(*target.value())->metadata().kind.value() ||
            *(*target.value())->metadata().kind.value() != ObjectKind::Directory) {
            stale();
            return;
        }
        ReviewedTreePlanRequest tree;
        tree.source_root = sources.root_source;
        tree.destination_root = std::move(target).take_value();
        if (destination.address) {
            auto address = destination.address->child_address(name);
            if (const auto* failure = address.unavailable_detail()) {
                fail(*failure);
                return;
            }
            tree.destination_address = std::move(address).take_value();
        }
        tree.comparison.profile = ComparisonProfile::Copy;
        tree.comparison.mode = ComparisonMode::CompleteDigest;
        tree.copy_policy = request.policy;
        tree.copy_policy.allow_unhandled_metadata_loss = false;
        tree.journal_root = request.journal_root;
        tree.journal_id = request.journal_id;
        tree.limits.max_entries = request.source_limits.held_entries;
        tree.limits.max_page_rows = request.source_limits.held_entries;
        auto created = ReviewedTreePlanJob::create(std::move(tree));
        if (const auto* failure = created.unavailable_detail()) {
            fail(*failure);
            return;
        }
        auto job = std::move(created).take_value();
        while (job->state() == ReviewedTreeState::Inventory)
            job->advance(stop_token);
        if (job->state() == ReviewedTreeState::AwaitingReview) {
            if (!current()) {
                stale();
                return;
            }
            auto assessment = job->assess_rules({}, 0, request.source_limits.held_entries);
            if (const auto* failure = assessment.unavailable_detail()) {
                fail(*failure);
                return;
            }
            auto page = std::move(assessment).take_value();
            const auto plan = page.identity;
            struct ConflictRow final {
                ConflictClass kind;
                bool can_replace;
            };
            std::map<std::uint64_t, ConflictRow> conflicts;
            bool can_use_source = true, can_apply = true;
            std::size_t conflict_count = 0;
            std::vector<FilesTreeCopyReview::Row> review_rows;
            review_rows.reserve(page.items.size());
            for (auto& item : page.items) {
                if (item.row.source_index && !item.facts.source_preservation_qualified &&
                    (item.action == ReviewedTreeAction::Copy || item.action == ReviewedTreeAction::CreateDirectory))
                    can_apply = false;
                if (item.action == ReviewedTreeAction::MergeDirectory &&
                    !(item.facts.allowed_choices & (std::uint64_t{1} << static_cast<unsigned>(ConflictChoice::MergeFolders))))
                    can_apply = false;
                if (item.row.source_index && item.row.relation == ComparisonRelation::Unknown && !item.facts.destination_present)
                    can_apply = false;
                const bool can_replace = item.row.requires_review && row_can_replace(item);
                if (item.row.requires_review || item.action == ReviewedTreeAction::MergeDirectory) {
                    conflicts.emplace(item.row.id, ConflictRow{item.facts.conflict_class, can_replace});
                    if (item.row.requires_review) {
                        ++conflict_count;
                        can_use_source = can_use_source && can_replace;
                    }
                }
                review_rows.push_back({std::move(item), can_replace});
            }
            const auto choice = wait_for_tree_choice({result.request, plan, std::move(review_rows),
                conflict_count != 0 && can_use_source, can_apply, conflict_count}, stop_token);
            if (const auto* failure = choice.unavailable_detail()) {
                job->advance(stop_token);
                fail(*failure);
                return;
            }
            if (!current() || stop_token.stop_requested()) {
                if (stop_token.stop_requested())
                    job->advance(stop_token);
                stale();
                return;
            }
            std::map<std::uint64_t, ConflictChoice> overrides;
            for (const auto& entry : choice.value()->overrides) {
                const auto row = conflicts.find(entry.row_id);
                if (row == conflicts.end() || row->second.kind == ConflictClass::DirectoryPair ||
                    (entry.choice != ConflictChoice::KeepTarget &&
                     (!replacement_choice(entry.choice) || !row->second.can_replace)) ||
                    !overrides.emplace(entry.row_id, entry.choice).second) {
                    fail(error(AdapterError::PlanInvalid, "files_execution.tree_override"));
                    return;
                }
            }
            std::map<std::pair<ConflictClass, ConflictChoice>, std::vector<std::uint64_t>> grouped;
            for (const auto& [id, row] : conflicts) {
                const auto override = overrides.find(id);
                const auto selected = row.kind == ConflictClass::DirectoryPair ? ConflictChoice::MergeFolders :
                    override == overrides.end() ? choice.value()->default_choice : override->second;
                grouped[{row.kind, selected}].push_back(id);
            }
            std::vector<ConflictRule> rules;
            for (auto& [selection, rows] : grouped) {
                ConflictRule rule;
                rule.plan = plan;
                rule.kind = selection.first;
                rule.choice = selection.second;
                rule.tie = ConflictChoice::KeepTarget;
                rule.scope = ConflictScope::Selected;
                rule.selected_rows = std::move(rows);
                rule.retain_replaced = true;
                rules.push_back(std::move(rule));
            }
            const auto receipt = job->review(rules);
            if (const auto* failure = receipt.unavailable_detail()) {
                fail(*failure);
                return;
            }
            if (!current() || stop_token.stop_requested()) {
                if (stop_token.stop_requested())
                    job->advance(stop_token);
                stale();
                return;
            }
            const auto started = job->begin_execution(*receipt.value());
            if (const auto* failure = started.unavailable_detail()) {
                fail(*failure);
                return;
            }
            while (job->state() == ReviewedTreeState::Revalidating || job->state() == ReviewedTreeState::Executing)
                job->advance(stop_token);
        }
        const auto progress = job->progress();
        result.error = progress.failure;
        if (job->preview()) {
            const auto final_page = job->page(0, request.source_limits.held_entries);
            if (final_page.value()) {
                for (const auto& item : final_page.value()->items) {
                    if (item.target_retained)
                        ++result.retained_targets;
                    if (item.state == ReviewedTreeItemState::Skipped && item.row.requires_review)
                        result.skipped_items.push_back(static_cast<std::size_t>(item.row.id - 1));
                    if (item.state == ReviewedTreeItemState::Complete &&
                        (item.action == ReviewedTreeAction::Copy || item.action == ReviewedTreeAction::Replace ||
                         item.action == ReviewedTreeAction::CreateDirectory || item.action == ReviewedTreeAction::MergeDirectory))
                        result.result.items.push_back({static_cast<std::size_t>(item.row.id - 1), OperationItemOutcome::Complete, {}});
                }
            }
        }
        if (progress.state == ReviewedTreeState::Complete) {
            result.state = FilesOperationExecutionState::Complete;
            result.result.state = OperationBatchState::Complete;
        } else if (progress.state == ReviewedTreeState::Partial || progress.state == ReviewedTreeState::NeedsReconciliation) {
            result.state = FilesOperationExecutionState::NeedsReconciliation;
        } else if (progress.state == ReviewedTreeState::Cancelled) {
            result.state = FilesOperationExecutionState::Cancelled;
        }
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

        if (stop_token.stop_requested()) {
            fail(error(ck::files::AdapterError::InterruptedRead, "files_execution.cancelled",
                       ck::files::ObservationState::Unknown));
            return;
        }
        if (value.journal_id == ck::files::OperationJournalId{}) {
            const auto id = ck::files::new_native_operation_journal_id();
            if (!id.value()) {
                fail(*id.unavailable_detail());
                return;
            }
            value.journal_id = *id.value();
        }
        auto handoff = std::make_shared<Handoff>();
        const bool deletion = value.operation == ck::files::FileOperation::Delete;
        const auto source_submission = value.source_service->acquire_operation_sources(
            value.source_navigation, value.source_stamp, value.source_limits,
            [handoff](ck::files::Observation<FilesPaneOperationSources> sources) {
                handoff->source(std::move(sources));
            });
        FilesPaneSubmitted destination_submission{FilesPaneSubmission::Accepted, {}};
        if (!deletion) {
            destination_submission = value.destination_service->acquire_operation_destination(
                value.destination_navigation, value.destination_stamp,
                [handoff](ck::files::Observation<FilesPaneOperationDestination> destination) {
                    handoff->destination(std::move(destination));
                });
        }
        if (source_submission.state != FilesPaneSubmission::Accepted ||
            (!deletion && destination_submission.state != FilesPaneSubmission::Accepted)) {
            if (source_submission.state == FilesPaneSubmission::Accepted)
                value.source_service->cancel(source_submission.ticket);
            if (!deletion && destination_submission.state == FilesPaneSubmission::Accepted)
                value.destination_service->cancel(destination_submission.ticket);
            fail(error(ck::files::AdapterError::InvalidAccess, "files_execution.pane_submission",
                       ck::files::ObservationState::Changed));
            return;
        }
        if (!handoff->wait(stop_token, !deletion)) {
            value.source_service->cancel(source_submission.ticket);
            if (!deletion)
                value.destination_service->cancel(destination_submission.ticket);
            std::optional<ck::files::UnavailableObservation> failure;
            {
                std::lock_guard lock(handoff->mutex);
                failure = std::move(handoff->failure);
            }
            if (failure)
                fail(std::move(*failure));
            else
                fail(error(ck::files::AdapterError::InterruptedRead, "files_execution.handoff_cancelled",
                           ck::files::ObservationState::Unknown));
            return;
        }
        if (!handoff->sources || (!deletion &&
            (!handoff->destination_result || !handoff->destination_result->directory ||
             !handoff->destination_result->provider_root))) {
            fail(error(ck::files::AdapterError::PlanInvalid, "files_execution.handoff_shape"));
            return;
        }
        const bool recursive = handoff->sources->root_source && handoff->sources->source_root &&
            handoff->sources->source_path && handoff->sources->source_path->is_valid() &&
            handoff->sources->sources.empty() && handoff->sources->paths.empty();
        if (value.review_existing_tree) {
            run_tree_copy(result, value, *handoff->sources, *handoff->destination_result, stop_token);
            emit(std::move(result));
            return;
        }
        if (deletion) {
            if (!recursive) {
                fail(error(ck::files::AdapterError::UnsupportedQuery,
                           "files_execution.delete_requires_recursive_directory",
                           ck::files::ObservationState::Unsupported));
                return;
            }
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
                               "files_execution.delete_inventory_cancelled",
                               ck::files::ObservationState::Unknown));
                    return;
                }
                inventory_job->advance(stop_token);
            }
            if (inventory_job->state() != ck::files::RecursiveInventoryState::Complete) {
                if (inventory_job->failure()) fail(*inventory_job->failure());
                else fail(error(ck::files::AdapterError::InvalidNativeResult,
                                "files_execution.delete_inventory"));
                return;
            }
            auto completed = inventory_job->completed_sources();
            if (!completed.value()) {
                fail(completed.unavailable_detail() ? *completed.unavailable_detail() :
                     error(ck::files::AdapterError::InvalidNativeResult,
                           "files_execution.delete_inventory_result"));
                return;
            }
            ck::files::NativeOperationDeleteTreeRequest delete_request;
            delete_request.inventory = std::move(completed).take_value();
            delete_request.journal_root = std::move(value.journal_root);
            delete_request.journal_id = value.journal_id;
            delete_request.journal_limits = value.policy.journal_limits;
            delete_request.open_journal = std::move(value.open_journal);
            auto job = ck::files::NativeOperationDeleteTreeJob::create(std::move(delete_request));
            if (!job.value()) {
                fail(*job.unavailable_detail());
                return;
            }
            auto deletion_job = std::move(job).take_value();
            while (deletion_job->state() != ck::files::OperationExecutionState::Complete &&
                   deletion_job->state() != ck::files::OperationExecutionState::Failed &&
                   deletion_job->state() != ck::files::OperationExecutionState::Cancelled &&
                   deletion_job->state() != ck::files::OperationExecutionState::NeedsReconciliation)
                deletion_job->advance(stop_token);
            const auto deletion_state = deletion_job->state();
            const auto outcome = deletion_state == ck::files::OperationExecutionState::Complete
                ? ck::files::OperationItemOutcome::Complete
                : deletion_state == ck::files::OperationExecutionState::Cancelled
                    ? ck::files::OperationItemOutcome::Cancelled
                    : deletion_state == ck::files::OperationExecutionState::NeedsReconciliation
                        ? ck::files::OperationItemOutcome::NeedsReconciliation
                        : ck::files::OperationItemOutcome::Failed;
            result.result.items.push_back({0, outcome, deletion_job->result().failure});
            result.result.state = outcome == ck::files::OperationItemOutcome::Complete
                ? ck::files::OperationBatchState::Complete
                : outcome == ck::files::OperationItemOutcome::Cancelled
                    ? ck::files::OperationBatchState::Cancelled
                    : outcome == ck::files::OperationItemOutcome::NeedsReconciliation
                        ? ck::files::OperationBatchState::NeedsReconciliation
                        : ck::files::OperationBatchState::Failed;
            result.state = execution_state(result.result.state, stop_token.stop_requested());
            if (result.state != FilesOperationExecutionState::Complete)
                result.error = deletion_job->result().failure;
            emit(std::move(result));
            return;
        }
        auto evidence = files_operation_evidence_request(
            value.operation, std::move(*handoff->sources), *handoff->destination_result);
        if (const auto* failure = evidence.unavailable_detail()) {
            fail(*failure);
            return;
        }
        FilesOperationNativePlanningRequest fresh_request;
        fresh_request.evidence = std::move(evidence).take_value();
        fresh_request.evidence.mount_policy = value.reviewed->mount_policy;
        fresh_request.evidence.link_policy = value.reviewed->link_policy;
        fresh_request.evidence.limits = value.reviewed->evidence_limits;
        fresh_request.projection_policy = value.reviewed->projection_policy;
        fresh_request.limits = value.reviewed->limits;
        std::shared_ptr<const ck::files::CompletedRecursiveInventory> sources;
        auto collected = collect_files_operation_evidence(std::move(fresh_request), stop_token, &sources);
        if (const auto* failure = collected.unavailable_detail()) {
            fail(*failure);
            return;
        }
        const auto& fresh = **collected.value();
        const auto revalidated = value.reviewed->plan->revalidate(*value.receipt, fresh.plan->items(),
            fresh.plan->issues(), fresh.plan->conflicts());
        if (const auto* failure = revalidated.unavailable_detail()) {
            fail(*failure);
            return;
        }
        if (!value.source_stamp.current() || !value.destination_stamp.current() ||
            !value.reviewed->same_namespace_and_sources(fresh)) {
            fail(error(ck::files::AdapterError::PlanDecisionStale, "files_execution.review_evidence_changed",
                       ck::files::ObservationState::Changed));
            return;
        }
        if (stop_token.stop_requested()) {
            fail(error(ck::files::AdapterError::InterruptedRead, "files_execution.cancelled",
                       ck::files::ObservationState::Unknown));
            return;
        }
        value.policy.allow_unhandled_metadata_loss =
            revalidated.value()->action == ck::files::ConsentAction::ContinueWithListedLosses;

        std::vector<ck::files::HeldChild::Pointer> direct_sources;
        std::vector<ck::files::NativeName> direct_names;
        std::vector<std::size_t> direct_items;
        if (!recursive) {
            auto destination_names = fresh.destination_names;
            if (revalidated.value()->action == ck::files::ConsentAction::KeepBoth) {
                if (const auto failure = prepare_keep_both_names(fresh, *revalidated.value(),
                        handoff->destination_result->directory, destination_names, stop_token)) {
                    fail(*failure);
                    return;
                }
                if (!value.source_stamp.current() || !value.destination_stamp.current()) {
                    fail(error(ck::files::AdapterError::PlanDecisionStale, "files_execution.review_evidence_changed",
                               ck::files::ObservationState::Changed));
                    return;
                }
                if (stop_token.stop_requested()) {
                    fail(error(ck::files::AdapterError::InterruptedRead, "files_execution.cancelled",
                               ck::files::ObservationState::Unknown));
                    return;
                }
            }
            std::vector<bool> skipped(sources->sources().size());
            if (revalidated.value()->action == ck::files::ConsentAction::SkipConflict)
                for (const auto& conflict : fresh.plan->conflicts())
                    skipped[conflict.item] = true;
            for (const auto index : revalidated.value()->reviewed_issues) {
                if (!skipped[fresh.plan->issues()[index].item]) {
                    value.policy.allow_unhandled_metadata_loss = true;
                    break;
                }
            }
            for (std::size_t i = 0; i < skipped.size(); ++i) {
                if (skipped[i]) {
                    result.skipped_items.push_back(i);
                } else {
                    direct_sources.push_back(sources->sources()[i]);
                    direct_names.push_back(std::move(destination_names[i]));
                    direct_items.push_back(i);
                }
            }
            sources.reset();
            if (direct_sources.empty()) {
                result.result.state = ck::files::OperationBatchState::Complete;
                result.state = FilesOperationExecutionState::Complete;
                emit(std::move(result));
                return;
            }
        }
        const auto remap_direct_items = [&] {
            for (auto& item : result.result.items)
                item.item = direct_items[item.item];
        };

        if (value.operation == ck::files::FileOperation::Move && !recursive) {
            ck::files::NativeOperationMoveBatchRequest move;
            move.sources = std::move(direct_sources);
            move.destination = std::move(handoff->destination_result->directory);
            move.destination_names = std::move(direct_names);
            move.journal_root = std::move(value.journal_root);
            move.journal_id = value.journal_id;
            move.journal_limits = value.policy.journal_limits;
            move.open_journal = std::move(value.open_journal);
            move.copy_limits = value.policy.copy_limits;
            move.allow_unhandled_metadata_loss = value.policy.allow_unhandled_metadata_loss;
            auto batch = ck::files::create_native_operation_move_batch(std::move(move));
            if (!batch.value()) {
                fail(*batch.unavailable_detail());
                return;
            }
            auto job = std::move(batch).take_value();
            while (job->state() == ck::files::OperationBatchState::Ready ||
                   job->state() == ck::files::OperationBatchState::Running)
                job->advance(stop_token);
            result.result = job->result();
            remap_direct_items();
            result.state = execution_state(job->state(), stop_token.stop_requested());
            if (result.state != FilesOperationExecutionState::Complete && !result.result.items.empty())
                result.error = first_batch_failure(result.result);
            emit(std::move(result));
            return;
        }
        if (recursive) {
            if (value.operation == ck::files::FileOperation::Move) {
                const auto source_parent = sources->sources().front()->parent_identity();
                const auto destination_identity = handoff->destination_result->directory->identity();
                if (!source_parent.value() || !destination_identity.value()) {
                    fail(error(ck::files::AdapterError::UnqualifiedIdentity,
                               "files_execution.recursive_move.volume_identity",
                               ck::files::ObservationState::Unknown));
                    return;
                }
                const auto &source_volume = *source_parent.value();
                const auto &destination_volume = *destination_identity.value();
                const bool same_filesystem = source_volume.scope == destination_volume.scope &&
                    source_volume.scheme == destination_volume.scheme &&
                    source_volume.volume == destination_volume.volume;
                if (same_filesystem) {
                    // Retire the descendant handles before Windows renames the held directory.
                    ck::files::NativeOperationMoveBatchRequest move;
                    move.sources.push_back(sources->sources().front());
                    sources.reset();
                    move.destination = std::move(handoff->destination_result->directory);
                    move.destination_names = fresh.destination_names;
                    move.journal_root = std::move(value.journal_root);
                    move.journal_id = value.journal_id;
                    move.journal_limits = value.policy.journal_limits;
                    move.open_journal = std::move(value.open_journal);
                    move.copy_limits = value.policy.copy_limits;
                    move.allow_unhandled_metadata_loss = value.policy.allow_unhandled_metadata_loss;
                    auto batch = ck::files::create_native_operation_move_batch(std::move(move));
                    if (!batch.value()) {
                        fail(*batch.unavailable_detail());
                        return;
                    }
                    auto job = std::move(batch).take_value();
                    while (job->state() == ck::files::OperationBatchState::Ready ||
                           job->state() == ck::files::OperationBatchState::Running)
                        job->advance(stop_token);
                    result.result = job->result();
                    result.state = execution_state(job->state(), stop_token.stop_requested());
                    if (result.state != FilesOperationExecutionState::Complete && !result.result.items.empty())
                        result.error = first_batch_failure(result.result);
                    emit(std::move(result));
                    return;
                }
            }
            ck::files::NativeOperationCopyTreeRequest tree_request(fresh.destination_names.front());
            tree_request.inventory = std::move(sources);
            tree_request.destination = std::move(handoff->destination_result->directory);
            tree_request.journal_root = std::move(value.journal_root);
            tree_request.journal_id = value.journal_id;
            tree_request.policy = value.policy;
            tree_request.operation = value.operation;
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
                   job->state() != ck::files::OperationExecutionState::RetainedSource &&
                   job->state() != ck::files::OperationExecutionState::NeedsReconciliation)
                job->advance(stop_token);
            const auto tree_state = job->state();
            const auto outcome = tree_state == ck::files::OperationExecutionState::Complete
                ? ck::files::OperationItemOutcome::Complete
                : tree_state == ck::files::OperationExecutionState::Cancelled
                    ? ck::files::OperationItemOutcome::Cancelled
                    : tree_state == ck::files::OperationExecutionState::NeedsReconciliation
                        ? ck::files::OperationItemOutcome::NeedsReconciliation
                    : tree_state == ck::files::OperationExecutionState::RetainedSource
                        ? ck::files::OperationItemOutcome::RetainedSource
                        : ck::files::OperationItemOutcome::Failed;
            result.result.items.push_back({0, outcome, job->result().failure});
            result.result.state = outcome == ck::files::OperationItemOutcome::Complete
                ? ck::files::OperationBatchState::Complete
                : outcome == ck::files::OperationItemOutcome::Cancelled
                    ? ck::files::OperationBatchState::Cancelled
                    : outcome == ck::files::OperationItemOutcome::NeedsReconciliation
                        ? ck::files::OperationBatchState::NeedsReconciliation
                    : outcome == ck::files::OperationItemOutcome::RetainedSource
                        ? ck::files::OperationBatchState::RetainedSource
                        : ck::files::OperationBatchState::Failed;
            result.state = execution_state(result.result.state, stop_token.stop_requested());
            if (result.state != FilesOperationExecutionState::Complete)
                result.error = job->result().failure;
            emit(std::move(result));
            return;
        }
        ck::files::NativeOperationCopyBatchRequest copy;
        copy.sources = std::move(direct_sources);
        copy.destination = std::move(handoff->destination_result->directory);
        copy.destination_address = handoff->destination_result->address;
        copy.destination_names = std::move(direct_names);
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
        remap_direct_items();
        result.state = execution_state(job->state(), stop_token.stop_requested());
        if (result.state != FilesOperationExecutionState::Complete && !result.result.items.empty())
            result.error = first_batch_failure(result.result);
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
        stop = std::stop_source{};
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
std::optional<FilesTreeCopyReview> FilesOperationExecutionService::take_tree_review() {
    std::lock_guard lock(state_->mutex);
    if (!state_->tree_review)
        return {};
    state_->signalled = false;
    return std::exchange(state_->tree_review, std::nullopt);
}
bool FilesOperationExecutionService::apply_tree_review(const FilesOperationExecutionTicket& request,
    ck::files::ComparisonPlanIdentity plan, ck::files::ConflictChoice choice,
    std::vector<FilesTreeConflictDecision> overrides) {
    using ck::files::ConflictChoice;
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || state_->stop.stop_requested() || state_->latest != request ||
        !plan.is_valid() || state_->tree_review_plan != plan || state_->tree_choice || !state_->tree_can_apply ||
        overrides.size() > ck::files::ComparisonLimits::hard_max_page_rows ||
        (choice != ConflictChoice::KeepTarget &&
         (!state_->tree_can_use_source || !replacement_choice(choice))))
        return false;
    state_->tree_choice = State::TreeChoice{choice, std::move(overrides)};
    state_->condition.notify_one();
    return true;
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
