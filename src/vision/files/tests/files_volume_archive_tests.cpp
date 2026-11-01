#include "files_volume_archive.hpp"
#include "scratch_directory.hpp"

#include <cvision/testing/cktest.hpp>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace ck::files;
using namespace ck::vision;

NativeAbsolutePath address(const std::filesystem::path &path) {
    auto value = NativeAbsolutePath::from_host(path);
    if (!value.value())
        throw std::runtime_error("volume archive fixture address unavailable");
    return std::move(value).take_value();
}

FilesVolumeRecord source(FilesVolumeArchiveSupport support = FilesVolumeArchiveSupport::LiveReadable) {
    return {"Fixture volume", "/fixture", "fixturefs", 64, 32, false, "fixture-volume",
            {support, support == FilesVolumeArchiveSupport::SnapshotPreferred, {}}, {}};
}

FilesVolumeArchiveDocument document(FilesVolumeArchiveSupport support = FilesVolumeArchiveSupport::LiveReadable) {
    FilesVolumeArchiveDocument result;
    result.source = source(support);
    result.entries.push_back({"root", FilesVolumeArchiveEntryKind::Directory, 0, {}, {}, 0});
    result.entries.push_back({"sparse.bin", FilesVolumeArchiveEntryKind::RegularFile, 16,
                              {{0, {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}}},
                               {8, {std::byte{'z'}}}},
                              {}, 0});
    result.entries.push_back({"link", FilesVolumeArchiveEntryKind::Symlink, 0, {}, "sparse.bin", 0});
    return result;
}

NativeAbsolutePath journal_root(const std::filesystem::path &base) {
    const auto root = base / "journals";
    std::filesystem::create_directories(root);
    auto result = address(root);
    if (!prepare_native_operation_journal_root(result).value())
        throw std::runtime_error("volume archive journal root unavailable");
    return result;
}

OperationJournalId journal_id(std::uint64_t value) {
    OperationJournalId result;
    for (unsigned byte = 0; byte != sizeof(value); ++byte)
        result.bytes[byte] = std::byte{static_cast<unsigned char>(value >> (byte * 8U))};
    return result;
}

