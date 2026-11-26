// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery.hpp"
#include "directory_cursor.hpp"
#include "files_recovery_finalization.hpp"
#include "held_child.hpp"
#include "native_filesystem.hpp"
#include "native_operation_copy_job.hpp"
#include "native_operation_journal_id.hpp"
#include "native_operation_move_job.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <limits>
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

std::optional<std::vector<std::byte>> read_bounded_journal_snapshot(
    ck::files::OperationJournalFile &file, std::size_t max_bytes) {
    const auto sz = file.size();
    if (!sz.value() || *sz.value() > max_bytes)
        return std::nullopt;
    std::vector<std::byte> bytes(static_cast<std::size_t>(*sz.value()));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t chunk = std::min<std::size_t>(bytes.size() - offset, 64U * 1024U);
        const auto got = file.read_at(offset, std::span<std::byte>(bytes.data() + offset, chunk));
        if (!got.value() || *got.value() == 0 || *got.value() > chunk)
            return std::nullopt;
        offset += *got.value();
    }
    return bytes;
}

std::optional<ck::files::NativeName> leaf_name_of(const ck::files::FrozenInventoryEntry &entry) {
    if (!entry.relative_components.empty() && entry.relative_components.back().is_valid())
        return entry.relative_components.back();
    if (const auto *bytes = entry.source_path.posix_bytes()) {
        const auto slash = bytes->find_last_of('/');
        if (slash != std::string::npos && slash + 1 < bytes->size())
            return ck::files::NativeName::from_posix(bytes->substr(slash + 1));
    }
    if (const auto *units = entry.source_path.windows_units()) {
        const auto sep = units->find_last_of(u"\\/");
        if (sep != std::u16string::npos && sep + 1 < units->size())
            return ck::files::NativeName::from_windows(units->substr(sep + 1));
    }
    return std::nullopt;
}

