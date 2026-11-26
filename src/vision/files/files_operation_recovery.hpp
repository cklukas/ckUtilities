// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "frozen_inventory.hpp"
#include "operation_recovery.hpp"
#include "operation_journal_file.hpp"
#include "transfer_reconciliation.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace ck::vision {

struct FilesOperationRecoveryLimits final {
    std::size_t max_journals = 1024;
    ck::files::OperationJournalLimits journal{};
    ck::files::OperationJournalCodecLimits codec{};
    ck::files::NativeOperationRecoveryEvidenceLimits evidence{};
    // Each read performs at most DirectoryCursor::max_batch_size native steps,
    // including skipped entries. Bound batches even when no journal matches.
    // End is live enumeration completion, never a frozen directory snapshot.
    std::size_t max_scan_batches = 16;
};

struct FilesOperationRecoveryReport final {
    ck::files::OperationJournalId journal_id{};
    ck::files::NativeOperationRecoveryInspection inspection;
};

enum class FilesOperationJournalKind { Transfer, RetainedTarget };

struct FilesOperationRecoveryItem final {
    // The native name is retained for a future worker action. display_name is
    // diagnostic text only and must never be reparsed as authority.
    std::optional<ck::files::NativeName> native_name;
    std::string display_name;
    std::optional<FilesOperationRecoveryReport> report;
    std::optional<ck::files::UnavailableObservation> error;
    FilesOperationJournalKind kind = FilesOperationJournalKind::Transfer;
    std::optional<ck::files::TransferJournalInspection> reconciliation;
};

struct FilesOperationRecoveryScan final {
    std::vector<FilesOperationRecoveryItem> journals;
    bool requires_reconciliation = false;
    std::optional<ck::files::TransferJournalCatalog> reconciliation_catalog;
};

enum class FilesOperationRecoveryOverlapKind {
    UnfinishedTransfer,
    UnfinishedDeletion,
    RetainedSourceRepeat,
    UntrustedJournal,
    UnqualifiedJournal,
};

struct FilesOperationRecoveryOverlap final {
    FilesOperationRecoveryOverlapKind kind = FilesOperationRecoveryOverlapKind::UntrustedJournal;
    std::string journal_name;
    std::optional<ck::files::OperationJournalId> journal_id;
    std::optional<ck::files::OperationJournalState> state;
    std::optional<ck::files::OperationRecoveryAction> action;
    std::string reason;
    std::optional<std::string> source_name;
    std::optional<std::string> destination_name;
    std::optional<std::string> destination_address;
    std::optional<ck::files::TransferRequestDisposition> disposition;
    std::optional<ck::files::TransferRowReview> row_review;
    std::optional<ck::files::TransferLineageStatus> lineage;
};

struct FilesOperationRecoveryOverlapReport final {
    std::vector<FilesOperationRecoveryOverlap> conflicts;
    bool has_conflict() const noexcept { return !conflicts.empty(); }
    bool has_untrusted_journal() const noexcept;
    bool has_retained_source_repeat() const noexcept;
};

// Pure identity-based overlap analysis against a completed recovery scan.
// Matches strictly through NativeDurableIdentity::matches_live, never display
// paths. Unreadable, malformed, untrusted, or identity-less unfinished
// journals fail closed so a new transfer cannot bypass unresolved old intent.
FilesOperationRecoveryOverlapReport assess_operation_recovery_overlap(
    const FilesOperationRecoveryScan &scan,
    const ck::files::FrozenInventory &inventory,
    std::optional<ck::files::ObjectIdentity> destination = std::nullopt,
    std::optional<ck::files::ObjectIdentity> source_parent = std::nullopt);

// Delegates to ck-core-fs-lib's transfer reconciliation classifier
// (ResumeRecorded / RetryUnresolved / SubmitAgain) against a completed scan.
ck::files::Observation<ck::files::TransferRequestReview> review_operation_transfer_request(
    const FilesOperationRecoveryScan &scan,
    const ck::files::TransferRequestReviewInput &input,
    std::stop_token stop = {});

// Worker-owned read-only composition: discovers journals below the private
// journal root and assesses identity overlap for the proposed transfer.
ck::files::Observation<FilesOperationRecoveryOverlapReport> check_operation_recovery_overlap(
    const ck::files::NativeAbsolutePath &private_root,
    const ck::files::FrozenInventory &inventory,
    std::optional<ck::files::ObjectIdentity> destination = std::nullopt,
    std::optional<ck::files::ObjectIdentity> source_parent = std::nullopt,
    FilesOperationRecoveryLimits limits = {});

enum class FilesOperationRecoverySubmission {
    Accepted,
    Busy,
    Stopped,
    Invalid,
};

enum class FilesOperationRecoveryState {
    Ready,
    Failed,
    WorkerFault,
};

struct FilesOperationRecoveryReply final {
    FilesOperationRecoveryState state = FilesOperationRecoveryState::Failed;
    std::optional<FilesOperationRecoveryScan> scan;
    std::optional<ck::files::UnavailableObservation> error;
};

// Read-only discovery below the already-qualified private journal root. It
// never follows a matching symlink or turns display text back into authority.
// Every malformed, truncated, legacy, or ambiguous record remains an explicit
// review item instead of being silently ignored.
ck::files::Observation<FilesOperationRecoveryScan> discover_operation_recovery(
    const ck::files::NativeAbsolutePath &private_root,
    FilesOperationRecoveryLimits limits = {});

// Worker-owned startup boundary for ordinary operation recovery inspection.
// The service never resumes, deletes, replaces, or retires a native object.
class FilesOperationRecoveryService final {
  public:
    explicit FilesOperationRecoveryService(std::function<void()> wake = {});
    ~FilesOperationRecoveryService();
    FilesOperationRecoveryService(const FilesOperationRecoveryService &) = delete;
    FilesOperationRecoveryService &operator=(const FilesOperationRecoveryService &) = delete;

    FilesOperationRecoverySubmission request(
        ck::files::NativeAbsolutePath private_root, FilesOperationRecoveryLimits limits = {});
    std::optional<FilesOperationRecoveryReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ck::vision
