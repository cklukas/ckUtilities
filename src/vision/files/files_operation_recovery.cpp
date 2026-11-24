// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery.hpp"
#include "directory_cursor.hpp"
#include "files_recovery_finalization.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

namespace ck::vision {
namespace {

template <class T>
ck::files::Observation<T> error(ck::files::AdapterError code, const char *operation,
                                ck::files::ObservationState state = ck::files::ObservationState::Failed) {
    return ck::files::Observation<T>::unavailable(state,
        {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

std::optional<FilesOperationJournalKind> journal_kind(const ck::files::NativeName &name) {
    const auto starts_with = [&](std::string_view prefix) {
        if (const auto *bytes = name.posix_bytes())
            return bytes->starts_with(prefix);
        const auto *units = name.windows_units();
        return units && units->size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), units->begin(), [](char byte, char16_t unit) {
                return static_cast<unsigned char>(byte) == unit;
            });
    };
    if (starts_with(".ck-retention-"))
        return FilesOperationJournalKind::RetainedTarget;
    constexpr std::string_view prefixes[]{".ck-copy-journal-", ".ck-tree-journal-", ".ck-move-journal-",
                                          ".ck-delete-tree-journal-", ".ck-reviewed-journal-"};
    if (std::any_of(std::begin(prefixes), std::end(prefixes), starts_with))
        return FilesOperationJournalKind::Transfer;
    return std::nullopt;
}

bool valid_limits(const FilesOperationRecoveryLimits &limits) {
    return limits.max_journals != 0 && limits.max_journals <= 16 * 1024 &&
           limits.journal.max_items != 0 && limits.journal.max_records != 0 &&
           limits.codec.max_bytes != 0 && limits.evidence.max_bytes != 0 &&
           limits.max_scan_batches != 0 && limits.max_scan_batches <= 4096;
}

} // namespace

ck::files::Observation<FilesOperationRecoveryScan> discover_operation_recovery(
    const ck::files::NativeAbsolutePath &private_root, FilesOperationRecoveryLimits limits) {
    if (!private_root.is_valid() || !valid_limits(limits))
        return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InvalidLimit,
                                                 "operation_recovery.scan_limits");
    // Acquire this exact native directory once. Every journal read is relative
    // to its retained handle; presentation text and navigation paths never
    // become reopening authority.
    auto opened = ck::files::open_native_operation_journal_directory(private_root);
    if (!opened.value())
        return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
            opened.state(), opened.unavailable_detail()->error);
    auto cursor = std::move(opened).take_value();
    return detail::finish_recovery_observation<FilesOperationRecoveryScan>(
        [&]() -> ck::files::Observation<FilesOperationRecoveryScan> {
            FilesOperationRecoveryScan result;
            auto remaining_batches = limits.max_scan_batches;
            for (;;) {
                if (!remaining_batches)
                    return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InventoryLimit,
                        "operation_recovery.scan_batches", ck::files::ObservationState::Unknown);
                // read() limits returned names but always caps native steps at
                // max_batch_size, including skipped records. Charge a whole batch.
                auto batch = cursor->read(std::min(limits.max_journals, ck::files::DirectoryCursor::max_batch_size));
                --remaining_batches;
                if (batch.error)
                    return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
                        batch.error->state, batch.error->error);
                for (const auto &name : batch.names) {
                    const auto kind = journal_kind(name);
                    if (!kind)
                        continue;
                    if (result.journals.size() >= limits.max_journals)
                        return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InventoryLimit,
                            "operation_recovery.count", ck::files::ObservationState::Unknown);

                    FilesOperationRecoveryItem item;
                    item.native_name = name;
                    item.display_name = name.display_name();
                    item.kind = *kind;
                    auto file = ck::files::open_held_operation_journal_file(*cursor, name);
                    if (!file.value()) {
                        item.error = *file.unavailable_detail();
                        result.requires_reconciliation = true;
                    } else {
                        auto inspection = detail::finish_recovery_observation<ck::files::NativeOperationRecoveryInspection>(
                            [&] {
                                if (item.kind == FilesOperationJournalKind::RetainedTarget)
                                    return ck::files::inspect_native_retention_recovery(
                                        **file.value(), limits.journal, limits.codec, limits.evidence);
                                return ck::files::inspect_native_operation_recovery_evidence(
                                    **file.value(), limits.journal, limits.codec, limits.evidence);
                            },
                            [&] { return (*file.value())->finish_io(); }, "operation_recovery.finish_io");
                        if (!inspection.value()) {
                            item.error = *inspection.unavailable_detail();
                            result.requires_reconciliation = true;
                        } else {
                            result.requires_reconciliation = result.requires_reconciliation ||
                                inspection.value()->report.requires_reconciliation;
                            item.report = FilesOperationRecoveryReport{
                                inspection.value()->journal_id, std::move(inspection).take_value()};
                        }
                    }
                    result.journals.push_back(std::move(item));
                }
                if (batch.state == ck::files::DirectoryReadState::End)
                    break;
                if (batch.state != ck::files::DirectoryReadState::More)
                    return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InterruptedRead,
                        "operation_recovery.enumerate", ck::files::ObservationState::Unknown);
            }
            return ck::files::Observation<FilesOperationRecoveryScan>::available(std::move(result));
        }, [&] { return ck::files::finish_operation_journal_directory(*cursor); },
        "operation_recovery.directory_finish_io");
}