std::optional<ck::files::TransferRowReview> find_transfer_row_review(
    const FilesOperationRecoveryItem &item, std::size_t item_index) noexcept {
    if (!item.reconciliation)
        return std::nullopt;
    for (const auto &row : item.reconciliation->items)
        if (row.item == item_index)
            return row.review;
    return std::nullopt;
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
            std::vector<std::vector<std::byte>> transfer_snapshots;
            std::vector<std::size_t> transfer_indices;
            std::size_t total_snapshot_bytes = 0;
            std::optional<ck::files::UnavailableObservation> snapshot_failure;
            constexpr std::size_t kMaxReconciliationTotalBytes =
                std::min<std::size_t>(64U * 1024U * 1024U, ck::files::OperationJournalCodecLimits::hard_max_bytes);
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
                    std::optional<std::vector<std::byte>> snapshot_bytes;
                    bool snapshot_budget_exceeded = false;
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
                                auto inspected = ck::files::inspect_native_operation_recovery_evidence(
                                    **file.value(), limits.journal, limits.codec, limits.evidence);
                                if (!snapshot_failure) {
                                    if (total_snapshot_bytes < kMaxReconciliationTotalBytes) {
                                        const std::size_t budget = std::min(
                                            limits.codec.max_bytes, kMaxReconciliationTotalBytes - total_snapshot_bytes);
                                        snapshot_bytes = read_bounded_journal_snapshot(**file.value(), budget);
                                    } else {
                                        snapshot_budget_exceeded = true;
                                    }
                                }
                                return inspected;
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
                    const bool valid_transfer_without_snapshot =
                        *kind == FilesOperationJournalKind::Transfer && item.report && !snapshot_bytes;
                    if (valid_transfer_without_snapshot && !snapshot_failure) {
                        snapshot_failure = ck::files::UnavailableObservation{
                            ck::files::ObservationState::Unknown,
                            {ck::files::ErrorDomain::Adapter,
                             static_cast<std::int64_t>(snapshot_budget_exceeded
                                 ? ck::files::AdapterError::InventoryLimit
                                 : ck::files::AdapterError::InterruptedRead),
                             snapshot_budget_exceeded
                                 ? "operation_recovery.reconciliation_snapshot_budget"
                                 : "operation_recovery.reconciliation_snapshot_read"}};
                    }
                    const std::size_t item_idx = result.journals.size();
                    result.journals.push_back(std::move(item));
                    if (*kind == FilesOperationJournalKind::Transfer) {
                        if (snapshot_bytes) {
                            total_snapshot_bytes += snapshot_bytes->size();
                            transfer_snapshots.push_back(std::move(*snapshot_bytes));
                        } else {
                            transfer_snapshots.emplace_back();
                        }
                        transfer_indices.push_back(item_idx);
                    }
                }
                if (batch.state == ck::files::DirectoryReadState::End)
                    break;
                if (batch.state != ck::files::DirectoryReadState::More)
                    return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InterruptedRead,
                        "operation_recovery.enumerate", ck::files::ObservationState::Unknown);
            }
            if (snapshot_failure) {
                result.reconciliation_error = std::move(snapshot_failure);
                result.requires_reconciliation = true;
            } else {
                ck::files::TransferReconciliationLimits reconciliation_limits;
                reconciliation_limits.max_journals =
                    std::min<std::size_t>(std::max<std::size_t>(limits.max_journals, 1), 16'384U);
                reconciliation_limits.max_total_bytes = kMaxReconciliationTotalBytes;
                reconciliation_limits.max_records_per_journal =
                    std::min<std::size_t>(std::max<std::size_t>(limits.journal.max_records, 1), 4096U);
                std::vector<std::span<const std::byte>> views;
                views.reserve(transfer_snapshots.size());
                for (const auto &buf : transfer_snapshots)
                    views.emplace_back(buf.data(), buf.size());
                auto catalog = ck::files::inspect_transfer_journals(views, reconciliation_limits);
                if (catalog.value()) {
                    if (catalog.value()->journals.size() == transfer_indices.size()) {
                        for (std::size_t k = 0; k < transfer_indices.size(); ++k)
                            result.journals[transfer_indices[k]].reconciliation = catalog.value()->journals[k];
                    }
                    result.reconciliation_catalog = std::move(catalog).take_value();
                } else {
                    result.reconciliation_error = catalog.unavailable_detail()
                        ? *catalog.unavailable_detail()
                        : ck::files::UnavailableObservation{
                              ck::files::ObservationState::Unknown,
                              {ck::files::ErrorDomain::Adapter,
                               static_cast<std::int64_t>(ck::files::AdapterError::ProviderReviewRequired),
                               "operation_recovery.reconciliation_catalog_failed"}};
                    result.requires_reconciliation = true;
                }
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
    struct ConflictOrigin {
        std::optional<std::size_t> catalog_index;
        std::optional<std::size_t> item_index;
    };
    std::vector<ConflictOrigin> origins;
    const auto matches_any_entry = [&](const NativeDurableIdentity &id) noexcept {
        return std::any_of(inventory.entries().begin(), inventory.entries().end(), [&](const auto &entry) noexcept {
            return id.matches_live(entry.identity);
        });
    };
    const auto matches_opt = [](const NativeDurableIdentity &id,
                                const std::optional<ObjectIdentity> &live) noexcept {
        return live.has_value() && id.matches_live(*live);
    };
    const auto add_conflict = [&](FilesOperationRecoveryOverlap conflict, ConflictOrigin origin = {}) {
        if (report.conflicts.size() < max_reported_conflicts) {
            report.conflicts.push_back(std::move(conflict));
            origins.push_back(origin);
        }
    };

    std::size_t transfer_catalog_index = 0;
    for (const auto &item : scan.journals) {
        std::optional<std::size_t> current_catalog_index;
        if (item.kind == FilesOperationJournalKind::Transfer) {
            current_catalog_index = transfer_catalog_index++;
        }
        if (report.conflicts.size() >= max_reported_conflicts)
            break;
        if (item.error || !item.report) {
            FilesOperationRecoveryOverlap conflict;
            conflict.kind = FilesOperationRecoveryOverlapKind::UntrustedJournal;
            conflict.journal_name = item.display_name;
            conflict.reason = item.error
                ? "unreadable or malformed journal (" + item.error->error.operation + ") cannot be excluded"
                : "operation journal report is missing and cannot be excluded";
            add_conflict(std::move(conflict), {current_catalog_index, std::nullopt});
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
            add_conflict(std::move(conflict), {current_catalog_index, std::nullopt});
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
                    add_conflict(std::move(conflict), {current_catalog_index, recovery.item});
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
                        add_conflict(std::move(conflict), {current_catalog_index, recovery.item});
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
                    conflict.row_review = find_transfer_row_review(item, recovery.item);
                    if (item.reconciliation)
                        conflict.lineage = item.reconciliation->lineage;
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
                    add_conflict(std::move(conflict), {current_catalog_index, recovery.item});
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
                    conflict.row_review = find_transfer_row_review(item, recovery.item);
                    if (item.reconciliation)
                        conflict.lineage = item.reconciliation->lineage;
                    conflict.reason = reverse_published
                        ? "previous move already committed this object from the destination folder and retained its source"
                        : "previous move already committed the destination and retained the source; repeat requires reconciliation";
                    add_conflict(std::move(conflict), {current_catalog_index, recovery.item});
                }
            }
        }
    }
    if (scan.reconciliation_error) {
        FilesOperationRecoveryOverlap conflict;
        conflict.kind = FilesOperationRecoveryOverlapKind::UntrustedJournal;
        conflict.reason = "transfer reconciliation catalog failed (" + scan.reconciliation_error->error.operation +
            ") and cannot exclude overlapping journals";
        add_conflict(std::move(conflict));
    }
    if (scan.reconciliation_catalog && !scan.reconciliation_catalog->journals.empty()) {
        const auto durable_src_parent = source_parent ? NativeDurableIdentity::from_live(*source_parent) : std::nullopt;
        const auto durable_dst_parent = destination ? NativeDurableIdentity::from_live(*destination) : std::nullopt;
        for (const auto &entry : inventory.entries()) {
            TransferRequestReviewInput input;
            input.kind = TransferRequestKind::SubmitAgain;
            input.source_object = NativeDurableIdentity::from_live(entry.identity);
            // source_parent and destination are only direct parents for top-level
            // entries; nested tree entries must not fabricate top-level addresses.
            if (entry.relative_components.size() <= 1) {
                const auto leaf = leaf_name_of(entry);
                if (durable_src_parent && leaf && leaf->is_valid())
                    input.source = TransferEntryAddress{*durable_src_parent, *leaf};
                if (durable_dst_parent && leaf && leaf->is_valid())
                    input.destination = TransferEntryAddress{*durable_dst_parent, *leaf};
            }
            if (!input.source_object && !input.source && !input.destination)
                continue;
            const auto reviewed = ck::files::review_transfer_request(*scan.reconciliation_catalog, input);
            if (!reviewed.value())
                continue;
            for (const auto &match : reviewed.value()->matches) {
                if (match.journal_index >= scan.reconciliation_catalog->journals.size())
                    continue;
                for (std::size_t idx = 0; idx < report.conflicts.size(); ++idx) {
                    if (report.conflicts[idx].disposition)
                        continue;
                    if (origins[idx].catalog_index != match.journal_index)
                        continue;
                    if (origins[idx].item_index && *origins[idx].item_index != match.item)
                        continue;
                    report.conflicts[idx].disposition = reviewed.value()->disposition;
                }
            }
        }
    }
    return report;
}

