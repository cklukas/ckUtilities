// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_volume_service.hpp"
#include "native_path.hpp"
#include "operation_recovery.hpp"
#include "operation_journal_file.hpp"
#include "observation.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <span>
#include <vector>
#include <string_view>

namespace ck::vision {

enum class FilesVolumeArchiveConsistency {
    Live,
    Snapshot,
};

enum class FilesVolumeArchiveEntryKind : std::uint8_t {
    Directory = 1,
    RegularFile = 2,
    Symlink = 3,
    HardLink = 4,
    Unsupported = 5,
};

enum class FilesVolumeArchiveLossKind : std::uint8_t {
    UnsupportedObject = 1,
    Metadata = 2,
    AdditionalChannel = 3,
    Security = 4,
    StorageRepresentation = 5,
    Name = 6,
    Consistency = 7,
    Data = 8,
};

struct FilesVolumeArchiveLoss final {
    std::size_t entry_index = 0;
    std::string path;
    FilesVolumeArchiveLossKind kind = FilesVolumeArchiveLossKind::Metadata;
    bool hard_failure = false;
    std::string diagnostic;
};

struct FilesVolumeArchiveExtent final {
    std::uint64_t offset = 0;
    std::vector<std::byte> bytes;
};

struct FilesVolumeArchiveEntry final {
    // Native path bytes in the archive's declared source encoding. This is
    // not display text and must never be normalized or reparsed by the UI.
    std::string path;
    FilesVolumeArchiveEntryKind kind = FilesVolumeArchiveEntryKind::Directory;
    std::uint64_t logical_size = 0;
    std::vector<FilesVolumeArchiveExtent> extents;
    std::string link_target;
    std::uint64_t hard_link_group = 0;
};

struct FilesVolumeArchiveDocument final {
    FilesVolumeRecord source;
    FilesVolumeArchiveConsistency consistency = FilesVolumeArchiveConsistency::Live;
    // Archive paths are exact native bytes for POSIX or little-endian UTF-16
    // code units for Windows. Display UTF-8 is never used as an archive path.
    ck::files::NameEncoding path_encoding = ck::files::NameEncoding::PosixBytes;
    std::vector<FilesVolumeArchiveEntry> entries;
    std::vector<FilesVolumeArchiveLoss> losses;
    bool losses_reviewed = false;
};

enum class FilesVolumeArchiveDestinationState : std::uint8_t {
    Ready,
    Conflict,
    Unknown,
};

struct FilesVolumeArchiveReviewSummary final {
    std::uint64_t logical_bytes = 0;
    std::uint64_t data_bytes = 0;
    std::size_t entries = 0;
    std::size_t directories = 0;
    std::size_t regular_files = 0;
    std::size_t links = 0;
    std::size_t hard_links = 0;
    std::size_t unsupported_objects = 0;
    std::size_t sparse_files = 0;
    std::size_t losses = 0;
    std::size_t hard_losses = 0;
    bool destination_conflict = false;
};

enum class FilesVolumeArchiveReviewDecision : std::uint8_t {
    Cancel,
    Proceed,
    ContinueWithListedLosses,
};

class FilesVolumeArchiveReview;

struct FilesVolumeArchiveApproval final {
    std::shared_ptr<const FilesVolumeArchiveReview> review;
    bool continued_with_listed_losses = false;
};

class FilesVolumeArchiveReview final : public std::enable_shared_from_this<FilesVolumeArchiveReview> {
  public:
    using Pointer = std::shared_ptr<const FilesVolumeArchiveReview>;

    static ck::files::Observation<Pointer> create(
        FilesVolumeArchiveDocument document, ck::files::NativeAbsolutePath destination_directory,
        std::string archive_name, FilesVolumeArchiveDestinationState destination_state);

    static std::string default_archive_name(std::string_view volume_name);