bool FilesOperationRecoveryOverlapReport::has_untrusted_journal() const noexcept {
    return std::any_of(conflicts.begin(), conflicts.end(), [](const auto &entry) {
        return entry.kind == FilesOperationRecoveryOverlapKind::UntrustedJournal ||
               entry.kind == FilesOperationRecoveryOverlapKind::UnqualifiedJournal;
    });
}

bool FilesOperationRecoveryOverlapReport::has_retained_source_repeat() const noexcept {
    return std::any_of(conflicts.begin(), conflicts.end(), [](const auto &entry) {
        return entry.kind == FilesOperationRecoveryOverlapKind::RetainedSourceRepeat;
    });
}

FilesOperationRecoveryOverlapReport assess_operation_recovery_overlap(
    const FilesOperationRecoveryScan &scan, const ck::files::FrozenInventory &inventory,
    std::optional<ck::files::ObjectIdentity> destination,
    std::optional<ck::files::ObjectIdentity> source_parent) {
    using namespace ck::files;
    constexpr std::size_t max_reported_conflicts = 256;
    FilesOperationRecoveryOverlapReport report;
    const auto matches_any_entry = [&](const NativeDurableIdentity &id) noexcept {
        return std::any_of(inventory.entries().begin(), inventory.entries().end(), [&](const auto &entry) noexcept {
            return id.matches_live(entry.identity);
        });
    };
    const auto matches_opt = [](const NativeDurableIdentity &id,
                                const std::optional<ObjectIdentity> &live) noexcept {
        return live.has_value() && id.matches_live(*live);
    };
    const auto add_conflict = [&](FilesOperationRecoveryOverlap conflict) {
        if (report.conflicts.size() < max_reported_conflicts)
            report.conflicts.push_back(std::move(conflict));
    };

    for (const auto &item : scan.journals) {
        if (report.conflicts.size() >= max_reported_conflicts)
            break;
        if (item.error || !item.report) {
            FilesOperationRecoveryOverlap conflict;
            conflict.kind = FilesOperationRecoveryOverlapKind::UntrustedJournal;
            conflict.journal_name = item.display_name;
            conflict.reason = item.error
                ? "unreadable or malformed journal (" + item.error->error.operation + ") cannot be excluded"
                : "operation journal report is missing and cannot be excluded";
            add_conflict(std::move(conflict));
            continue;
        }
        const auto &inspection = item.report->inspection;
        if (inspection.report.has_untrusted_tail || inspection.has_untrusted_evidence) {
            FilesOperationRecoveryOverlap conflict;
            conflict.kind = FilesOperationRecoveryOverlapKind::UntrustedJournal;
            conflict.journal_name = item.display_name;
            conflict.journal_id = item.report->journal_id;
            conflict.reason = inspection.report.has_untrusted_tail
                ? "journal has an untrusted tail and requires reconciliation"
                : "journal has untrusted native evidence and requires reconciliation";
            add_conflict(std::move(conflict));
            continue;
        }
        auto artifact = inspection.artifacts.begin();
        for (const auto &recovery : inspection.report.items) {
            if (report.conflicts.size() >= max_reported_conflicts)
                break;
            while (artifact != inspection.artifacts.end() && artifact->item < recovery.item)
                ++artifact;
            const NativeOperationRecoveryArtifact *matched_artifact =
                (artifact != inspection.artifacts.end() && artifact->item == recovery.item) ? &*artifact : nullptr;

            if (recovery.requires_reconciliation) {
                if (!matched_artifact || (!matched_artifact->evidence && !matched_artifact->deletion_evidence)) {
                    FilesOperationRecoveryOverlap conflict;
                    conflict.kind = FilesOperationRecoveryOverlapKind::UnqualifiedJournal;
                    conflict.journal_name = item.display_name;
                    conflict.journal_id = item.report->journal_id;
                    conflict.state = recovery.state;
                    conflict.action = recovery.action;
                    conflict.reason = "unfinished journal lacks decoded native identities and cannot be excluded";
                    add_conflict(std::move(conflict));
                    continue;
                }
                if (matched_artifact->deletion_evidence) {
                    const auto &del = *matched_artifact->deletion_evidence;
                    const bool overlaps =
                        matches_any_entry(del.source_identity) || matches_opt(del.source_identity, destination) ||
                        matches_opt(del.source_identity, source_parent) ||
                        matches_any_entry(del.source_parent_identity) ||
                        matches_opt(del.source_parent_identity, destination);
                    if (overlaps) {
                        FilesOperationRecoveryOverlap conflict;
                        conflict.kind = FilesOperationRecoveryOverlapKind::UnfinishedDeletion;
                        conflict.journal_name = item.display_name;
                        conflict.journal_id = item.report->journal_id;
                        conflict.state = recovery.state;
                        conflict.action = recovery.action;
                        conflict.source_name = del.source_name.display_name();
                        conflict.reason = "unfinished deletion overlaps source or destination identity";
                        add_conflict(std::move(conflict));
                    }
                    continue;
                }
                const auto &ev = *matched_artifact->evidence;
                const bool same_source = matches_any_entry(ev.source_identity);
                const bool same_destination = matches_opt(ev.destination_identity, destination);
                const bool source_hits_old_dest = matches_any_entry(ev.destination_identity);
                const bool dest_hits_old_source = matches_opt(ev.source_identity, destination);
                const bool dest_hits_old_source_parent = matches_opt(ev.source_parent_identity, destination);
                const bool same_parent_and_dest =
                    matches_opt(ev.source_parent_identity, source_parent) && same_destination;
                const bool hits_published = ev.published_identity &&
                    (matches_any_entry(*ev.published_identity) || matches_opt(*ev.published_identity, destination));
                const bool hits_staging = ev.staging_identity &&
                    (matches_any_entry(*ev.staging_identity) || matches_opt(*ev.staging_identity, destination));
                if (same_source || same_destination || source_hits_old_dest || dest_hits_old_source ||
                    dest_hits_old_source_parent || same_parent_and_dest || hits_published || hits_staging) {
                    FilesOperationRecoveryOverlap conflict;
                    conflict.kind = FilesOperationRecoveryOverlapKind::UnfinishedTransfer;
                    conflict.journal_name = item.display_name;
                    conflict.journal_id = item.report->journal_id;
                    conflict.state = recovery.state;
                    conflict.action = recovery.action;
                    if (ev.source_name)
                        conflict.source_name = ev.source_name->display_name();
                    if (ev.destination_name.is_valid())
                        conflict.destination_name = ev.destination_name.display_name();
                    if (ev.destination_address)
                        conflict.destination_address = ev.destination_address->display_text();
                    if (same_source && same_destination)
                        conflict.reason = "unfinished transfer matches both source and destination identities";
                    else if (same_parent_and_dest)
                        conflict.reason = "unfinished transfer matches source folder and destination identities";
                    else if (same_source)
                        conflict.reason = "unfinished transfer matches a selected source identity";
                    else if (same_destination)
                        conflict.reason = "unfinished transfer matches the destination folder identity";
                    else if (hits_published)
                        conflict.reason = "unfinished transfer matches a previously published object identity";
                    else if (hits_staging)
                        conflict.reason = "unfinished transfer matches an unpublished staging object identity";
                    else if (source_hits_old_dest)
                        conflict.reason = "selected source overlaps a previous transfer destination";
                    else
                        conflict.reason = "destination overlaps a previous transfer source";
                    add_conflict(std::move(conflict));
                }
                continue;
            }

            if (item.kind == FilesOperationJournalKind::Transfer &&
                recovery.state == OperationJournalState::RetainedSource && matched_artifact &&
                matched_artifact->evidence) {
                const auto &ev = *matched_artifact->evidence;
                const bool same_source = matches_any_entry(ev.source_identity);
                const bool same_destination = matches_opt(ev.destination_identity, destination);
                const bool reverse_published = ev.published_identity && matches_any_entry(*ev.published_identity) &&
                    matches_opt(ev.source_parent_identity, destination);
                if ((same_source && same_destination) || reverse_published) {
                    FilesOperationRecoveryOverlap conflict;
                    conflict.kind = FilesOperationRecoveryOverlapKind::RetainedSourceRepeat;
                    conflict.journal_name = item.display_name;
                    conflict.journal_id = item.report->journal_id;
                    conflict.state = recovery.state;
                    conflict.action = recovery.action;
                    if (ev.source_name)
                        conflict.source_name = ev.source_name->display_name();
                    if (ev.destination_name.is_valid())
                        conflict.destination_name = ev.destination_name.display_name();
                    if (ev.destination_address)
                        conflict.destination_address = ev.destination_address->display_text();
                    conflict.reason = reverse_published
                        ? "previous move already committed this object from the destination folder and retained its source"
                        : "previous move already committed the destination and retained the source; repeat requires reconciliation";
                    add_conflict(std::move(conflict));
                }
            }
        }
    }
    return report;
}