ck::files::Observation<ck::files::TransferRequestReview> review_operation_transfer_request(
    const FilesOperationRecoveryScan &scan, const ck::files::TransferRequestReviewInput &input,
    std::stop_token stop) {
    if (!scan.reconciliation_catalog) {
        if (scan.reconciliation_error)
            return ck::files::Observation<ck::files::TransferRequestReview>::unavailable(
                scan.reconciliation_error->state, scan.reconciliation_error->error);
        return error<ck::files::TransferRequestReview>(
            ck::files::AdapterError::ProviderReviewRequired,
            "operation_recovery.reconciliation_catalog_missing",
            ck::files::ObservationState::Unknown);
    }
    return ck::files::review_transfer_request(*scan.reconciliation_catalog, input, stop);
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

namespace {

std::optional<ck::files::NativeName> recovery_generated_name(
    ck::files::NameEncoding encoding, std::string_view prefix,
    const ck::files::OperationJournalId &id, std::size_t suffix_num) {
    std::string text(prefix);
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < 6 && i < id.bytes.size(); ++i) {
        const auto value = std::to_integer<unsigned char>(id.bytes[i]);
        text.push_back(hex[value >> 4U]);
        text.push_back(hex[value & 0x0FU]);
    }
    text.push_back('-');
    text += std::to_string(suffix_num);
    if (encoding == ck::files::NameEncoding::PosixBytes)
        return ck::files::NativeName::from_posix(std::move(text));
    if (encoding == ck::files::NameEncoding::WindowsUtf16) {
        std::u16string units;
        units.reserve(text.size());
        for (const auto ch : text)
            units.push_back(static_cast<char16_t>(static_cast<unsigned char>(ch)));
        return ck::files::NativeName::from_windows(std::move(units));
    }
    return std::nullopt;
}

std::optional<ck::files::NativeName> recovery_backup_name(
    const ck::files::NativeName &base, const ck::files::OperationJournalId &id, std::size_t attempt = 0) {
    constexpr char hex[] = "0123456789abcdef";
    std::string suffix = ".ck-replaced-";
    for (std::size_t i = 0; i < 4 && i < id.bytes.size(); ++i) {
        const auto value = std::to_integer<unsigned char>(id.bytes[i]);
        suffix.push_back(hex[value >> 4U]);
        suffix.push_back(hex[value & 0x0FU]);
    }
    if (attempt > 0)
        suffix += "-" + std::to_string(attempt);
    if (const auto *bytes = base.posix_bytes())
        return ck::files::NativeName::from_posix(*bytes + suffix);
    if (const auto *units = base.windows_units()) {
        std::u16string combined = *units;
        for (const auto ch : suffix)
            combined.push_back(static_cast<char16_t>(static_cast<unsigned char>(ch)));
        return ck::files::NativeName::from_windows(std::move(combined));
    }
    return std::nullopt;
}

ck::files::Observation<std::unique_ptr<ck::files::DirectoryCursor>> open_native_dir(
    ck::files::NativeFileSystem &fs, const ck::files::NativeAbsolutePath &path) {
    auto host = path.host_path();
    if (!host.value())
        return ck::files::Observation<std::unique_ptr<ck::files::DirectoryCursor>>::unavailable(
            host.state(), host.unavailable_detail()->error);
    return fs.open_directory(*host.value());
}

ck::files::Observation<std::shared_ptr<ck::files::DirectoryCursor>> find_matching_directory(
    ck::files::NativeFileSystem &fs,
    const std::vector<ck::files::NativeAbsolutePath> &candidates,
    const ck::files::NativeDurableIdentity &expected_identity) {
    using Result = ck::files::Observation<std::shared_ptr<ck::files::DirectoryCursor>>;
    for (const auto &path : candidates) {
        if (!path.is_valid())
            continue;
        auto opened = open_native_dir(fs, path);
        if (!opened.value())
            continue;
        const auto live_id = (*opened.value())->identity();
        if (live_id.value() && expected_identity.matches_live(*live_id.value()))
            return Result::available(std::shared_ptr<ck::files::DirectoryCursor>(std::move(opened).take_value()));
    }
    return Result::unavailable(ck::files::ObservationState::Gone,
        {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(ck::files::AdapterError::UnqualifiedIdentity),
         "operation_recovery.source_parent_not_open"});
}

} // namespace