template <class T>
void append_le(std::vector<std::byte> &out, T value) {
    for (std::size_t index = 0; index != sizeof(T); ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

void append_string(std::vector<std::byte> &out, std::string_view value) {
    append_le<std::uint64_t>(out, value.size());
    for (const auto byte : value)
        out.push_back(std::byte{static_cast<unsigned char>(byte)});
}

std::vector<std::byte> journal_evidence(std::uint64_t transaction, std::string_view identity = "fixture-volume",
                                        std::string_view mount = "/fixture",
                                        std::string_view archive = "fixture.ckarchive") {
    std::vector<std::byte> result;
    append_le<std::uint64_t>(result, transaction);
    append_string(result, identity);
    append_string(result, mount);
    append_string(result, archive);
    return result;
}

auto open_journal() {
    return [](const NativeAbsolutePath &root, const NativeName &name, bool create_new) {
        return open_native_operation_journal_file(root, name, create_new);
    };
}

std::optional<NativeName> archive_journal_name(std::string_view suffix) {
    const std::string value = ".ck-volume-archive-" + std::string(suffix);
#if defined(_WIN32)
    return NativeName::from_windows(std::u16string(value.begin(), value.end()));
#else
    return NativeName::from_posix(value);
#endif
}

} // namespace

CK_TEST(volume_enumerator_publishes_native_revalidation_identity) {
    const auto volumes = enumerate_native_volumes();
    CK_CHECK(volumes.value());
    CK_CHECK(!volumes.value()->empty());
    for (const auto &volume : *volumes.value()) {
        CK_CHECK(!volume.mount_point.empty());
        CK_CHECK(!volume.identity.empty());
#if !defined(_WIN32)
        // POSIX source paths can be reused after unmount/remount. They must
        // not be the complete identity handed to a future archive worker.
        CK_CHECK(volume.identity != volume.mount_point);
#endif
    }
}

CK_TEST(volume_enumerator_reports_native_archive_capability) {
    const auto volumes = enumerate_native_volumes();
    CK_CHECK(volumes.value());
    bool live_source = false;
    for (const auto &volume : *volumes.value()) {
        CK_CHECK(!volume.archive.diagnostic.empty());
#if defined(_WIN32)
        CK_CHECK(!volume.native_mount_units.empty());
        if (volume.archive.support == FilesVolumeArchiveSupport::LiveReadable) {
            live_source = true;
            CK_CHECK(volume.archive.diagnostic.find("Live source traversal qualified") != std::string::npos);
        }
#else
        if (volume.mount_point == "/") {
            live_source = volume.archive.support == FilesVolumeArchiveSupport::LiveReadable;
            CK_CHECK(volume.archive.diagnostic.find("Live source traversal qualified") != std::string::npos);
        }
#endif
    }
    // A locked-down test host may expose only denied or unsupported mounts;
    // capability reporting itself must still be complete and non-silent.
    CK_CHECK(live_source || !volumes.value()->empty());
}

CK_TEST(volume_archive_writer_preserves_sparse_extents_and_publishes_without_replacement) {
    ck::testing::ScratchDirectory scratch("files-volume-archive", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesVolumeArchiveWriteRequest request{document(), scratch.path(), "fixture.ckarchive", 7, {}, {}};
    const auto written = write_volume_archive(request);
    CK_CHECK(written.value());
    CK_CHECK(written.value()->entry_count == 3);
    CK_CHECK(written.value()->data_bytes == 4);
    CK_CHECK(written.value()->publication == FilesVolumeArchivePublication::Durable);
    CK_CHECK(written.value()->digests_verified);
    CK_CHECK(std::filesystem::file_size(written.value()->published_path) == written.value()->logical_size);

    std::ifstream archive(written.value()->published_path, std::ios::binary);
    archive.seekg(56 + 1, std::ios::beg); // Header plus the first manifest byte.
    CK_CHECK(archive.good());
    std::vector<char> all(static_cast<std::size_t>(std::filesystem::file_size(written.value()->published_path)));
    archive.seekg(0);
    archive.read(all.data(), static_cast<std::streamsize>(all.size()));
    CK_CHECK(archive.good() || archive.eof());

    const auto second = write_volume_archive(request);
    CK_CHECK(!second.value());
    CK_CHECK(second.state() == ObservationState::Changed);
}

CK_TEST(volume_archive_writer_refuses_unknown_capability_and_unsafe_entry_paths) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-refusal", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesVolumeArchiveWriteRequest unknown{document(FilesVolumeArchiveSupport::Unknown), scratch.path(),
                                           "unknown.ckarchive", 1, {}, {}};
    const auto refused = write_volume_archive(unknown);
    CK_CHECK(!refused.value());
    CK_CHECK(refused.state() == ObservationState::Unknown);

    auto unsafe_document = document();
    unsafe_document.entries[1].path = "../escape";
    FilesVolumeArchiveWriteRequest unsafe{std::move(unsafe_document), scratch.path(), "unsafe.ckarchive", 2, {}, {}};
    const auto unsafe_result = write_volume_archive(unsafe);
    CK_CHECK(!unsafe_result.value());
    CK_CHECK(unsafe_result.state() == ObservationState::Unsupported);
}

CK_TEST(volume_archive_review_reports_sparse_shape_and_binds_default_name) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-review", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    auto review = FilesVolumeArchiveReview::create(document(), address(scratch.path()), {},
                                                   FilesVolumeArchiveDestinationState::Ready);
    CK_CHECK(review.value());
    const auto &model = *review.value();
    CK_CHECK(model->archive_name() == "Fixture_volume.ckarchive");
    CK_CHECK(model->summary().entries == 3);
    CK_CHECK(model->summary().regular_files == 1);
    CK_CHECK(model->summary().sparse_files == 1);
    CK_CHECK(model->summary().logical_bytes == 16);
    CK_CHECK(model->summary().data_bytes == 4);
    CK_CHECK(!model->requires_loss_consent());
    const auto approval = model->approve(FilesVolumeArchiveReviewDecision::Proceed);
    CK_CHECK(approval.value());
    CK_CHECK(!approval.value()->continued_with_listed_losses);
    CK_CHECK(approval.value()->review == model);
}

