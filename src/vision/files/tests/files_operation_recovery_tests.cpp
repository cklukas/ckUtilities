// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery.hpp"
#include "files_operation_recovery_cleanup.hpp"

#include "native_filesystem.hpp"
#include "ck/file_io.hpp"
#include "operation_journal_file.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "scratch_directory.hpp"
#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
using namespace ck::files;
using namespace ck::vision;

NativeAbsolutePath address(const std::filesystem::path &path) {
    auto value = NativeAbsolutePath::from_host(path);
    if (!value.value())
        throw std::runtime_error("invalid operation recovery fixture address");
    return std::move(value).take_value();
}

OperationJournalId journal_id() {
    OperationJournalId id;
    id.bytes[0] = std::byte{0x91};
    return id;
}

// Deliberately malformed contents still use a real private native journal.
// Keep the same owner through every write, flush and fallible native close.
bool write_fixture_journal_bytes(const NativeAbsolutePath &root, const NativeName &name,
                                 std::string_view text) {
    auto opened = open_native_operation_journal_file(root, name, true);
    CK_CHECK(opened.value());
    if (!opened.value()) return false;
    auto file = std::move(opened).take_value();
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = file->write_at(offset, bytes.subspan(offset));
        const bool progress = written.value() && *written.value() != 0 &&
                              *written.value() <= bytes.size() - offset;
        CK_CHECK(progress);
        if (!progress) break;
        offset += *written.value();
    }
    // Drain the actual owner even if an earlier write failed. Neither success
    // nor a retained C++ object substitutes for checked native completion.
    const auto size = file->size();
    CK_CHECK(size.value() && *size.value() == bytes.size());
    const auto flushed = file->flush();
    CK_CHECK(flushed.value() && *flushed.value());
    const auto finished = file->finish_io();
    CK_CHECK(finished.value() && *finished.value());
    return offset == bytes.size() && size.value() && *size.value() == bytes.size() &&
           flushed.value() && *flushed.value() &&
           finished.value() && *finished.value();
}

NativeOperationRecoveryEvidence evidence() {
    AdapterScope scope;
    const auto identity = *NativeDurableIdentity::from_live(
        {scope, IdentityScheme::PosixDeviceInode, NativeIdentifier::from_u64(17),
         NativeIdentifier::from_u64(18), {}});
    return {*NativeName::from_posix(".ck-staging"), *NativeName::from_posix("destination"),
            identity, identity, identity, identity, {}, {}, {}};
}
}