ck::files::Observation<FilesOperationRecoveryActionResult> execute_operation_recovery_action(
    const FilesOperationRecoveryActionRequest &request, std::stop_token stop) {
    using namespace ck::files;
    using Result = Observation<FilesOperationRecoveryActionResult>;
    if (!request.private_root.is_valid() || !request.journal_name.is_valid() || !valid_limits(request.limits))
        return error<FilesOperationRecoveryActionResult>(AdapterError::InvalidLimit, "operation_recovery.action_input");
    if (stop.stop_requested())
        return error<FilesOperationRecoveryActionResult>(AdapterError::InterruptedRead,
            "operation_recovery.action_cancelled", ObservationState::Unknown);

    auto opened_dir = open_native_operation_journal_directory(request.private_root);
    if (!opened_dir.value())
        return Result::unavailable(opened_dir.state(), opened_dir.unavailable_detail()->error);
    auto journal_dir = std::move(opened_dir).take_value();

    return detail::finish_recovery_observation<FilesOperationRecoveryActionResult>(
        [&]() -> Result {
            // Capture the journal file's live identity from the held journal directory
            // so we can retire the journal via identity-bound retire_child after success.
            auto journal_entry_obs = journal_dir->open_child(request.journal_name, EntryAccess::MetadataOnly);
            if (!journal_entry_obs.value())
                return Result::unavailable(journal_entry_obs.state(), journal_entry_obs.unavailable_detail()->error);
            auto journal_entry = std::move(journal_entry_obs).take_value();
            const auto journal_meta = journal_entry->inspect_basic();
            if (!journal_meta.value() || !journal_meta.value()->identity.value())
                return error<FilesOperationRecoveryActionResult>(AdapterError::UnqualifiedIdentity,
                    "operation_recovery.journal_identity", ObservationState::Unknown);
            const ObjectIdentity journal_file_id = *journal_meta.value()->identity.value();
            journal_entry.reset();

            auto journal_file_obs = open_held_operation_journal_file(*journal_dir, request.journal_name);
            if (!journal_file_obs.value())
                return Result::unavailable(journal_file_obs.state(), journal_file_obs.unavailable_detail()->error);
            auto journal_file = std::move(journal_file_obs).take_value();

            const bool is_retention = request.kind == FilesOperationRecoveryActionKind::RestoreRetainedTarget ||
                                      request.kind == FilesOperationRecoveryActionKind::DisposeRetainedTarget;
            std::optional<FileOperation> recorded_operation;
            auto inspected = detail::finish_recovery_observation<NativeOperationRecoveryInspection>(
                [&] {
                    if (is_retention)
                        return inspect_native_retention_recovery(
                            *journal_file, request.limits.journal,
                            request.limits.codec, request.limits.evidence);
                    auto loaded = load_operation_journal(
                        *journal_file, request.limits.journal, request.limits.codec);
                    if (loaded.value() && loaded.value()->journal)
                        recorded_operation = loaded.value()->journal->operation();
                    return inspect_native_operation_recovery_evidence(
                        *journal_file, request.limits.journal,
                        request.limits.codec, request.limits.evidence);
                },
                [&] { return journal_file->finish_io(); }, "operation_recovery.action_finish_io");
            journal_file.reset();
            if (!inspected.value())
                return Result::unavailable(inspected.state(), inspected.unavailable_detail()->error);

            const auto &inspection = *inspected.value();
            NativeFileSystem fs;
            FilesOperationRecoveryActionResult out;
            out.kind = request.kind;

            if (is_retention) {
                const NativeOperationRecoveryEvidence *evidence = nullptr;
                for (const auto &artifact : inspection.artifacts) {
                    if (artifact.evidence) {
                        evidence = &*artifact.evidence;
                        break;
                    }
                }
                if (!evidence || !evidence->destination_address || !evidence->destination_address->is_valid())
                    return error<FilesOperationRecoveryActionResult>(AdapterError::UnqualifiedIdentity,
                        "operation_recovery.retention_evidence_missing", ObservationState::Unknown);
                auto dest_opened = open_native_dir(fs, *evidence->destination_address);
                if (!dest_opened.value())
                    return Result::unavailable(dest_opened.state(), dest_opened.unavailable_detail()->error);
                auto dest_dir = std::move(dest_opened).take_value();
                const auto dest_id = dest_dir->identity();
                if (!dest_id.value() || !evidence->destination_identity.matches_live(*dest_id.value()))
                    return error<FilesOperationRecoveryActionResult>(AdapterError::MetadataChanged,
                        "operation_recovery.retention_parent_changed", ObservationState::Changed);

                const auto expected_retained_id = evidence->published_identity
                    ? evidence->published_identity : evidence->staging_identity;
                if (!expected_retained_id)
                    return error<FilesOperationRecoveryActionResult>(AdapterError::UnqualifiedIdentity,
                        "operation_recovery.retention_object_identity", ObservationState::Unknown);

                out.items_processed = 1;
                if (request.kind == FilesOperationRecoveryActionKind::DisposeRetainedTarget) {
                    auto retained_open = dest_dir->open_child(evidence->destination_name, EntryAccess::MetadataOnly);
                    if (retained_open.value()) {
                        auto retained_entry = std::move(retained_open).take_value();
                        const auto meta = retained_entry->inspect_basic();
                        if (!meta.value() || !meta.value()->identity.value() ||
                            !expected_retained_id->matches_live(*meta.value()->identity.value()))
                            return error<FilesOperationRecoveryActionResult>(AdapterError::MetadataChanged,
                                "operation_recovery.retained_object_changed", ObservationState::Changed);
                        const ObjectIdentity live_id = *meta.value()->identity.value();
                        retained_entry.reset();
                        auto retired = dest_dir->retire_child(evidence->destination_name, live_id);
                        if (!retired.value())
                            return Result::unavailable(retired.state(), retired.unavailable_detail()->error);
                        out.summary = "Disposed retained target version " + evidence->destination_name.display_name();
                    } else if (retained_open.state() == ObservationState::Gone) {
                        out.summary = "Retained target version was already removed; cleared recovery record";
                    } else {
                        return Result::unavailable(retained_open.state(), retained_open.unavailable_detail()->error);
                    }
                    out.items_completed = 1;
                } else {
                    // RestoreRetainedTarget
                    const NativeName original_name = evidence->source_name.value_or(evidence->staging_name);
                    if (!original_name.is_valid())
                        return error<FilesOperationRecoveryActionResult>(AdapterError::InvalidIdentifier,
                            "operation_recovery.restore_original_name");
                    auto retained_open = dest_dir->open_child(evidence->destination_name, EntryAccess::MetadataOnly);
                    if (!retained_open.value())
                        return Result::unavailable(retained_open.state(), retained_open.unavailable_detail()->error);
                    auto retained_entry = std::move(retained_open).take_value();
                    const auto meta = retained_entry->inspect_basic();
                    if (!meta.value() || !meta.value()->identity.value() ||
                        !expected_retained_id->matches_live(*meta.value()->identity.value()))
                        return error<FilesOperationRecoveryActionResult>(AdapterError::MetadataChanged,
                            "operation_recovery.retained_object_changed", ObservationState::Changed);
                    const ObjectIdentity retained_live_id = *meta.value()->identity.value();
                    retained_entry.reset();

                    // If the original target name is currently occupied, park it aside first
                    // using rename_child_noreplace so no user data is overwritten.
                    auto current_open = dest_dir->open_child(original_name, EntryAccess::MetadataOnly);
                    if (current_open.value()) {
                        auto current_entry = std::move(current_open).take_value();
                        const auto current_meta = current_entry->inspect_basic();
                        if (!current_meta.value() || !current_meta.value()->identity.value())
                            return error<FilesOperationRecoveryActionResult>(AdapterError::UnqualifiedIdentity,
                                "operation_recovery.restore_current_identity", ObservationState::Unknown);
                        const ObjectIdentity current_id = *current_meta.value()->identity.value();
                        current_entry.reset();
                        bool parked_ok = false;
                        std::optional<UnavailableObservation> last_park_err;
                        for (std::size_t attempt = 0; attempt < 64; ++attempt) {
                            const auto backup = recovery_backup_name(original_name, inspection.journal_id, attempt);
                            if (!backup)
                                return error<FilesOperationRecoveryActionResult>(AdapterError::UnrepresentableName,
                                    "operation_recovery.restore_backup_name");
                            auto parked = dest_dir->rename_child_noreplace(original_name, current_id, *dest_dir, *backup);
                            if (parked.value()) {
                                parked_ok = true;
                                break;
                            }
                            last_park_err = *parked.unavailable_detail();
                            if (last_park_err->state != ObservationState::Changed &&
                                last_park_err->error.code != static_cast<std::int64_t>(AdapterError::PlanDecisionStale))
                                break;
                        }
                        if (!parked_ok)
                            return Result::unavailable(last_park_err->state, last_park_err->error);
                    } else if (current_open.state() != ObservationState::Gone) {
                        return Result::unavailable(current_open.state(), current_open.unavailable_detail()->error);
                    }

                    auto restored = dest_dir->rename_child_noreplace(
                        evidence->destination_name, retained_live_id, *dest_dir, original_name);
                    if (!restored.value())
                        return Result::unavailable(restored.state(), restored.unavailable_detail()->error);
                    out.items_completed = 1;
                    out.summary = "Restored retained version to " + original_name.display_name();
                }

                auto retired_journal = journal_dir->retire_child(request.journal_name, journal_file_id);
                if (!retired_journal.value())
                    return Result::unavailable(retired_journal.state(), retired_journal.unavailable_detail()->error);
                out.journal_retired = true;
                return Result::available(std::move(out));
            }

            // ResumeRecordedJob / RetryUnresolvedItems (B-03)
            const FileOperation op = recorded_operation.value_or(FileOperation::Copy);
            if (op != FileOperation::Copy && op != FileOperation::Move)
                return error<FilesOperationRecoveryActionResult>(AdapterError::UnsupportedQuery,
                    "operation_recovery.unsupported_operation", ObservationState::Unsupported);

            for (const auto &item_report : inspection.report.items) {
                if (stop.stop_requested())
                    return error<FilesOperationRecoveryActionResult>(AdapterError::InterruptedRead,
                        "operation_recovery.action_cancelled", ObservationState::Unknown);
                if (item_report.state == OperationJournalState::Complete ||
                    item_report.state == OperationJournalState::Cancelled)
                    continue;

                const NativeOperationRecoveryEvidence *evidence = nullptr;
                for (const auto &artifact : inspection.artifacts) {
                    if (artifact.item == item_report.item && artifact.evidence) {
                        evidence = &*artifact.evidence;
                        break;
                    }
                }
                if (!evidence || !evidence->destination_address || !evidence->destination_address->is_valid())
                    return error<FilesOperationRecoveryActionResult>(AdapterError::UnqualifiedIdentity,
                        "operation_recovery.item_evidence_missing", ObservationState::Unknown);

                ++out.items_processed;

                auto dest_opened = open_native_dir(fs, *evidence->destination_address);
                if (!dest_opened.value())
                    return Result::unavailable(dest_opened.state(), dest_opened.unavailable_detail()->error);
                auto dest_dir = std::shared_ptr<DirectoryCursor>(std::move(dest_opened).take_value());
                const auto dest_id = dest_dir->identity();
                if (!dest_id.value() || !evidence->destination_identity.matches_live(*dest_id.value()))
                    return error<FilesOperationRecoveryActionResult>(AdapterError::MetadataChanged,
                        "operation_recovery.destination_parent_changed", ObservationState::Changed);

                // Clean up any verified unpublished staging file left behind by the interrupted attempt.
                if (evidence->staging_name.is_valid() && evidence->staging_identity &&
                    item_report.state != OperationJournalState::DestinationCommitted &&
                    item_report.state != OperationJournalState::RetainedSource &&
                    item_report.state != OperationJournalState::SourceRemovalIntended) {
                    auto stage_open = dest_dir->open_child(evidence->staging_name, EntryAccess::MetadataOnly);
                    if (stage_open.value()) {
                        auto stage_entry = std::move(stage_open).take_value();
                        const auto smeta = stage_entry->inspect_basic();
                        if (smeta.value() && smeta.value()->identity.value() &&
                            evidence->staging_identity->matches_live(*smeta.value()->identity.value())) {
                            const ObjectIdentity stage_id = *smeta.value()->identity.value();
                            stage_entry.reset();
                            auto cleaned = dest_dir->retire_child(evidence->staging_name, stage_id);
                            if (!cleaned.value())
                                return Result::unavailable(cleaned.state(), cleaned.unavailable_detail()->error);
                        }
                    }
                }

                // If destination is already committed for a Copy, verify live destination identity and complete.
                if (op == FileOperation::Copy &&
                    item_report.state == OperationJournalState::DestinationCommitted) {
                    auto dest_entry_obs = dest_dir->open_child(evidence->destination_name, EntryAccess::MetadataOnly);
                    if (!dest_entry_obs.value())
                        return Result::unavailable(dest_entry_obs.state(), dest_entry_obs.unavailable_detail()->error);
                    auto dest_entry = std::move(dest_entry_obs).take_value();
                    if (evidence->published_identity) {
                        const auto dmeta = dest_entry->inspect_basic();
                        if (!dmeta.value() || !dmeta.value()->identity.value() ||
                            !evidence->published_identity->matches_live(*dmeta.value()->identity.value()))
                            return error<FilesOperationRecoveryActionResult>(AdapterError::MetadataChanged,
                                "operation_recovery.committed_destination_changed", ObservationState::Changed);
                    }
                    ++out.items_completed;
                    continue;
                }

                // If destination is already committed for a Move, verify destination and retire source if stable.
                if (op == FileOperation::Move &&
                    (item_report.state == OperationJournalState::DestinationCommitted ||
                     item_report.state == OperationJournalState::RetainedSource ||
                     item_report.state == OperationJournalState::SourceRemovalIntended)) {
                    auto dest_entry_obs = dest_dir->open_child(evidence->destination_name, EntryAccess::MetadataOnly);
                    if (!dest_entry_obs.value())
                        return Result::unavailable(dest_entry_obs.state(), dest_entry_obs.unavailable_detail()->error);
                    auto dest_entry = std::move(dest_entry_obs).take_value();
                    if (evidence->published_identity) {
                        const auto dmeta = dest_entry->inspect_basic();
                        if (!dmeta.value() || !dmeta.value()->identity.value() ||
                            !evidence->published_identity->matches_live(*dmeta.value()->identity.value()))
                            return error<FilesOperationRecoveryActionResult>(AdapterError::MetadataChanged,
                                "operation_recovery.committed_destination_changed", ObservationState::Changed);
                    }
                    dest_entry.reset();
                    if (!evidence->source_name)
                        return error<FilesOperationRecoveryActionResult>(AdapterError::UnqualifiedIdentity,
                            "operation_recovery.source_evidence_missing", ObservationState::Unknown);
                    auto src_dir_obs = find_matching_directory(fs, request.candidate_source_directories, evidence->source_parent_identity);
                    if (!src_dir_obs.value())
                        return Result::unavailable(src_dir_obs.state(), src_dir_obs.unavailable_detail()->error);
                    auto src_dir = std::move(src_dir_obs).take_value();
                    auto src_entry_obs = src_dir->open_child(*evidence->source_name, EntryAccess::ContentRead | EntryAccess::MoveSource);
                    if (!src_entry_obs.value()) {
                        if (src_entry_obs.state() == ObservationState::Gone) {
                            ++out.items_completed;
                            continue;
                        }
                        return Result::unavailable(src_entry_obs.state(), src_entry_obs.unavailable_detail()->error);
                    }
                    auto src_entry = std::move(src_entry_obs).take_value();
                    const auto smeta = src_entry->inspect_basic();
                    if (!smeta.value() || !smeta.value()->identity.value() ||
                        !evidence->source_identity.matches_live(*smeta.value()->identity.value()))
                        return error<FilesOperationRecoveryActionResult>(AdapterError::MetadataChanged,
                            "operation_recovery.source_changed", ObservationState::Changed);
                    const ObjectIdentity src_id = *smeta.value()->identity.value();
                    if (evidence->content_digest) {
                        Sha256 hash;
                        std::uint64_t offset = 0;
                        for (;;) {
                            if (stop.stop_requested())
                                return error<FilesOperationRecoveryActionResult>(AdapterError::InterruptedRead,
                                    "operation_recovery.action_cancelled", ObservationState::Unknown);
                            auto chunk = src_entry->read_content({}, offset, stop);
                            if (!chunk.value())
                                return Result::unavailable(chunk.state(), chunk.unavailable_detail()->error);
                            if (!chunk.value()->bytes.empty()) {
                                hash.update(chunk.value()->bytes);
                                offset += chunk.value()->bytes.size();
                            }
                            if (chunk.value()->complete) break;
                            if (chunk.value()->bytes.empty()) {
                                ++out.items_retained_source;
                                offset = std::numeric_limits<std::uint64_t>::max();
                                break;
                            }
                        }
                        if (offset == std::numeric_limits<std::uint64_t>::max())
                            continue;
                        if (hash.digest() != *evidence->content_digest) {
                            ++out.items_retained_source;
                            continue;
                        }
                    }
                    const auto stable = src_entry->qualify_stable_content();
                    if (!stable.value() || !*stable.value()) {
                        ++out.items_retained_source;
                        continue;
                    }
                    src_entry.reset();
                    auto retired = src_dir->retire_child(*evidence->source_name, src_id);
                    if (!retired.value())
                        return Result::unavailable(retired.state(), retired.unavailable_detail()->error);
                    ++out.items_completed;
                    continue;
                }

                // Re-execute the unfinished Copy or Move item with a fresh linked journal ID.
                if (!evidence->source_name)
                    return error<FilesOperationRecoveryActionResult>(AdapterError::UnqualifiedIdentity,
                        "operation_recovery.source_evidence_missing", ObservationState::Unknown);
                auto src_dir_obs = find_matching_directory(fs, request.candidate_source_directories, evidence->source_parent_identity);
                if (!src_dir_obs.value())
                    return Result::unavailable(src_dir_obs.state(), src_dir_obs.unavailable_detail()->error);
                auto src_dir = std::move(src_dir_obs).take_value();
                auto held_source = HeldChild::observe(src_dir, *evidence->source_name);
                if (!held_source.value())
                    return Result::unavailable(held_source.state(), held_source.unavailable_detail()->error);
                if (!(*held_source.value())->metadata().identity.value() ||
                    !evidence->source_identity.matches_live(*(*held_source.value())->metadata().identity.value()))
                    return error<FilesOperationRecoveryActionResult>(AdapterError::MetadataChanged,
                        "operation_recovery.source_changed", ObservationState::Changed);

                auto fresh_id = new_native_operation_journal_id();
                if (!fresh_id.value())
                    return Result::unavailable(fresh_id.state(), fresh_id.unavailable_detail()->error);
                const auto fresh_journal_name = recovery_generated_name(
                    request.private_root.encoding(),
                    op == FileOperation::Move ? ".ck-move-journal-" : ".ck-copy-journal-",
                    *fresh_id.value(), item_report.item);
                const auto fresh_staging_name = recovery_generated_name(
                    evidence->destination_name.encoding(), ".ck-recovery-staging-",
                    *fresh_id.value(), item_report.item);
                if (!fresh_journal_name || !fresh_staging_name)
                    return error<FilesOperationRecoveryActionResult>(AdapterError::UnrepresentableName,
                        "operation_recovery.generated_name");

                auto fresh_journal = OperationJournal::create(op, *fresh_id.value(), request.limits.journal);
                if (!fresh_journal.value())
                    return Result::unavailable(fresh_journal.state(), fresh_journal.unavailable_detail()->error);
                auto fresh_file = open_native_operation_journal_file(request.private_root, *fresh_journal_name, true);
                if (!fresh_file.value())
                    return Result::unavailable(fresh_file.state(), fresh_file.unavailable_detail()->error);
                auto shared_file = std::shared_ptr<OperationJournalFile>(std::move(fresh_file).take_value());
                auto planned = append_operation_journal_file(*shared_file, **fresh_journal.value(),
                    {1, 0, OperationJournalState::Planned, {}});
                if (!planned.value())
                    return Result::unavailable(planned.state(), planned.unavailable_detail()->error);

                const auto src_parent_id = src_dir->identity();
                if (!src_parent_id.value())
                    return Result::unavailable(src_parent_id.state(), src_parent_id.unavailable_detail()->error);

                OperationExecutionState final_state = OperationExecutionState::Failed;
                std::optional<UnavailableObservation> job_failure;
                const bool same_volume_move = op == FileOperation::Move &&
                    src_parent_id.value()->volume == dest_id.value()->volume;
                if (same_volume_move) {
                    NativeOperationMoveRequest move_req{
                        *fresh_journal.value(),
                        shared_file,
                        0,
                        *held_source.value(),
                        dest_dir,
                        *dest_id.value(),
                        evidence->destination_name,
                        {},
                        true};
                    auto job = NativeOperationMoveJob::create(std::move(move_req));
                    if (!job.value())
                        return Result::unavailable(job.state(), job.unavailable_detail()->error);
                    auto move_job = std::move(job).take_value();
                    while (move_job->state() != OperationExecutionState::Complete &&
                           move_job->state() != OperationExecutionState::Failed &&
                           move_job->state() != OperationExecutionState::Cancelled &&
                           move_job->state() != OperationExecutionState::RetainedSource &&
                           move_job->state() != OperationExecutionState::NeedsReconciliation)
                        move_job->advance(stop);
                    final_state = move_job->state();
                    job_failure = move_job->result().failure;
                } else {
                    auto held_src_ptr = *held_source.value();
                    NativeOperationCopyRequest copy_req{
                        *fresh_journal.value(),
                        shared_file,
                        0,
                        src_dir,
                        *src_parent_id.value(),
                        *evidence->source_name,
                        *held_src_ptr->metadata().identity.value(),
                        dest_dir,
                        *dest_id.value(),
                        evidence->destination_address,
                        *fresh_staging_name,
                        evidence->destination_name,
                        {},
                        [held_src_ptr](OpenedEntry &staging, std::stop_token) {
                            OperationMetadataRequest meta_req;
                            meta_req.apply_traversal_metadata = [&staging, held_src_ptr](std::stop_token) {
                                return staging.apply_portable_basic_metadata(held_src_ptr->metadata());
                            };
                            meta_req.apply_security_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
                            meta_req.apply_final_times = [](std::stop_token) { return Observation<bool>::available(true); };
                            meta_req.flush_destination = [&staging](std::stop_token) { return staging.flush_content(); };
                            return OperationMetadataJob::create(std::move(meta_req));
                        },
                        op,
                        {}, {}, {}, {}, {}};
                    auto job = NativeOperationCopyJob::create(std::move(copy_req));
                    if (!job.value())
                        return Result::unavailable(job.state(), job.unavailable_detail()->error);
                    auto copy_job = std::move(job).take_value();
                    while (copy_job->state() != OperationExecutionState::Complete &&
                           copy_job->state() != OperationExecutionState::Failed &&
                           copy_job->state() != OperationExecutionState::Cancelled &&
                           copy_job->state() != OperationExecutionState::RetainedSource &&
                           copy_job->state() != OperationExecutionState::NeedsReconciliation)
                        copy_job->advance(stop);
                    final_state = copy_job->state();
                    job_failure = copy_job->result().failure;
                }
                (void)shared_file->finish_io();
                shared_file.reset();

                if (final_state == OperationExecutionState::Complete) {
                    ++out.items_completed;
                    // Retire the completed linked recovery journal as well so it doesn't clutter recovery.
                    if (auto linked_entry_obs = journal_dir->open_child(*fresh_journal_name, EntryAccess::MetadataOnly);
                        linked_entry_obs.value()) {
                        auto linked_entry = std::move(linked_entry_obs).take_value();
                        if (auto lmeta = linked_entry->inspect_basic();
                            lmeta.value() && lmeta.value()->identity.value()) {
                            const ObjectIdentity lid = *lmeta.value()->identity.value();
                            linked_entry.reset();
                            (void)journal_dir->retire_child(*fresh_journal_name, lid);
                        }
                    }
                } else if (final_state == OperationExecutionState::RetainedSource) {
                    ++out.items_retained_source;
                } else {
                    if (job_failure)
                        return Result::unavailable(job_failure->state, job_failure->error);
                    return error<FilesOperationRecoveryActionResult>(AdapterError::InvalidNativeResult,
                        "operation_recovery.linked_job_failed");
                }
            }

            if (out.items_completed == out.items_processed && out.items_retained_source == 0) {
                auto retired_journal = journal_dir->retire_child(request.journal_name, journal_file_id);
                if (!retired_journal.value())
                    return Result::unavailable(retired_journal.state(), retired_journal.unavailable_detail()->error);
                out.journal_retired = true;
                out.summary = (request.kind == FilesOperationRecoveryActionKind::ResumeRecordedJob
                    ? "Resumed recorded job (" : "Retried unresolved items (") +
                    std::to_string(out.items_completed) + " completed)";
            } else {
                out.summary = "Recovery partially completed: " + std::to_string(out.items_completed) +
                    " completed, " + std::to_string(out.items_retained_source) + " retained source";
            }
            return Result::available(std::move(out));
        },
        [&] { return finish_operation_journal_directory(*journal_dir); },
        "operation_recovery.action_directory_finish_io");
}