CK_TEST(volume_archive_review_requires_explicit_loss_consent_and_writer_refuses_unreviewed_losses) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-review-loss", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    auto reviewed_document = document();
    reviewed_document.losses.push_back({1, "sparse.bin", FilesVolumeArchiveLossKind::Metadata, false,
                                        "portable timestamps are not encoded"});
    auto review = FilesVolumeArchiveReview::create(std::move(reviewed_document), address(scratch.path()),
                                                   "loss.ckarchive", FilesVolumeArchiveDestinationState::Ready);
    CK_CHECK(review.value());
    const auto &model = *review.value();
    CK_CHECK(model->summary().losses == 1);
    CK_CHECK(model->summary().hard_losses == 0);
    const auto refused = model->approve(FilesVolumeArchiveReviewDecision::Proceed);
    CK_CHECK(!refused.value());
    const std::size_t loss = 0;
    const auto approved = model->approve(FilesVolumeArchiveReviewDecision::ContinueWithListedLosses,
                                         std::span<const std::size_t>(&loss, 1));
    CK_CHECK(approved.value());
    CK_CHECK(approved.value()->continued_with_listed_losses);

    auto unreviewed = model->document();
    FilesVolumeArchiveWriteRequest write_request{unreviewed, scratch.path(), "loss.ckarchive", 17, {}, {}};
    const auto write_result = write_volume_archive(write_request);
    CK_CHECK(!write_result.value());
    CK_CHECK(write_result.state() == ObservationState::Unknown);
}

CK_TEST(volume_archive_review_refuses_unrepresentable_windows_path_encoding) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-review-encoding", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    auto value = document();
    value.path_encoding = NameEncoding::WindowsUtf16;
    value.entries[0].path = "x"; // odd byte count cannot be UTF-16 native units
    const auto review = FilesVolumeArchiveReview::create(std::move(value), address(scratch.path()),
                                                         "encoding.ckarchive", FilesVolumeArchiveDestinationState::Ready);
    CK_CHECK(!review.value());
    CK_CHECK(review.state() == ObservationState::Unsupported);
}

CK_TEST(volume_archive_planning_service_returns_review_without_writing) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-planning", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesVolumeArchivePlanningService service;
    FilesVolumeArchivePlanningRequest request{
        source(), address(scratch.path()), "planned.ckarchive", FilesVolumeArchiveConsistency::Live,
        [](std::stop_token) { return Observation<FilesVolumeArchiveDocument>::available(document()); }};
    const auto submitted = service.request(std::move(request));
    CK_CHECK(submitted.state == FilesVolumeArchivePlanningSubmission::Accepted);
    std::optional<FilesVolumeArchivePlanningReply> reply;
    for (int i = 0; i != 500 && !reply; ++i) {
        reply = service.take();
        if (!reply)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(reply && reply->state == FilesVolumeArchivePlanningState::Ready);
    CK_CHECK(reply->review);
    if (reply && reply->review)
        CK_CHECK(reply->review->archive_name() == "planned.ckarchive");
    CK_CHECK(!std::filesystem::exists(scratch.path() / "planned.ckarchive"));
    service.request_stop();
    service.join();
}

