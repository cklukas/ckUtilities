#include "files_volume_archive.hpp"
#include "scratch_directory.hpp"

#include <cvision/testing/cktest.hpp>

#include <chrono>
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
            {support, support == FilesVolumeArchiveSupport::SnapshotPreferred, {}}};
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

CK_TEST(volume_enumerator_qualifies_only_readable_posix_archive_sources) {
    const auto volumes = enumerate_native_volumes();
    CK_CHECK(volumes.value());
#if defined(_WIN32)
    for (const auto &volume : *volumes.value()) {
        CK_CHECK(volume.archive.support == FilesVolumeArchiveSupport::Unsupported);
        CK_CHECK(volume.archive.diagnostic.find("UTF-16") != std::string::npos);
    }
#else
    bool live_source = false;
    for (const auto &volume : *volumes.value()) {
        if (volume.mount_point == "/") {
            live_source = volume.archive.support == FilesVolumeArchiveSupport::LiveReadable;
            CK_CHECK(volume.archive.diagnostic.find("Live source traversal qualified") != std::string::npos);
        }
    }
    CK_CHECK(live_source);
#endif
}

CK_TEST(volume_archive_writer_preserves_sparse_extents_and_publishes_without_replacement) {
    ck::testing::ScratchDirectory scratch("files-volume-archive", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesVolumeArchiveWriteRequest request{document(), scratch.path(), "fixture.ckarchive", 7};
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
                                           "unknown.ckarchive", 1};
    const auto refused = write_volume_archive(unknown);
    CK_CHECK(!refused.value());
    CK_CHECK(refused.state() == ObservationState::Unknown);

    auto unsafe_document = document();
    unsafe_document.entries[1].path = "../escape";
    FilesVolumeArchiveWriteRequest unsafe{std::move(unsafe_document), scratch.path(), "unsafe.ckarchive", 2};
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
    FilesVolumeArchiveWriteRequest write_request{unreviewed, scratch.path(), "loss.ckarchive", 17};
    const auto write_result = write_volume_archive(write_request);
    CK_CHECK(!write_result.value());
    CK_CHECK(write_result.state() == ObservationState::Unknown);
}

CK_TEST(volume_archive_review_refuses_unrepresentable_windows_path_encoding) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-review-encoding", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    auto value = document();
    value.path_encoding = NameEncoding::WindowsUtf16;
    const auto review = FilesVolumeArchiveReview::create(std::move(value), address(scratch.path()),
                                                         "encoding.ckarchive", FilesVolumeArchiveDestinationState::Ready);
    CK_CHECK(!review.value());
    CK_CHECK(review.state() == ObservationState::Unsupported);
}

CK_TEST(volume_archive_service_keeps_build_and_publication_on_worker) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-service", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesVolumeArchiveService service;
    FilesVolumeArchiveRequest request{source(), address(scratch.path()), "worker.ckarchive",
                                      FilesVolumeArchiveConsistency::Live, 11,
                                      [](std::stop_token) { return Observation<FilesVolumeArchiveDocument>::available(document()); }};
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
    service.request_stop();
    service.join();
}

CK_TEST(volume_archive_service_does_not_accept_capability_upgrade_from_builder) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-revalidation", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    FilesVolumeArchiveService service;
    auto request_source = source(FilesVolumeArchiveSupport::Unknown);
    FilesVolumeArchiveRequest request{
        request_source, address(scratch.path()), "revalidation.ckarchive", FilesVolumeArchiveConsistency::Live, 13,
        [](std::stop_token) {
            return Observation<FilesVolumeArchiveDocument>::available(document(FilesVolumeArchiveSupport::LiveReadable));
        }};
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
                                      }};
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