    const FilesVolumeArchiveDocument &document() const noexcept { return document_; }
    const ck::files::NativeAbsolutePath &destination_directory() const noexcept { return destination_directory_; }
    const std::string &archive_name() const noexcept { return archive_name_; }
    const FilesVolumeArchiveReviewSummary &summary() const noexcept { return summary_; }
    bool cancel_is_default() const noexcept { return true; }
    bool requires_loss_consent() const noexcept { return !document_.losses.empty(); }

    // Approval binds the immutable document, destination, archive name and
    // loss list together. It performs no I/O and never authorizes mutation by
    // itself; the future worker must retain this review pointer and recheck
    // the source/destination identities before writing.
    ck::files::Observation<FilesVolumeArchiveApproval> approve(
        FilesVolumeArchiveReviewDecision decision,
        std::span<const std::size_t> reviewed_losses = {}) const;

  private:
    FilesVolumeArchiveReview(FilesVolumeArchiveDocument document,
                             ck::files::NativeAbsolutePath destination_directory,
                             std::string archive_name,
                             FilesVolumeArchiveReviewSummary summary) noexcept
        : document_(std::move(document)), destination_directory_(std::move(destination_directory)),
          archive_name_(std::move(archive_name)), summary_(summary) {}

    FilesVolumeArchiveDocument document_;
    ck::files::NativeAbsolutePath destination_directory_;
    std::string archive_name_;
    FilesVolumeArchiveReviewSummary summary_;
};

// Worker-owned planning boundary. It builds and validates the immutable source
// document and destination conflict state, then returns the review model to the
// UI. It never writes an archive; approval is a separate UI action.
struct FilesVolumeArchivePlanningRequest final {
    FilesVolumeRecord source;
    ck::files::NativeAbsolutePath destination_directory;
    std::string archive_name;
    FilesVolumeArchiveConsistency consistency = FilesVolumeArchiveConsistency::Live;
    std::function<ck::files::Observation<FilesVolumeArchiveDocument>(std::stop_token)> build_document;
};

enum class FilesVolumeArchivePlanningSubmission {
    Accepted,
    Busy,
    Stopped,
    Invalid,
};

enum class FilesVolumeArchivePlanningState {
    Ready,
    Failed,
    Cancelled,
    WorkerFault,
};

struct FilesVolumeArchivePlanningTicket final {
    FilesVolumeArchivePlanningTicket() = default;
    std::uint64_t number() const noexcept { return identity_ ? *identity_ : 0; }
    bool valid() const noexcept { return identity_ != nullptr; }
    friend bool operator==(const FilesVolumeArchivePlanningTicket &, const FilesVolumeArchivePlanningTicket &) = default;

  private:
    friend class FilesVolumeArchivePlanningService;
    explicit FilesVolumeArchivePlanningTicket(std::uint64_t number)
        : identity_(std::make_shared<const std::uint64_t>(number)) {}
    std::shared_ptr<const std::uint64_t> identity_;
};

struct FilesVolumeArchivePlanningSubmitted final {
    FilesVolumeArchivePlanningSubmission state = FilesVolumeArchivePlanningSubmission::Invalid;
    FilesVolumeArchivePlanningTicket ticket;
};

struct FilesVolumeArchivePlanningReply final {
    FilesVolumeArchivePlanningTicket request;
    FilesVolumeArchivePlanningState state = FilesVolumeArchivePlanningState::Failed;
    FilesVolumeArchiveReview::Pointer review;
    std::optional<ck::files::UnavailableObservation> error;
};

class FilesVolumeArchivePlanningService final {
  public:
    explicit FilesVolumeArchivePlanningService(std::function<void()> wake = {});
    ~FilesVolumeArchivePlanningService();
    FilesVolumeArchivePlanningService(const FilesVolumeArchivePlanningService &) = delete;
    FilesVolumeArchivePlanningService &operator=(const FilesVolumeArchivePlanningService &) = delete;