CK_TEST(volume_archive_service_keeps_build_and_publication_on_worker) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-service", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesVolumeArchiveService service;
    const auto destination = address(scratch.path());
    FilesVolumeArchiveRequest request{source(), destination, "worker.ckarchive",
                                      FilesVolumeArchiveConsistency::Live, 11,
                                      [](std::stop_token) { return Observation<FilesVolumeArchiveDocument>::available(document()); },
                                      journal_root(scratch.path()), journal_id(11), {}, open_journal()};
    const auto submitted = service.request(std::move(request));
    CK_CHECK(submitted.state == FilesVolumeArchiveSubmission::Accepted);
    std::optional<FilesVolumeArchiveReply> reply;
    for (int i = 0; i != 500 && !reply; ++i) {
        reply = service.take();
        if (!reply)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(reply.has_value());
    CK_CHECK(reply->state == FilesVolumeArchiveState::Complete);
    CK_CHECK(reply->result && std::filesystem::exists(reply->result->published_path));
    std::size_t journal_files = 0;
    for (const auto &entry : std::filesystem::directory_iterator(scratch.path() / "journals")) {
        if (entry.is_regular_file() && entry.path().filename().string().starts_with(".ck-volume-archive-")) {
            ++journal_files;
            std::ifstream stream(entry.path(), std::ios::binary | std::ios::ate);
            CK_CHECK(stream.good());
            if (stream) {
                const auto size = stream.tellg();
                CK_CHECK(size > 0);
                if (size > 0) {
                    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
                    stream.seekg(0);
                    stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    CK_CHECK(stream.good());
                    auto decoded = decode_operation_journal(bytes);
                    CK_CHECK(decoded.value());
                    if (decoded.value()) {
                        const auto &records = decoded.value()->journal->records();
                        CK_CHECK(records.size() == 7);
                        if (records.size() == 7) {
                            CK_CHECK(records.front().state == OperationJournalState::Planned);
                            CK_CHECK(records.back().state == OperationJournalState::Complete);
                        }
                        auto recovery = inspect_volume_archive_recovery(*decoded.value()->journal,
                                                                         decoded.value()->tail);
                        CK_CHECK(recovery.value());
                        if (recovery.value()) {
                            CK_CHECK(!recovery.value()->operation.requires_reconciliation);
                            CK_CHECK(recovery.value()->evidence.transaction_id == 11);
                            CK_CHECK(recovery.value()->evidence.archive_name == "worker.ckarchive");
#if defined(_WIN32)
                            std::string expected_destination;
                            for (const auto unit : *destination.windows_units()) {
                                expected_destination.push_back(static_cast<char>(unit & 0xff));
                                expected_destination.push_back(static_cast<char>(unit >> 8));
                            }
                            CK_CHECK(recovery.value()->evidence.destination_encoding == NameEncoding::WindowsUtf16);
                            const auto staging_name = NativeName::from_windows(u".worker.ckarchive.cktmp-11");
#else
                            const auto expected_destination = *destination.posix_bytes();
                            CK_CHECK(recovery.value()->evidence.destination_encoding == NameEncoding::PosixBytes);
                            const auto staging_name = NativeName::from_posix(".worker.ckarchive.cktmp-11");
#endif
                            CK_CHECK(recovery.value()->evidence.destination_native_path == expected_destination);
                            const auto expected_staging_address = destination.child_address(*staging_name);
                            CK_CHECK(expected_staging_address.value());
                            if (expected_staging_address.value()) {
                                CK_CHECK(recovery.value()->evidence.staging_encoding ==
                                         expected_staging_address.value()->encoding());
#if defined(_WIN32)
                                std::string expected_staging;
                                for (const auto unit : *expected_staging_address.value()->windows_units()) {
                                    expected_staging.push_back(static_cast<char>(unit & 0xff));
                                    expected_staging.push_back(static_cast<char>(unit >> 8));
                                }
#else
                                const auto expected_staging = *expected_staging_address.value()->posix_bytes();
#endif
                                CK_CHECK(recovery.value()->evidence.staging_native_path == expected_staging);
                            }
                            CK_CHECK(recovery.value()->evidence.staging_digest.has_value());
                        }
                    }
                }
            }
        }
    }
    CK_CHECK(journal_files == 1);
    service.request_stop();
    service.join();
}