CK_TEST(files_operation_recovery_discovery_retains_unfinished_and_malformed_journals) {
    const ck::testing::ScratchDirectory scratch("files-operation-recovery-discovery", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto destination_path = scratch.path() / "destination";
    const auto journal_path = scratch.path() / "journals";
    CK_CHECK(ck::io::create_directories(destination_path));
    CK_CHECK(ck::io::create_directories(journal_path));
    const auto destination_address = address(destination_path);
    const auto root = address(journal_path);
    CK_CHECK(prepare_native_operation_journal_root(root).value());

#if defined(_WIN32)
    const auto name = *NativeName::from_windows(u".ck-copy-journal-0001");
#else
    const auto name = *NativeName::from_posix(".ck-copy-journal-0001");
#endif
    auto created = OperationJournal::create(FileOperation::Copy, journal_id(), {4, 32, 4096});
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto journal = std::move(created).take_value();
    const auto encoded = encode_native_operation_recovery_evidence(evidence());
    CK_CHECK(encoded.value());
    if (!encoded.value()) return;

    auto file = open_native_operation_journal_file(root, name, true);
    CK_CHECK(file.value());
    if (!file.value()) return;
    CK_CHECK(append_operation_journal_file(**file.value(), *journal,
                                          {1, 0, OperationJournalState::Planned, {}}).value());
    CK_CHECK(append_operation_journal_file(**file.value(), *journal,
                                          {2, 0, OperationJournalState::Prepared, *encoded.value()}).value());

#if defined(_WIN32)
    // The live private writer deliberately excludes another journal open.
    // Preserve this refusal instead of treating an in-flight journal as a
    // completed recovery report or weakening its sharing contract.
    const auto live_scan = discover_operation_recovery(root);
    CK_CHECK(live_scan.value() && live_scan.value()->journals.size() == 1 &&
             live_scan.value()->requires_reconciliation);
    if (live_scan.value() && live_scan.value()->journals.size() == 1) {
        const auto &live = live_scan.value()->journals.front();
        CK_CHECK(live.native_name && *live.native_name == name);
        CK_CHECK(!live.report && live.error.has_value());
        if (live.error) {
            CK_CHECK(live.error->state == ObservationState::Denied);
            CK_CHECK(live.error->error.domain == ErrorDomain::NtStatus);
            CK_CHECK(static_cast<std::uint32_t>(live.error->error.code) == 0xc0000043U);
            CK_CHECK(live.error->error.operation == "journal_reader.NtCreateFile");
        }
    }
#endif
    const auto finished = (**file.value()).finish_io();
    CK_CHECK(finished.value() && *finished.value());
    if (!finished.value() || !*finished.value()) return;

#if defined(_WIN32)
    const auto malformed_name = *NativeName::from_windows(u".ck-move-journal-0002");
#else
    const auto malformed_name = *NativeName::from_posix(".ck-move-journal-0002");
#endif
    if (!write_fixture_journal_bytes(root, malformed_name, "torn")) return;
    CK_CHECK(ck::io::create_directories(journal_path / ".ck-delete-tree-journal-0003"));
    std::string malformed_before, journal_before;
    CK_CHECK(ck::io::read_file(journal_path / ".ck-move-journal-0002", malformed_before, 5));
    CK_CHECK(malformed_before == "torn");
    CK_CHECK(ck::io::read_file(journal_path / ".ck-copy-journal-0001", journal_before, 64 * 1024));

    const auto scan = discover_operation_recovery(root);
    CK_CHECK(scan.value() && scan.value()->journals.size() == 3 &&
             scan.value()->requires_reconciliation);
    if (!scan.value()) return;
    const auto found = std::find_if(scan.value()->journals.begin(), scan.value()->journals.end(),
                                    [](const auto &item) { return item.display_name == ".ck-copy-journal-0001"; });
    CK_CHECK(found != scan.value()->journals.end() && found->report.has_value());
    if (found != scan.value()->journals.end() && found->report) {
        CK_CHECK(found->report->journal_id == journal_id());
        CK_CHECK(found->report->inspection.report.requires_reconciliation);
        CK_CHECK(found->report->inspection.artifacts.size() == 1);
    }
    const auto malformed_item = std::find_if(scan.value()->journals.begin(), scan.value()->journals.end(),
                                             [](const auto &item) { return item.display_name == ".ck-move-journal-0002"; });
    const auto directory_item = std::find_if(scan.value()->journals.begin(), scan.value()->journals.end(),
                                             [](const auto &item) { return item.display_name == ".ck-delete-tree-journal-0003"; });
    CK_CHECK(malformed_item != scan.value()->journals.end() && malformed_item->error.has_value());
    CK_CHECK(directory_item != scan.value()->journals.end() && directory_item->error.has_value());
    // Discovery is read-only evidence, never replay or cleanup authority.
    std::string malformed_after, journal_after;
    CK_CHECK(ck::io::read_file(journal_path / ".ck-move-journal-0002", malformed_after, 5));
    CK_CHECK(malformed_after == malformed_before);
    CK_CHECK(ck::io::read_file(journal_path / ".ck-copy-journal-0001", journal_after, 64 * 1024));
    CK_CHECK(journal_after == journal_before);
}

CK_TEST(files_operation_recovery_service_supports_repeated_read_only_scans) {
    const ck::testing::ScratchDirectory scratch("files-operation-recovery-service", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = address(scratch.path());
    CK_CHECK(prepare_native_operation_journal_root(root).value());
#if defined(_WIN32)
    const auto name = *NativeName::from_windows(u".ck-copy-journal-repeated");
#else
    const auto name = *NativeName::from_posix(".ck-copy-journal-repeated");
#endif
    constexpr std::string_view torn = "malformed but retained for review";
    if (!write_fixture_journal_bytes(root, name, torn)) return;
    std::string before;
    CK_CHECK(ck::io::read_file(scratch.path() / ".ck-copy-journal-repeated", before, torn.size() + 1));
    CK_CHECK(before == torn);

    std::mutex wake_mutex;
    std::condition_variable wake_condition;
    FilesOperationRecoveryService service([&] { wake_condition.notify_all(); });
    const auto wait_for_reply = [&]() -> std::optional<FilesOperationRecoveryReply> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            if (auto reply = service.take())
                return reply;
            if (std::chrono::steady_clock::now() >= deadline)
                return std::nullopt;
            std::unique_lock lock(wake_mutex);
            wake_condition.wait_for(lock, std::chrono::milliseconds(10));
        }
    };

    CK_CHECK(service.request(root) == FilesOperationRecoverySubmission::Accepted);
    const auto first = wait_for_reply();
    CK_CHECK(first && first->state == FilesOperationRecoveryState::Ready && first->scan);
    if (!first || !first->scan)
        return;
    CK_CHECK(first->scan->journals.size() == 1 && first->scan->journals[0].error.has_value());

    // A second request is a fresh bounded observation, not a continuation or
    // mutation of the first result. This models repeated startup inspection.
    CK_CHECK(service.request(root) == FilesOperationRecoverySubmission::Accepted);
    const auto second = wait_for_reply();
    CK_CHECK(second && second->state == FilesOperationRecoveryState::Ready && second->scan);
    if (second && second->scan)
        CK_CHECK(second->scan->journals.size() == 1 && second->scan->journals[0].error.has_value());
    CK_CHECK(ck::io::is_regular_file(scratch.path() / ".ck-copy-journal-repeated"));
    std::string after;
    CK_CHECK(ck::io::read_file(scratch.path() / ".ck-copy-journal-repeated", after, torn.size() + 1));
    CK_CHECK(after == before);

    service.request_stop();
    service.join();
    CK_CHECK(service.request(root) == FilesOperationRecoverySubmission::Stopped);
}