ck::files::Observation<FilesOperationRecoveryOverlapReport> check_operation_recovery_overlap(
    const ck::files::NativeAbsolutePath &private_root, const ck::files::FrozenInventory &inventory,
    std::optional<ck::files::ObjectIdentity> destination,
    std::optional<ck::files::ObjectIdentity> source_parent, FilesOperationRecoveryLimits limits) {
    auto scan = discover_operation_recovery(private_root, limits);
    if (!scan.value())
        return ck::files::Observation<FilesOperationRecoveryOverlapReport>::unavailable(
            scan.state(), scan.unavailable_detail()->error);
    return ck::files::Observation<FilesOperationRecoveryOverlapReport>::available(
        assess_operation_recovery_overlap(*scan.value(), inventory, std::move(destination),
                                          std::move(source_parent)));
}

struct FilesOperationRecoveryService::State final {
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::pair<ck::files::NativeAbsolutePath, FilesOperationRecoveryLimits>> pending;
    std::optional<FilesOperationRecoveryReply> reply;
    bool stopping = false, busy = false, signalled = false;
    std::thread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)), worker([this] { run(); }) {}
    ~State() {
        request_stop();
        join();
    }
    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        condition.notify_all();
    }
    void join() {
        if (worker.joinable()) worker.join();
    }
    void signal() {
        if (signalled || !wake) return;
        signalled = true;
        try { wake(); } catch (...) {}
    }
    void emit(FilesOperationRecoveryReply value) {
        std::lock_guard lock(mutex);
        if (stopping) return;
        reply = std::move(value);
        busy = false;
        signal();
    }
    void run_one(ck::files::NativeAbsolutePath root, FilesOperationRecoveryLimits limits) {
        try {
            auto scan = discover_operation_recovery(root, limits);
            FilesOperationRecoveryReply result;
            if (scan.value()) {
                result.state = FilesOperationRecoveryState::Ready;
                result.scan = std::move(scan).take_value();
            } else {
                result.state = FilesOperationRecoveryState::Failed;
                result.error = *scan.unavailable_detail();
            }
            emit(std::move(result));
        } catch (...) {
            FilesOperationRecoveryReply result;
            result.state = FilesOperationRecoveryState::WorkerFault;
            // Keep the fallback allocation-free: formatting a NativeError
            // here can throw again under persistent worker allocation failure.
            // WorkerFault with no error already has an explicit UI diagnostic.
            emit(std::move(result));
        }
    }
    void run() {
        for (;;) {
            std::optional<std::pair<ck::files::NativeAbsolutePath, FilesOperationRecoveryLimits>> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) { pending.reset(); return; }
                job = std::move(*pending);
                pending.reset();
                busy = true;
                signalled = false;
            }
            run_one(std::move(job->first), std::move(job->second));
        }
    }
};

FilesOperationRecoveryService::FilesOperationRecoveryService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesOperationRecoveryService::~FilesOperationRecoveryService() = default;
FilesOperationRecoverySubmission FilesOperationRecoveryService::request(
    ck::files::NativeAbsolutePath private_root, FilesOperationRecoveryLimits limits) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return FilesOperationRecoverySubmission::Stopped;
    if (!private_root.is_valid() || !valid_limits(limits)) return FilesOperationRecoverySubmission::Invalid;
    if (state_->busy || state_->pending) return FilesOperationRecoverySubmission::Busy;
    state_->pending.emplace(std::move(private_root), limits);
    state_->condition.notify_one();
    return FilesOperationRecoverySubmission::Accepted;
}
std::optional<FilesOperationRecoveryReply> FilesOperationRecoveryService::take() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->reply);
    state_->reply.reset();
    state_->signalled = false;
    return result;
}
void FilesOperationRecoveryService::request_stop() noexcept { state_->request_stop(); }
void FilesOperationRecoveryService::join() { state_->join(); }
bool FilesOperationRecoveryService::running() const noexcept {
    std::lock_guard lock(state_->mutex);
    return !state_->stopping;
}
bool FilesOperationRecoveryService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

} // namespace ck::vision