CK_TEST(volume_archive_recovery_requires_review_for_unfinished_or_untrusted_journal) {
    auto created = OperationJournal::create(FileOperation::Copy, journal_id(21));
    CK_CHECK(created.value());
    if (!created.value())
        return;
    const auto evidence = journal_evidence(21, "fixture-volume", "/fixture", "recovery.ckarchive");
    CK_CHECK((*created.value())->append({1, 0, OperationJournalState::Planned, evidence}).value());
    CK_CHECK((*created.value())->append({2, 0, OperationJournalState::Prepared, evidence}).value());
    CK_CHECK((*created.value())->append({3, 0, OperationJournalState::Copied, evidence}).value());
    auto recovery = inspect_volume_archive_recovery(**created.value());
    CK_CHECK(recovery.value());
    if (recovery.value()) {
        CK_CHECK(recovery.value()->operation.requires_reconciliation);
        CK_CHECK(recovery.value()->operation.items.size() == 1);
        CK_CHECK(recovery.value()->operation.items[0].action == OperationRecoveryAction::InspectUnpublishedStaging);
    }

    auto tail = UnavailableObservation{ObservationState::Unknown,
                                       {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult),
                                        "test.truncated_tail"}};
    auto tailed = inspect_volume_archive_recovery(**created.value(), tail);
    CK_CHECK(tailed.value());
    CK_CHECK(tailed.value() && tailed.value()->operation.has_untrusted_tail);
}

CK_TEST(volume_archive_recovery_rejects_malformed_or_changed_identity_evidence) {
    auto malformed = OperationJournal::create(FileOperation::Copy, journal_id(22));
    CK_CHECK(malformed.value());
    if (malformed.value()) {
        CK_CHECK((*malformed.value())->append({1, 0, OperationJournalState::Planned, {std::byte{1}}}).value());
        const auto refused = inspect_volume_archive_recovery(**malformed.value());
        CK_CHECK(!refused.value());
        CK_CHECK(refused.state() == ObservationState::Unknown);
    }

    auto changed = OperationJournal::create(FileOperation::Copy, journal_id(23));
    CK_CHECK(changed.value());
    if (changed.value()) {
        CK_CHECK((*changed.value())->append({1, 0, OperationJournalState::Planned, journal_evidence(23)}).value());
        CK_CHECK((*changed.value())->append({2, 0, OperationJournalState::Prepared,
                                          journal_evidence(23, "different-volume")}).value());
        const auto refused = inspect_volume_archive_recovery(**changed.value());
        CK_CHECK(!refused.value());
        CK_CHECK(refused.state() == ObservationState::Changed);
    }
}

CK_TEST(volume_archive_recovery_scan_preserves_native_names_and_reports_unfinished_journals) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-recovery-scan", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = journal_root(scratch.path());
    const auto name = archive_journal_name("00000000000000000000000000000031");
    CK_CHECK(name.has_value());
    if (!name)
        return;
    auto file = open_native_operation_journal_file(root, *name, true);
    CK_CHECK(file.value());
    if (!file.value())
        return;
    auto journal = OperationJournal::create(FileOperation::Copy, journal_id(31));
    CK_CHECK(journal.value());
    if (!journal.value())
        return;
    const auto evidence = journal_evidence(31, "fixture-volume", "/fixture", "scan.ckarchive");
    auto journal_file = std::move(file).take_value();
    CK_CHECK(append_operation_journal_file(*journal_file, **journal.value(),
                                           {1, 0, OperationJournalState::Planned, evidence}).value());
    journal_file.reset();
    const auto scan = discover_volume_archive_recovery(root);
    CK_CHECK(scan.value());
    if (scan.value()) {
        CK_CHECK(scan.value()->journals.size() == 1);
        CK_CHECK(scan.value()->requires_reconciliation);
        CK_CHECK(scan.value()->journals[0].native_name.has_value());
        CK_CHECK(scan.value()->journals[0].report.has_value());
        CK_CHECK(scan.value()->journals[0].report->operation.requires_reconciliation);
        CK_CHECK(scan.value()->journals[0].display_name.find(".ck-volume-archive-") == 0);
    }
}