CK_TEST(files_operation_recovery_reopens_the_same_durable_journal_after_worker_restart) {
    const ck::testing::ScratchDirectory scratch("files-operation-recovery-restart", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = address(scratch.path());
    CK_CHECK(prepare_native_operation_journal_root(root).value());
#if defined(_WIN32)
    const auto name = *NativeName::from_windows(u".ck-copy-journal-restart");
#else
    const auto name = *NativeName::from_posix(".ck-copy-journal-restart");
#endif
    const auto id = journal_id();
    auto journal = OperationJournal::create(FileOperation::Copy, id, {1, 8, 4096});
    CK_CHECK(journal.value());
    if (!journal.value())
        return;
    auto file_result = open_native_operation_journal_file(root, name, true);
    CK_CHECK(file_result.value());
    if (!file_result.value())
        return;
    auto file = std::move(file_result).take_value();
    CK_CHECK(append_operation_journal_file(*file, **journal.value(),
                                           {1, 0, OperationJournalState::Planned, {}}).value());
    const auto finished = file->finish_io();
    CK_CHECK(finished.value() && *finished.value());
    if (!finished.value() || !*finished.value()) return;
    file.reset(); // The next discovery is a fresh worker/process boundary.

    const auto first = discover_operation_recovery(root);
    CK_CHECK(first.value() && first.value()->journals.size() == 1);
    if (!first.value() || first.value()->journals.empty())
        return;
    CK_CHECK(first.value()->journals.front().report.has_value());
    if (first.value()->journals.front().report)
        CK_CHECK(first.value()->journals.front().report->journal_id == id &&
                 first.value()->journals.front().report->inspection.report.requires_reconciliation);

    auto reopened_result = open_native_operation_journal_file(root, name, false);
    CK_CHECK(reopened_result.value());
    if (!reopened_result.value())
        return;
    auto reopened = std::move(reopened_result).take_value();
    const auto loaded = load_operation_journal(*reopened);
    CK_CHECK(loaded.value() && loaded.value()->journal->id() == id &&
             loaded.value()->journal->records().size() == 1 && !loaded.value()->tail.has_value());
    const auto reopened_finished = reopened->finish_io();
    CK_CHECK(reopened_finished.value() && *reopened_finished.value());
    if (!reopened_finished.value() || !*reopened_finished.value()) return;
    reopened.reset();

    const auto second = discover_operation_recovery(root);
    CK_CHECK(second.value() && second.value()->journals.size() == 1 &&
             second.value()->journals.front().report.has_value());
    if (second.value() && !second.value()->journals.empty() && second.value()->journals.front().report)
        CK_CHECK(second.value()->journals.front().report->journal_id == id &&
                 second.value()->journals.front().report->inspection.report.requires_reconciliation);
}

CK_TEST(files_operation_recovery_discovers_completed_and_interrupted_target_retention_after_restart) {
    const ck::testing::ScratchDirectory scratch("files-retention-recovery", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = address(scratch.path());
    CK_CHECK(prepare_native_operation_journal_root(root).value());
    const auto native_name = [](std::string spelling) {
#ifdef _WIN32
        return *NativeName::from_windows(std::u16string(spelling.begin(), spelling.end()));
#else
        return *NativeName::from_posix(std::move(spelling));
#endif
    };
    const auto identity = [](std::uint64_t number) {
        return NativeDurableIdentity{
#ifdef _WIN32
            IdentityScheme::WindowsFileId128,
#else
            IdentityScheme::PosixDeviceInode,
#endif
            NativeIdentifier::from_u64(17), NativeIdentifier::from_u64(number), {}};
    };
    const auto parent = identity(20);
    const auto old = identity(21);
    const auto original_name = native_name("old-target.txt");
    const auto retained_name = native_name("old-target.previous.txt");
    const auto write_journal = [&](const NativeName &filename, bool complete, bool malformed) {
        const auto journal = OperationJournal::create(FileOperation::Move, journal_id());
        auto opened = open_native_operation_journal_file(root, filename, true);
        if (!journal.value() || !opened.value())
            throw std::runtime_error("cannot create retention journal fixture");
        auto file = std::move(opened).take_value();
        NativeOperationRecoveryEvidence entry{original_name, retained_name, parent, old,
            malformed ? identity(22) : parent, old, root, std::array<std::byte, 32>{}, {}};
        if (!complete)
            entry.source_name = original_name;
        std::uint64_t sequence = 0;
        const auto append = [&](OperationJournalState state) {
            const auto encoded = encode_native_operation_recovery_evidence(entry);
            if (!encoded.value() || !append_operation_journal_file(*file, **journal.value(),
                    {++sequence, 0, state, *encoded.value()}).value())
                throw std::runtime_error("cannot append retention journal fixture");
        };
        append(OperationJournalState::Planned);
        append(OperationJournalState::Prepared);
        append(OperationJournalState::CommitIntended);
        if (complete) {
            entry.published_identity = old;
            append(OperationJournalState::DestinationCommitted);
            append(OperationJournalState::SourceRemovalIntended);
            append(OperationJournalState::SourceRetired);
            append(OperationJournalState::Complete);
        }
        const auto finished = file->finish_io();
        CK_CHECK(finished.value() && *finished.value());
    };
    const auto completed = native_name(".ck-retention-01000000000000000000000000000000.journal");
    const auto interrupted = native_name(".ck-retention-02000000000000000000000000000000.journal");
    const auto malformed = native_name(".ck-retention-03000000000000000000000000000000.journal");
    const auto incoming = native_name(".ck-reviewed-journal-04000000000000000000000000000000");
    write_journal(completed, true, false);
    write_journal(interrupted, false, false);
    write_journal(malformed, true, true);
    if (!write_fixture_journal_bytes(root, incoming, "torn incoming journal"))
        throw std::runtime_error("cannot create incoming journal fixture");

    const auto scan = discover_operation_recovery(root);
    CK_CHECK(scan.value() && scan.value()->journals.size() == 4 && scan.value()->requires_reconciliation);
    if (!scan.value())
        return;
    const auto find = [&](const NativeName &name) -> const FilesOperationRecoveryItem& {
        const auto found = std::find_if(scan.value()->journals.begin(), scan.value()->journals.end(),
            [&](const auto &item) { return item.native_name == name; });
        if (found == scan.value()->journals.end())
            throw std::runtime_error("retention journal was omitted from discovery");
        return *found;
    };
    const auto &done = find(completed);
    CK_CHECK(done.kind == FilesOperationJournalKind::RetainedTarget && done.report);
    if (!done.report || done.report->inspection.artifacts.size() != 1)
        throw std::runtime_error("completed retention journal omitted its evidence");
    const auto &receipt = done.report->inspection.artifacts.front();
    CK_CHECK(receipt.state == OperationJournalState::Complete && receipt.evidence);
    CK_CHECK(receipt.evidence && !receipt.evidence->source_name && receipt.evidence->staging_name == original_name &&
        receipt.evidence->destination_name == retained_name && receipt.evidence->destination_address == root);
    CK_CHECK(!done.report->inspection.report.requires_reconciliation);
    const auto &pending = find(interrupted);
    CK_CHECK(pending.report && pending.report->inspection.report.requires_reconciliation &&
        pending.report->inspection.report.items.front().action == OperationRecoveryAction::RequireReview &&
        pending.report->inspection.artifacts.size() == 1 &&
        pending.report->inspection.artifacts.front().state == OperationJournalState::CommitIntended);
    const auto &bad = find(malformed);
    CK_CHECK(bad.report && bad.report->inspection.has_untrusted_evidence &&
        bad.report->inspection.report.requires_reconciliation && bad.report->inspection.artifacts.empty());
    CK_CHECK(find(incoming).kind == FilesOperationJournalKind::Transfer && find(incoming).error);
}

CK_TEST(files_operation_recovery_cleanup_requires_live_ownership_and_preserves_evidence) {
    const ck::testing::ScratchDirectory scratch("files-operation-recovery-cleanup", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto destination_path = scratch.path() / "destination";
    const auto journal_path = scratch.path() / "journals";
    CK_CHECK(ck::io::create_directories(destination_path));
    CK_CHECK(ck::io::create_directories(journal_path));
    const auto destination_address = address(destination_path);
    const auto root = address(journal_path);
    CK_CHECK(prepare_native_operation_journal_root(root).value());
    ck::files::NativeFileSystem filesystem;
    auto destination_result = filesystem.open_native_directory(destination_address);
    CK_CHECK(destination_result.value());
    if (!destination_result.value()) return;
    auto destination = std::shared_ptr<const DirectoryCursor>(std::move(destination_result).take_value());
 #if defined(_WIN32)
    auto staging_name = NativeName::from_windows(u".ck-copy-staging-recovery");
 #else
    auto staging_name = NativeName::from_posix(".ck-copy-staging-recovery");
 #endif
    CK_CHECK(staging_name.has_value());
    if (!staging_name) return;
    auto staging_result = destination->create_child_file(*staging_name);
    CK_CHECK(staging_result.value());
    if (!staging_result.value()) return;
    auto staging = std::move(staging_result).take_value();
    const auto staging_metadata = staging->inspect_basic();
    const auto destination_identity = destination->identity();
    CK_CHECK(staging_metadata.value() && staging_metadata.value()->identity.value() &&
             destination_identity.value());
    if (!staging_metadata.value() || !staging_metadata.value()->identity.value() ||
        !destination_identity.value()) return;
    const auto staging_durable = NativeDurableIdentity::from_live(*staging_metadata.value()->identity.value());
    const auto destination_durable = NativeDurableIdentity::from_live(*destination_identity.value());
    CK_CHECK(staging_durable && destination_durable);
    if (!staging_durable || !destination_durable) return;

#if defined(_WIN32)
    const auto journal_name = *NativeName::from_windows(u".ck-copy-journal-recovery-cleanup");
#else
    const auto journal_name = *NativeName::from_posix(".ck-copy-journal-recovery-cleanup");
#endif
    OperationJournalId id;
    id.bytes[0] = std::byte{0xa1};
    auto journal_result = OperationJournal::create(FileOperation::Copy, id, {1, 8, 4096});
    CK_CHECK(journal_result.value());
    if (!journal_result.value()) return;
    auto journal = std::move(journal_result).take_value();
 #if defined(_WIN32)
    const auto published_name = *NativeName::from_windows(u"published");
 #else
    const auto published_name = *NativeName::from_posix("published");
 #endif
    NativeOperationRecoveryEvidence receipt{
        *staging_name, published_name, *destination_durable,
        *destination_durable, *destination_durable, *staging_durable,
        destination_address, {}, {}};
    const auto encoded = encode_native_operation_recovery_evidence(receipt);
    CK_CHECK(encoded.value());
    if (!encoded.value()) return;
    auto journal_file_result = open_native_operation_journal_file(root, journal_name, true);
    CK_CHECK(journal_file_result.value());
    if (!journal_file_result.value()) return;
    auto journal_file = std::move(journal_file_result).take_value();
    CK_CHECK(append_operation_journal_file(*journal_file, *journal,
        {1, 0, OperationJournalState::Planned, {}}).value());
    CK_CHECK(append_operation_journal_file(*journal_file, *journal,
        {2, 0, OperationJournalState::Prepared, *encoded.value()}).value());
    CK_CHECK(append_operation_journal_file(*journal_file, *journal,
        {3, 0, OperationJournalState::Copied, *encoded.value()}).value());
    const auto journal_finished = journal_file->finish_io();
    CK_CHECK(journal_finished.value() && *journal_finished.value());
    if (!journal_finished.value() || !*journal_finished.value()) return;
    journal_file.reset();
    staging.reset();
    destination.reset();

    std::string journal_before;
    const auto journal_file_path = journal_path / ".ck-copy-journal-recovery-cleanup";
    CK_CHECK(ck::io::read_file(journal_file_path, journal_before));
    std::mutex wake_mutex;
    std::condition_variable wake_condition;
    FilesOperationRecoveryCleanupService service([&] { wake_condition.notify_all(); });
    CK_CHECK(service.request(root, journal_name, id) ==
             FilesOperationRecoveryCleanupSubmission::Accepted);
    std::optional<FilesOperationRecoveryCleanupReply> reply;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!reply && std::chrono::steady_clock::now() < deadline) {
        reply = service.take();
        if (!reply) {
            std::unique_lock lock(wake_mutex);
            wake_condition.wait_for(lock, std::chrono::milliseconds(10));
        }
    }
    CK_CHECK(reply.has_value());
    if (reply) {
        CK_CHECK(reply->state == FilesOperationRecoveryCleanupState::Failed);
        CK_CHECK(reply->error && reply->error->state == ObservationState::Unknown);
        CK_CHECK(reply->error && reply->error->error.domain == ErrorDomain::Adapter);
        CK_CHECK(reply->error && reply->error->error.code == static_cast<std::int64_t>(AdapterError::UnqualifiedIdentity));
        CK_CHECK(reply->error && reply->error->error.operation ==
                 "operation_recovery_cleanup.live_creator_ownership_required");
    }
    std::string staging_bytes, journal_after;
    CK_CHECK(ck::io::read_file(destination_path / ".ck-copy-staging-recovery", staging_bytes));
    CK_CHECK(staging_bytes.empty());
    CK_CHECK(ck::io::read_file(journal_file_path, journal_after));
    CK_CHECK(journal_after == journal_before);
    service.request_stop();
    service.join();
    // A second worker has no additional authority. Preserve the exact journal
    // and retained object rather than appending a fabricated Cancelled record.
    FilesOperationRecoveryCleanupService restarted;
    CK_CHECK(restarted.request(root, journal_name, id) ==
             FilesOperationRecoveryCleanupSubmission::Accepted);
    std::optional<FilesOperationRecoveryCleanupReply> restarted_reply;
    const auto restart_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!restarted_reply && std::chrono::steady_clock::now() < restart_deadline) {
        restarted_reply = restarted.take();
        if (!restarted_reply)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CK_CHECK(restarted_reply.has_value());
    if (restarted_reply)
        CK_CHECK(restarted_reply->state == FilesOperationRecoveryCleanupState::Failed);
    restarted.request_stop();
    restarted.join();
    CK_CHECK(ck::io::read_file(journal_file_path, journal_after));
    CK_CHECK(journal_after == journal_before);
    CK_CHECK(ck::io::read_file(destination_path / ".ck-copy-staging-recovery", staging_bytes));
}

#if defined(_WIN32)
CK_TEST(files_operation_recovery_native_journal_preserves_long_raw_utf16_root) {
    const ck::testing::ScratchDirectory scratch("files-operation-recovery-native-root",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    auto journal_path = scratch.path();
    const std::wstring component = L"native-\u03bb-" + std::wstring(48, L'x');
    for (unsigned index = 0; index != 5; ++index)
        journal_path /= component + std::to_wstring(index);
    std::wstring raw = L"raw-";
    raw.push_back(static_cast<wchar_t>(0xd800));
    journal_path /= raw;
    CK_CHECK(journal_path.native().size() > 260);
    CK_CHECK(ck::io::create_directories(journal_path));
    const auto root = address(journal_path);
    const auto prepared = prepare_native_operation_journal_root(root);
    CK_CHECK(prepared.value() && *prepared.value());
    if (!prepared.value() || !*prepared.value()) return;
    const auto name = *NativeName::from_windows(u".ck-copy-journal-native-root");
    constexpr std::string_view torn = "torn-native-root";
    if (!write_fixture_journal_bytes(root, name, torn)) return;
    std::string before;
    CK_CHECK(ck::io::read_file(journal_path / L".ck-copy-journal-native-root", before, torn.size() + 1));
    CK_CHECK(before == torn);
    const auto scan = discover_operation_recovery(root);
    CK_CHECK(scan.value() && scan.value()->journals.size() == 1 &&
             scan.value()->requires_reconciliation);
    if (scan.value() && scan.value()->journals.size() == 1) {
        const auto &item = scan.value()->journals.front();
        CK_CHECK(item.native_name && *item.native_name == name);
        CK_CHECK(!item.report && item.error.has_value());
    }
    std::string after;
    CK_CHECK(ck::io::read_file(journal_path / L".ck-copy-journal-native-root", after, torn.size() + 1));
    CK_CHECK(after == before);
}
#endif


#if defined(_WIN32)
CK_TEST(files_operation_recovery_preserves_raw_utf16_journal_suffix_as_error_item) {
    const ck::testing::ScratchDirectory scratch("files-operation-recovery-native-name",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = address(scratch.path());
    const auto prepared = prepare_native_operation_journal_root(root);
    CK_CHECK(prepared.value() && *prepared.value());
    if (!prepared.value() || !*prepared.value()) return;

    // The recognized prefix is ASCII; the suffix is exact native UTF-16 data.
    // Display text is never parsed back into a filename or mutation authority.
    std::u16string units = u".ck-copy-journal-native-";
    units.push_back(static_cast<char16_t>(0xd800));
    const auto name = NativeName::from_windows(units);
    CK_CHECK(name.has_value());
    if (!name) return;
    constexpr std::string_view torn = "torn-raw-journal-suffix";
    if (!write_fixture_journal_bytes(root, *name, torn)) return;

    std::wstring host_name;
    host_name.reserve(units.size());
    for (const auto unit : units) host_name.push_back(static_cast<wchar_t>(unit));
    const auto journal_path = scratch.path() / host_name;
    std::string before;
    CK_CHECK(ck::io::is_regular_file(journal_path));
    CK_CHECK(ck::io::read_file(journal_path, before, torn.size() + 1));
    CK_CHECK(before == torn);

    const auto scan = discover_operation_recovery(root);
    CK_CHECK(scan.value() && scan.value()->journals.size() == 1 &&
             scan.value()->requires_reconciliation);
    if (scan.value() && scan.value()->journals.size() == 1) {
        const auto &item = scan.value()->journals.front();
        CK_CHECK(item.native_name && *item.native_name == *name);
        CK_CHECK(item.native_name && item.native_name->windows_units() &&
                 *item.native_name->windows_units() == units);
        CK_CHECK(!item.report && item.error.has_value());
    }
    // Read-only inspection must retain the precise malformed artifact.
    // Existing live-writer denial and no-creator-ownership cleanup cases above
    // remain independent security assertions; no replay/discard is attempted.
    std::string after;
    CK_CHECK(ck::io::is_regular_file(journal_path));
    CK_CHECK(ck::io::read_file(journal_path, after, torn.size() + 1));
    CK_CHECK(after == before);
}
#endif