struct FilesOperationRecoveryService::State final {
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::pair<ck::files::NativeAbsolutePath, FilesOperationRecoveryLimits>> pending;
    std::optional<FilesOperationRecoveryActionRequest> pending_action;
    std::optional<FilesOperationRecoveryReply> reply;
    std::optional<FilesOperationRecoveryActionReply> action_reply;
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
    void emit_action(FilesOperationRecoveryActionReply value) {
        std::lock_guard lock(mutex);
        if (stopping) return;
        action_reply = std::move(value);
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
    void run_action(FilesOperationRecoveryActionRequest req) {
        try {
            auto res = execute_operation_recovery_action(req);
            FilesOperationRecoveryActionReply out;
            if (res.value()) {
                out.state = FilesOperationRecoveryState::Ready;
                out.result = std::move(res).take_value();
            } else {
                out.state = FilesOperationRecoveryState::Failed;
                out.error = *res.unavailable_detail();
            }
            emit_action(std::move(out));
        } catch (...) {
            FilesOperationRecoveryActionReply out;
            out.state = FilesOperationRecoveryState::WorkerFault;
            emit_action(std::move(out));
        }
    }
    void run() {
        for (;;) {
            std::optional<std::pair<ck::files::NativeAbsolutePath, FilesOperationRecoveryLimits>> job;
            std::optional<FilesOperationRecoveryActionRequest> action_job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value() || pending_action.has_value(); });
                if (stopping) { pending.reset(); pending_action.reset(); return; }
                if (pending_action) {
                    action_job = std::move(*pending_action);
                    pending_action.reset();
                } else {
                    job = std::move(*pending);
                    pending.reset();
                }
                busy = true;
                signalled = false;
            }
            if (action_job)
                run_action(std::move(*action_job));
            else
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
    if (state_->busy || state_->pending || state_->pending_action) return FilesOperationRecoverySubmission::Busy;
    state_->pending.emplace(std::move(private_root), limits);
    state_->condition.notify_one();
    return FilesOperationRecoverySubmission::Accepted;
}
FilesOperationRecoverySubmission FilesOperationRecoveryService::request_action(
    FilesOperationRecoveryActionRequest action) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return FilesOperationRecoverySubmission::Stopped;
    if (!action.private_root.is_valid() || !action.journal_name.is_valid() || !valid_limits(action.limits))
        return FilesOperationRecoverySubmission::Invalid;
    if (state_->busy || state_->pending || state_->pending_action) return FilesOperationRecoverySubmission::Busy;
    state_->pending_action.emplace(std::move(action));
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
std::optional<FilesOperationRecoveryActionReply> FilesOperationRecoveryService::take_action() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->action_reply);
    state_->action_reply.reset();
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
    return state_->busy || state_->pending.has_value() || state_->pending_action.has_value();
}

} // namespace ck::vision