CK_TEST(volume_archive_recovery_service_scans_off_ui_thread_and_rejects_invalid_limits) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-recovery-service", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = journal_root(scratch.path());
    std::atomic_bool woke = false;
    FilesVolumeArchiveRecoveryService service([&woke] { woke.store(true); });
    FilesVolumeArchiveRecoveryLimits invalid;
    invalid.max_journals = 0;
    CK_CHECK(service.request(root, invalid) == FilesVolumeArchiveRecoverySubmission::Invalid);
    CK_CHECK(service.request(root) == FilesVolumeArchiveRecoverySubmission::Accepted);
    CK_CHECK(service.request(root) == FilesVolumeArchiveRecoverySubmission::Busy);

    std::optional<FilesVolumeArchiveRecoveryReply> reply;
    for (int i = 0; i != 500 && !reply; ++i) {
        reply = service.take();
        if (!reply)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(reply && reply->state == FilesVolumeArchiveRecoveryState::Ready);
    CK_CHECK(reply && reply->scan && reply->scan->journals.empty());
    CK_CHECK(woke.load());
    service.request_stop();
    service.join();
    CK_CHECK(!service.running());
}

CK_TEST(volume_archive_service_does_not_accept_capability_upgrade_from_builder) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-revalidation", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesVolumeArchiveService service;
    auto request_source = source(FilesVolumeArchiveSupport::Unknown);
    FilesVolumeArchiveRequest request{
        request_source, address(scratch.path()), "revalidation.ckarchive", FilesVolumeArchiveConsistency::Live, 13,
        [](std::stop_token) {
            return Observation<FilesVolumeArchiveDocument>::available(document(FilesVolumeArchiveSupport::LiveReadable));
        }, journal_root(scratch.path()), journal_id(13), {}, open_journal()};
    const auto submitted = service.request(std::move(request));
    CK_CHECK(submitted.state == FilesVolumeArchiveSubmission::Accepted);
    std::optional<FilesVolumeArchiveReply> reply;
    for (int i = 0; i != 500 && !reply; ++i) {
        reply = service.take();
        if (!reply)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(reply && reply->state == FilesVolumeArchiveState::Failed);
    CK_CHECK(reply->error && reply->error->state == ObservationState::Changed);
    CK_CHECK(!std::filesystem::exists(scratch.path() / "revalidation.ckarchive"));
    service.request_stop();
    service.join();
}

CK_TEST(volume_archive_service_cancels_queued_request_before_native_build) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-cancel", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    FilesVolumeArchiveService service;
    FilesVolumeArchiveRequest request{source(), address(scratch.path()), "cancel.ckarchive",
                                      FilesVolumeArchiveConsistency::Live, 12,
                                      [&mutex, &changed, &entered](std::stop_token token) {
                                          {
                                              std::lock_guard lock(mutex);
                                              entered = true;
                                              changed.notify_all();
                                          }
                                          while (!token.stop_requested())
                                              std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                          return Observation<FilesVolumeArchiveDocument>::unavailable(
                                              ObservationState::Unknown,
                                              {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead),
                                               "volume_archive.cancelled"});
                                      }, journal_root(scratch.path()), journal_id(12), {}, open_journal()};
    const auto submitted = service.request(std::move(request));
    CK_CHECK(submitted.state == FilesVolumeArchiveSubmission::Accepted);
    {
        std::unique_lock lock(mutex);
        CK_CHECK(changed.wait_for(lock, std::chrono::seconds(5), [&entered] { return entered; }));
    }
    CK_CHECK(service.cancel(submitted.ticket));
    std::optional<FilesVolumeArchiveReply> reply;
    for (int i = 0; i != 500 && !reply; ++i) {
        reply = service.take();
        if (!reply)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(reply && reply->state == FilesVolumeArchiveState::Cancelled);
    service.request_stop();
    service.join();
}