    FilesVolumeArchivePlanningSubmitted request(FilesVolumeArchivePlanningRequest request);
    bool cancel(const FilesVolumeArchivePlanningTicket &request);
    std::optional<FilesVolumeArchivePlanningReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

struct FilesVolumeArchiveWriteRequest final {
    FilesVolumeArchiveDocument document;
    std::filesystem::path destination_directory;
    std::string archive_name;
    std::uint64_t transaction_id = 1;
    // Optional for the standalone format writer; the Files archive service
    // supplies this worker-owned durable transition callback.
    std::function<ck::files::Observation<bool>(ck::files::OperationJournalState)> append_journal;
};

enum class FilesVolumeArchivePublication {
    Durable,
    PublishedDurabilityUnknown,
};

struct FilesVolumeArchiveWriteResult final {
    std::filesystem::path published_path;
    std::uint64_t logical_size = 0;
    std::uint64_t data_bytes = 0;
    std::size_t entry_count = 0;
    FilesVolumeArchivePublication publication = FilesVolumeArchivePublication::Durable;
    std::string diagnostic;
    bool digests_verified = false;
};

// The archive journal's evidence is intentionally small and identity-only.
// Startup inspection returns it together with the generic recovery projection;
// it never opens, deletes, resumes or publishes an archive.
struct FilesVolumeArchiveJournalEvidence final {
    std::uint64_t transaction_id = 0;
    std::string source_identity;
    std::string source_mount_point;
    std::string archive_name;
    // Version-2 journals retain the exact destination address in its native
    // encoding. Legacy journals may omit it and remain inspect-only.
    std::optional<ck::files::NameEncoding> destination_encoding;
    std::string destination_native_path;
    friend bool operator==(const FilesVolumeArchiveJournalEvidence &,
                           const FilesVolumeArchiveJournalEvidence &) = default;
};

struct FilesVolumeArchiveRecoveryReport final {
    ck::files::OperationJournalId journal_id;
    FilesVolumeArchiveJournalEvidence evidence;
    ck::files::OperationRecoveryReport operation;
};

struct FilesVolumeArchiveRecoveryLimits final {
    std::size_t max_journals = 1024;
    ck::files::OperationJournalLimits journal{};
    ck::files::OperationJournalCodecLimits codec{};
};

struct FilesVolumeArchiveRecoveryItem final {
    // The native name is retained for a future recovery action; display_name
    // is diagnostic text only and must never be reparsed as authority.
    std::optional<ck::files::NativeName> native_name;
    std::string display_name;
    std::optional<FilesVolumeArchiveRecoveryReport> report;
    std::optional<ck::files::UnavailableObservation> error;
};

struct FilesVolumeArchiveRecoveryScan final {
    std::vector<FilesVolumeArchiveRecoveryItem> journals;
    bool requires_reconciliation = false;
};

enum class FilesVolumeArchiveRecoverySubmission {
    Accepted,
    Busy,
    Stopped,
    Invalid,
};

enum class FilesVolumeArchiveRecoveryState {
    Ready,
    Failed,
    WorkerFault,
};

struct FilesVolumeArchiveRecoveryReply final {
    FilesVolumeArchiveRecoveryState state = FilesVolumeArchiveRecoveryState::Failed;
    std::optional<FilesVolumeArchiveRecoveryScan> scan;
    std::optional<ck::files::UnavailableObservation> error;
};

ck::files::Observation<FilesVolumeArchiveRecoveryReport> inspect_volume_archive_recovery(
    const ck::files::OperationJournal &journal,
    std::optional<ck::files::UnavailableObservation> tail = std::nullopt);

ck::files::Observation<FilesVolumeArchiveRecoveryReport> inspect_volume_archive_recovery(
    ck::files::OperationJournalFile &file,
    ck::files::OperationJournalLimits journal_limits = {},
    ck::files::OperationJournalCodecLimits codec_limits = {});

// Read-only startup discovery below the already-qualified private journal
// root. It never follows a matching symlink, mutates a journal, or turns a
// display name back into a native path. Matching malformed/unfinished entries
// are retained as explicit reconciliation items instead of being ignored.
ck::files::Observation<FilesVolumeArchiveRecoveryScan> discover_volume_archive_recovery(
    const ck::files::NativeAbsolutePath &private_root,
    FilesVolumeArchiveRecoveryLimits limits = {});

// Worker-owned startup boundary for the read-only recovery projection. The
// service never resumes, deletes, opens for mutation, or publishes an archive.
// A caller may submit one bounded scan at a time and consume its reply from
// the UI thread.
class FilesVolumeArchiveRecoveryService final {
  public:
    explicit FilesVolumeArchiveRecoveryService(std::function<void()> wake = {});
    ~FilesVolumeArchiveRecoveryService();
    FilesVolumeArchiveRecoveryService(const FilesVolumeArchiveRecoveryService &) = delete;
    FilesVolumeArchiveRecoveryService &operator=(const FilesVolumeArchiveRecoveryService &) = delete;

    FilesVolumeArchiveRecoverySubmission request(
        ck::files::NativeAbsolutePath private_root, FilesVolumeArchiveRecoveryLimits limits = {});
    std::optional<FilesVolumeArchiveRecoveryReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

ck::files::Observation<FilesVolumeArchiveWriteResult> write_volume_archive(
    const FilesVolumeArchiveWriteRequest &request, std::stop_token stop_token = {});

struct FilesVolumeArchiveRequest final {
    FilesVolumeRecord source;
    ck::files::NativeAbsolutePath destination_directory;
    std::string archive_name;
    FilesVolumeArchiveConsistency consistency = FilesVolumeArchiveConsistency::Live;
    std::uint64_t transaction_id = 1;
    // The native source adapter is worker-owned. It must return a complete
    // immutable document or an explicit unavailable result; it may not expose
    // native handles or partial entries to FilesApp.
    std::function<ck::files::Observation<FilesVolumeArchiveDocument>(std::stop_token)> build_document;
    std::optional<ck::files::NativeAbsolutePath> journal_root;
    ck::files::OperationJournalId journal_id{};
    ck::files::OperationJournalLimits journal_limits{};
    std::function<ck::files::Observation<std::unique_ptr<ck::files::OperationJournalFile>>(
        const ck::files::NativeAbsolutePath &, const ck::files::NativeName &, bool)> open_journal;
};

enum class FilesVolumeArchiveSubmission {
    Accepted,
    Busy,
    Stopped,
    Invalid,
};

enum class FilesVolumeArchiveState {
    Complete,
    Indeterminate,
    Failed,
    Cancelled,
    WorkerFault,
};

struct FilesVolumeArchiveTicket final {
    FilesVolumeArchiveTicket() = default;
    std::uint64_t number() const noexcept { return identity_ ? *identity_ : 0; }
    bool valid() const noexcept { return identity_ != nullptr; }
    friend bool operator==(const FilesVolumeArchiveTicket &, const FilesVolumeArchiveTicket &) = default;

  private:
    friend class FilesVolumeArchiveService;
    explicit FilesVolumeArchiveTicket(std::uint64_t number) : identity_(std::make_shared<const std::uint64_t>(number)) {}
    std::shared_ptr<const std::uint64_t> identity_;
};

struct FilesVolumeArchiveSubmitted final {
    FilesVolumeArchiveSubmission state = FilesVolumeArchiveSubmission::Invalid;
    FilesVolumeArchiveTicket ticket;
};

struct FilesVolumeArchiveReply final {
    FilesVolumeArchiveTicket request;
    FilesVolumeArchiveState state = FilesVolumeArchiveState::Failed;
    std::optional<FilesVolumeArchiveWriteResult> result;
    std::optional<ck::files::UnavailableObservation> error;
};

class FilesVolumeArchiveService final {
  public:
    explicit FilesVolumeArchiveService(std::function<void()> wake = {});
    ~FilesVolumeArchiveService();
    FilesVolumeArchiveService(const FilesVolumeArchiveService &) = delete;
    FilesVolumeArchiveService &operator=(const FilesVolumeArchiveService &) = delete;

    FilesVolumeArchiveSubmitted request(FilesVolumeArchiveRequest request);
    bool cancel(const FilesVolumeArchiveTicket &request);
    std::optional<FilesVolumeArchiveReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;
    bool working() const;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace ck::vision
