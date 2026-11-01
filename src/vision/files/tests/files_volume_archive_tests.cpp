#include "files_volume_archive.hpp"
#include "sha256.hpp"
#include "scratch_directory.hpp"

#include <cvision/testing/cktest.hpp>

#include <chrono>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <condition_variable>
#include <cstring>
#include <limits>
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

void append_native_path(std::vector<std::byte> &out, const NativeAbsolutePath &path) {
    append_le<std::uint8_t>(out, static_cast<std::uint8_t>(path.encoding()));
    if (path.posix_bytes()) {
        append_string(out, *path.posix_bytes());
        return;
    }
    std::string bytes;
    for (const auto unit : *path.windows_units()) {
        bytes.push_back(static_cast<char>(unit & 0xff));
        bytes.push_back(static_cast<char>(unit >> 8));
    }
    append_string(out, bytes);
}

std::vector<std::byte> archive_recovery_evidence(
    std::uint64_t transaction, const NativeAbsolutePath &destination, const NativeAbsolutePath &staging,
    std::string_view archive, std::optional<std::array<std::byte, 32>> digest = {}) {
    std::vector<std::byte> result;
    append_le<std::uint64_t>(result, transaction);
    append_string(result, "fixture-volume");
    append_string(result, "/fixture");
    append_string(result, archive);
    append_native_path(result, destination);
    append_le<std::uint8_t>(result, digest ? 3U : 1U);
    append_native_path(result, staging);
    if (digest)
        result.insert(result.end(), digest->begin(), digest->end());
    return result;
}

std::array<std::byte, 32> digest_bytes(std::span<const std::byte> bytes) {
    ck::files::Sha256 digest;
    digest.update(bytes);
    return digest.digest();
}

std::uint32_t little_u32(std::span<const std::byte> bytes) {
    std::uint32_t result = 0;
    for (unsigned index = 0; index != 4; ++index)
        result |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[index])) << (index * 8);
    return result;
}

void write_little_u32(std::vector<std::byte> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index)
        bytes[offset + index] = std::byte{static_cast<unsigned char>((value >> (index * 8)) & 0xffU)};
}

std::uint32_t journal_checksum(std::span<const std::byte> bytes) {
    std::uint32_t result = 2166136261U;
    for (const auto byte : bytes) {
        result ^= std::to_integer<unsigned char>(byte);
        result *= 16777619U;
    }
    return result;
}

void refresh_journal_frame_checksum(std::vector<std::byte> &bytes, std::size_t offset) {
    const auto body_size = static_cast<std::size_t>(little_u32(std::span<const std::byte>(bytes).subspan(offset + 5, 4)));
    const auto body = std::span<const std::byte>(bytes).subspan(offset + 9, body_size);
    write_little_u32(bytes, offset + 9 + body_size, journal_checksum(body));
}

std::size_t journal_frame_size(std::span<const std::byte> bytes, std::size_t offset) {
    return 9 + static_cast<std::size_t>(little_u32(bytes.subspan(offset + 5, 4))) + sizeof(std::uint32_t);
}

template <class T>
bool read_archive_le(std::span<const std::byte> bytes, std::size_t &offset, T &value) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(T))
        return false;
    value = 0;
    for (std::size_t index = 0; index != sizeof(T); ++index)
        value |= static_cast<T>(std::to_integer<unsigned char>(bytes[offset + index])) << (index * 8U);
    offset += sizeof(T);
    return true;
}

bool skip_archive_string(std::span<const std::byte> bytes, std::size_t &offset) {
    std::uint64_t length = 0;
    if (!read_archive_le(bytes, offset, length) || length > bytes.size() - offset)
        return false;
    offset += static_cast<std::size_t>(length);
    return true;
}

struct ParsedArchiveExtent final {
    std::uint64_t source_offset = 0;
    std::uint64_t archive_offset = 0;
    std::uint64_t length = 0;
};

struct ParsedArchiveEntry final {
    std::uint8_t kind = 0;
    std::uint64_t logical_size = 0;
    std::vector<ParsedArchiveExtent> extents;
};

struct ParsedArchive final {
    std::uint64_t data_offset = 0;
    std::uint64_t data_size = 0;
    std::vector<ParsedArchiveEntry> entries;
};

std::optional<ParsedArchive> parse_archive_fixture(std::span<const std::byte> bytes) {
    if (bytes.size() < 56 ||
        std::memcmp(bytes.data(), "CKARV01\0", 8) != 0)
        return std::nullopt;
    std::size_t offset = 8;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint64_t manifest_size = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t data_size = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t reserved = 0;
    if (!read_archive_le(bytes, offset, version) || !read_archive_le(bytes, offset, flags) ||
        !read_archive_le(bytes, offset, manifest_size) || !read_archive_le(bytes, offset, data_offset) ||
        !read_archive_le(bytes, offset, data_size) || !read_archive_le(bytes, offset, entry_count) ||
        !read_archive_le(bytes, offset, reserved) || version != 2 || flags != 0 || reserved != 0 ||
        manifest_size > bytes.size() - 56 || data_offset != 56 + manifest_size ||
        data_size > bytes.size() - data_offset || data_offset + data_size != bytes.size() ||
        entry_count > bytes.size())
        return std::nullopt;

    const auto manifest_end = static_cast<std::size_t>(data_offset);
    ParsedArchive result{data_offset, data_size, {}};
    result.entries.reserve(static_cast<std::size_t>(entry_count));
    if (offset > manifest_end)
        return std::nullopt;
    for (std::size_t index = 0; index != 4; ++index)
        if (!skip_archive_string(bytes.subspan(0, manifest_end), offset))
            return std::nullopt;
    std::uint8_t consistency = 0;
    std::uint8_t path_encoding = 0;
    if (!read_archive_le(bytes, offset, consistency) || !read_archive_le(bytes, offset, path_encoding) ||
        offset > manifest_end || manifest_end - offset < 6)
        return std::nullopt;
    offset += 6;
    for (std::uint64_t index = 0; index != entry_count; ++index) {
        ParsedArchiveEntry entry;
        std::uint8_t padding = 0;
        if (!read_archive_le(bytes, offset, entry.kind))
            return std::nullopt;
        for (int pad = 0; pad != 7; ++pad)
            if (!read_archive_le(bytes, offset, padding))
                return std::nullopt;
        std::uint64_t hard_link_group = 0;
        if (!read_archive_le(bytes, offset, entry.logical_size) ||
            !read_archive_le(bytes, offset, hard_link_group) ||
            !skip_archive_string(bytes.subspan(0, manifest_end), offset) ||
            !skip_archive_string(bytes.subspan(0, manifest_end), offset))
            return std::nullopt;
        std::uint64_t extent_count = 0;
        if (!read_archive_le(bytes, offset, extent_count) || extent_count > manifest_end)
            return std::nullopt;
        entry.extents.reserve(static_cast<std::size_t>(extent_count));
        for (std::uint64_t extent_index = 0; extent_index != extent_count; ++extent_index) {
            ParsedArchiveExtent extent;
            if (!read_archive_le(bytes, offset, extent.source_offset) ||
                !read_archive_le(bytes, offset, extent.archive_offset) ||
                !read_archive_le(bytes, offset, extent.length))
                return std::nullopt;
            entry.extents.push_back(extent);
        }
        result.entries.push_back(std::move(entry));
    }
    std::uint64_t loss_count = 0;
    if (!read_archive_le(bytes, offset, loss_count) || loss_count > manifest_end)
        return std::nullopt;
    for (std::uint64_t loss_index = 0; loss_index != loss_count; ++loss_index) {
        std::uint64_t entry_index = 0;
        std::uint8_t loss_kind = 0;
        std::uint8_t hard_failure = 0;
        std::uint8_t padding = 0;
        if (!read_archive_le(bytes, offset, entry_index) || !read_archive_le(bytes, offset, loss_kind) ||
            !read_archive_le(bytes, offset, hard_failure))
            return std::nullopt;
        for (int pad = 0; pad != 6; ++pad)
            if (!read_archive_le(bytes, offset, padding))
                return std::nullopt;
        if (!skip_archive_string(bytes.subspan(0, manifest_end), offset) ||
            !skip_archive_string(bytes.subspan(0, manifest_end), offset))
            return std::nullopt;
    }
    if (offset != manifest_end)
        return std::nullopt;
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

    const auto parsed = parse_archive_fixture(std::as_bytes(std::span<const char>(all.data(), all.size())));
    CK_CHECK(parsed.has_value());
    if (parsed) {
        CK_CHECK(parsed->data_size == 16);
        CK_CHECK(parsed->entries.size() == 3);
        if (parsed->entries.size() == 3) {
            CK_CHECK(parsed->entries[0].logical_size == 0);
            CK_CHECK(parsed->entries[0].extents.empty());
            CK_CHECK(parsed->entries[1].kind == 2);
            CK_CHECK(parsed->entries[1].logical_size == 16);
            CK_CHECK(parsed->entries[1].extents.size() == 2);
            if (parsed->entries[1].extents.size() == 2) {
                CK_CHECK(parsed->entries[1].extents[0].source_offset == 0);
                CK_CHECK(parsed->entries[1].extents[0].archive_offset == 0);
                CK_CHECK(parsed->entries[1].extents[0].length == 3);
                CK_CHECK(parsed->entries[1].extents[1].source_offset == 8);
                CK_CHECK(parsed->entries[1].extents[1].archive_offset == 8);
                CK_CHECK(parsed->entries[1].extents[1].length == 1);
            }
            CK_CHECK(parsed->entries[2].kind == 3);
            CK_CHECK(parsed->entries[2].extents.empty());
        }
        const auto data_begin = static_cast<std::size_t>(parsed->data_offset);
        CK_CHECK(static_cast<unsigned char>(all[data_begin + 0]) == 'a');
        CK_CHECK(static_cast<unsigned char>(all[data_begin + 1]) == 'b');
        CK_CHECK(static_cast<unsigned char>(all[data_begin + 2]) == 'c');
        for (std::size_t index = 3; index != 8; ++index)
            CK_CHECK(static_cast<unsigned char>(all[data_begin + index]) == 0);
        CK_CHECK(static_cast<unsigned char>(all[data_begin + 8]) == 'z');
        for (std::size_t index = 9; index != 16; ++index)
            CK_CHECK(static_cast<unsigned char>(all[data_begin + index]) == 0);
    }

    const auto second = write_volume_archive(request);
    CK_CHECK(!second.value());
    CK_CHECK(second.state() == ObservationState::Changed);
}

CK_TEST(volume_archive_writer_qualifies_material_sparse_allocation) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-material-sparse", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    auto sparse_document = document();
    constexpr std::uint64_t logical_bytes = 4 * 1024 * 1024;
    sparse_document.entries[1].logical_size = logical_bytes;
    sparse_document.entries[1].extents = {
        {0, {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}}},
        {logical_bytes - 1, {std::byte{'z'}}}};
    FilesVolumeArchiveWriteRequest request{std::move(sparse_document), scratch.path(), "material.ckarchive", 8,
                                           {}, {}};
    const auto written = write_volume_archive(request);
    if (written.value()) {
        CK_CHECK(written.value()->data_bytes == 4);
        CK_CHECK(written.value()->logical_size > logical_bytes);
        CK_CHECK(std::filesystem::file_size(written.value()->published_path) == written.value()->logical_size);
    } else {
        CK_CHECK(written.state() == ObservationState::Unsupported);
        if (written.unavailable_detail())
            CK_CHECK(written.unavailable_detail()->error.operation == "volume_archive.sparse_allocation");
        CK_CHECK(!std::filesystem::exists(scratch.path() / "material.ckarchive"));
    }
}

CK_TEST(volume_archive_writer_faults_at_each_journal_boundary_without_false_completion) {
    const std::array<OperationJournalState, 4> boundaries{
        OperationJournalState::Copied,
        OperationJournalState::Verified,
        OperationJournalState::CommitIntended,
        OperationJournalState::DestinationCommitted};
    ck::testing::ScratchDirectory scratch("files-volume-archive-journal-faults", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    for (std::size_t index = 0; index != boundaries.size(); ++index) {
        const auto destination = scratch.path() / std::to_string(index);
        std::filesystem::create_directories(destination);
        const auto failed_state = boundaries[index];
        std::vector<OperationJournalState> observed;
        FilesVolumeArchiveWriteRequest request{
            document(), destination, "fault.ckarchive", static_cast<std::uint64_t>(100 + index),
            [&observed, failed_state](OperationJournalState state) {
                observed.push_back(state);
                if (state == failed_state)
                    return Observation<bool>::unavailable(
                        ObservationState::Failed,
                        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult),
                         "test.journal_fault"});
                return Observation<bool>::available(true);
            },
            {}};
        const auto result = write_volume_archive(request);
        CK_CHECK(std::find(observed.begin(), observed.end(), failed_state) != observed.end());
        const auto published = destination / "fault.ckarchive";
        if (failed_state == OperationJournalState::DestinationCommitted) {
            CK_CHECK(result.value());
            CK_CHECK(result.value() && result.value()->publication ==
                     FilesVolumeArchivePublication::PublishedDurabilityUnknown);
            CK_CHECK(std::filesystem::exists(published));
        } else {
            CK_CHECK(!result.value());
            CK_CHECK(!std::filesystem::exists(published));
            CK_CHECK(!std::filesystem::exists(destination / (".fault.ckarchive.cktmp-" +
                                               std::to_string(100 + index))));
        }
    }
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

    const auto destination = address(std::filesystem::current_path());
#if defined(_WIN32)
    const auto staging_name = NativeName::from_windows(u".recovery.ckarchive.cktmp-24");
    const auto changed_staging_name = NativeName::from_windows(u".recovery-other.ckarchive.cktmp-24");
#else
    const auto staging_name = NativeName::from_posix(".recovery.ckarchive.cktmp-24");
    const auto changed_staging_name = NativeName::from_posix(".recovery-other.ckarchive.cktmp-24");
#endif
    CK_CHECK(staging_name && changed_staging_name);
    if (!staging_name || !changed_staging_name)
        return;
    auto staging = destination.child_address(*staging_name);
    auto changed_staging = destination.child_address(*changed_staging_name);
    CK_CHECK(staging.value() && changed_staging.value());
    if (!staging.value() || !changed_staging.value())
        return;
    std::array<std::byte, 32> digest_a{};
    std::array<std::byte, 32> digest_b{};
    digest_b[0] = std::byte{1};
    const auto evidence_without_digest = archive_recovery_evidence(
        24, destination, *staging.value(), "recovery.ckarchive");
    const auto evidence_with_digest = archive_recovery_evidence(
        24, destination, *staging.value(), "recovery.ckarchive", digest_a);
    const auto evidence_changed_digest = archive_recovery_evidence(
        24, destination, *staging.value(), "recovery.ckarchive", digest_b);

    auto digest_removed = OperationJournal::create(FileOperation::Copy, journal_id(24));
    CK_CHECK(digest_removed.value());
    if (digest_removed.value()) {
        CK_CHECK((*digest_removed.value())->append({1, 0, OperationJournalState::Planned,
                                                     evidence_without_digest}).value());
        CK_CHECK((*digest_removed.value())->append({2, 0, OperationJournalState::Prepared,
                                                     evidence_without_digest}).value());
        CK_CHECK((*digest_removed.value())->append({3, 0, OperationJournalState::Copied,
                                                     evidence_with_digest}).value());
        CK_CHECK((*digest_removed.value())->append({4, 0, OperationJournalState::Verified,
                                                     evidence_without_digest}).value());
        const auto refused = inspect_volume_archive_recovery(**digest_removed.value());
        CK_CHECK(!refused.value());
        CK_CHECK(refused.state() == ObservationState::Changed);
    }

    auto digest_changed = OperationJournal::create(FileOperation::Copy, journal_id(25));
    CK_CHECK(digest_changed.value());
    if (digest_changed.value()) {
        const auto before = archive_recovery_evidence(25, destination, *staging.value(), "recovery.ckarchive");
        const auto first = archive_recovery_evidence(25, destination, *staging.value(), "recovery.ckarchive", digest_a);
        const auto changed_evidence = archive_recovery_evidence(
            25, destination, *staging.value(), "recovery.ckarchive", digest_b);
        CK_CHECK((*digest_changed.value())->append({1, 0, OperationJournalState::Planned, before}).value());
        CK_CHECK((*digest_changed.value())->append({2, 0, OperationJournalState::Prepared, before}).value());
        CK_CHECK((*digest_changed.value())->append({3, 0, OperationJournalState::Copied, first}).value());
        CK_CHECK((*digest_changed.value())->append({4, 0, OperationJournalState::Verified, changed_evidence}).value());
        const auto refused = inspect_volume_archive_recovery(**digest_changed.value());
        CK_CHECK(!refused.value());
        CK_CHECK(refused.state() == ObservationState::Changed);
    }

    auto staging_changed = OperationJournal::create(FileOperation::Copy, journal_id(26));
    CK_CHECK(staging_changed.value());
    if (staging_changed.value()) {
        const auto before = archive_recovery_evidence(26, destination, *staging.value(), "recovery.ckarchive");
        const auto first = archive_recovery_evidence(26, destination, *staging.value(), "recovery.ckarchive", digest_a);
        const auto changed_staging_evidence = archive_recovery_evidence(
            26, destination, *changed_staging.value(), "recovery.ckarchive", digest_a);
        CK_CHECK((*staging_changed.value())->append({1, 0, OperationJournalState::Planned, before}).value());
        CK_CHECK((*staging_changed.value())->append({2, 0, OperationJournalState::Prepared, before}).value());
        CK_CHECK((*staging_changed.value())->append({3, 0, OperationJournalState::Copied, first}).value());
        CK_CHECK((*staging_changed.value())->append({4, 0, OperationJournalState::Verified,
                                                      changed_staging_evidence}).value());
        const auto refused = inspect_volume_archive_recovery(**staging_changed.value());
        CK_CHECK(!refused.value());
        CK_CHECK(refused.state() == ObservationState::Changed);
    }
}

CK_TEST(volume_archive_journal_decoder_mutations_are_bounded_and_monotonic) {
    auto journal = OperationJournal::create(FileOperation::Copy, journal_id(51));
    CK_CHECK(journal.value());
    if (!journal.value())
        return;
    CK_CHECK((*journal.value())->append({1, 0, OperationJournalState::Planned, {std::byte{'p'}}}).value());
    CK_CHECK((*journal.value())->append({2, 0, OperationJournalState::Prepared, {std::byte{'r'}}}).value());
    CK_CHECK((*journal.value())->append({3, 0, OperationJournalState::Copied, {std::byte{'c'}}}).value());
    const auto encoded = encode_operation_journal(**journal.value());
    CK_CHECK(encoded.value());
    if (!encoded.value())
        return;

    const auto baseline = *encoded.value();
    OperationJournalCodecLimits bounded;
    bounded.max_bytes = baseline.size();
    auto check_mutation = [&](const std::vector<std::byte> &mutated, bool exact_baseline = false) {
        const auto decoded = decode_operation_journal(mutated, {}, bounded);
        if (exact_baseline) {
            CK_CHECK(decoded.value());
            if (decoded.value())
                CK_CHECK(!decoded.value()->tail.has_value());
            return;
        }
        // A mutation may invalidate the first frame or leave a verified prefix,
        // but it must never become a clean, longer journal.
        CK_CHECK(!decoded.value() || decoded.value()->tail.has_value() ||
                 decoded.value()->journal->records().size() < (*journal.value())->records().size());
        if (decoded.value())
            CK_CHECK(!decoded.value()->journal->records().empty());
    };

    check_mutation(baseline, true);
    for (std::size_t length = 0; length != baseline.size(); ++length) {
        std::vector<std::byte> truncated(baseline.begin(), baseline.begin() + length);
        check_mutation(truncated);
    }
    for (std::size_t index = 0; index != baseline.size(); ++index) {
        auto mutated = baseline;
        mutated[index] = std::byte{static_cast<unsigned char>(std::to_integer<unsigned char>(mutated[index]) ^ 0x5aU)};
        check_mutation(mutated);
    }

    const auto first_size = journal_frame_size(baseline, 0);
    const auto second_size = journal_frame_size(baseline, first_size);
    CK_CHECK(first_size < baseline.size());
    CK_CHECK(first_size + second_size <= baseline.size());
    if (first_size + second_size <= baseline.size()) {
        auto reordered = baseline;
        std::copy_n(baseline.begin() + first_size, second_size, reordered.begin());
        std::copy_n(baseline.begin(), first_size, reordered.begin() + second_size);
        check_mutation(reordered);
    }

    auto invalid_operation = baseline;
    invalid_operation[9] = std::byte{0xff};
    refresh_journal_frame_checksum(invalid_operation, 0);
    check_mutation(invalid_operation);

    auto invalid_sequence = baseline;
    write_little_u32(invalid_sequence, 9 + 17, 9);
    write_little_u32(invalid_sequence, 9 + 21, 0);
    refresh_journal_frame_checksum(invalid_sequence, 0);
    check_mutation(invalid_sequence);

    auto invalid_state = baseline;
    invalid_state[9 + 33] = std::byte{static_cast<unsigned char>(OperationJournalState::Verified)};
    refresh_journal_frame_checksum(invalid_state, 0);
    check_mutation(invalid_state);

    auto identity_changed = baseline;
    identity_changed[first_size + 9 + 1] = std::byte{0x7f};
    refresh_journal_frame_checksum(identity_changed, first_size);
    check_mutation(identity_changed);

    auto oversized_frame = baseline;
    write_little_u32(oversized_frame, 5, std::numeric_limits<std::uint32_t>::max());
    check_mutation(oversized_frame);
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

CK_TEST(volume_archive_recovery_cleanup_revalidates_digest_and_records_cancellation) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-recovery-cleanup", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = journal_root(scratch.path());
    const auto destination = address(scratch.path());
    constexpr std::uint64_t transaction = 41;
    constexpr std::string_view archive = "cleanup.ckarchive";
#if defined(_WIN32)
    const auto staging_name = NativeName::from_windows(u".cleanup.ckarchive.cktmp-41");
#else
    const auto staging_name = NativeName::from_posix(".cleanup.ckarchive.cktmp-41");
#endif
    CK_CHECK(staging_name.has_value());
    if (!staging_name)
        return;
    const auto staging_address = destination.child_address(*staging_name);
    CK_CHECK(staging_address.value());
    if (!staging_address.value())
        return;
    const auto staging_path = scratch.path() / ".cleanup.ckarchive.cktmp-41";
    const std::array<std::byte, 12> content{
        std::byte{'a'}, std::byte{'r'}, std::byte{'c'}, std::byte{'h'}, std::byte{'i'}, std::byte{'v'},
        std::byte{'e'}, std::byte{'-'}, std::byte{'s'}, std::byte{'t'}, std::byte{'a'}, std::byte{'g'}};
    {
        std::ofstream stream(staging_path, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(content.data()), static_cast<std::streamsize>(content.size()));
    }
    const auto digest = digest_bytes(content);
    const auto name = archive_journal_name("29000000000000000000000000000000");
    CK_CHECK(name.has_value());
    if (!name)
        return;
    auto file = open_native_operation_journal_file(root, *name, true);
    CK_CHECK(file.value());
    if (!file.value())
        return;
    auto journal = OperationJournal::create(FileOperation::Copy, journal_id(transaction));
    CK_CHECK(journal.value());
    if (!journal.value())
        return;
    const auto before_digest = archive_recovery_evidence(transaction, destination, *staging_address.value(), archive);
    const auto after_digest = archive_recovery_evidence(transaction, destination, *staging_address.value(), archive, digest);
    auto journal_file = std::move(file).take_value();
    CK_CHECK(append_operation_journal_file(*journal_file, **journal.value(), {1, 0, OperationJournalState::Planned, before_digest}).value());
    CK_CHECK(append_operation_journal_file(*journal_file, **journal.value(), {2, 0, OperationJournalState::Prepared, before_digest}).value());
    CK_CHECK(append_operation_journal_file(*journal_file, **journal.value(), {3, 0, OperationJournalState::Copied, after_digest}).value());
    journal_file.reset();

    std::atomic_bool woke = false;
    FilesVolumeArchiveRecoveryCleanupService service([&woke] { woke.store(true); });
    CK_CHECK(service.request(root, *name, journal_id(transaction)) ==
             FilesVolumeArchiveRecoveryCleanupSubmission::Accepted);
    std::optional<FilesVolumeArchiveRecoveryCleanupReply> reply;
    for (int i = 0; i != 500 && !reply; ++i) {
        reply = service.take();
        if (!reply)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(woke.load());
#if defined(_WIN32)
    CK_CHECK(reply && reply->state == FilesVolumeArchiveRecoveryCleanupState::Complete);
    CK_CHECK(reply && reply->removed);
    CK_CHECK(!std::filesystem::exists(staging_path));
#else
    CK_CHECK(reply && reply->state == FilesVolumeArchiveRecoveryCleanupState::Failed);
    CK_CHECK(reply && reply->error && reply->error->state == ObservationState::Unsupported);
    CK_CHECK(std::filesystem::exists(staging_path));
#endif
    service.request_stop();
    service.join();

    auto reopened = open_native_operation_journal_file(root, *name, false);
    CK_CHECK(reopened.value());
    if (reopened.value()) {
        auto loaded = load_operation_journal(**reopened.value());
        CK_CHECK(loaded.value());
        if (loaded.value()) {
#if defined(_WIN32)
            CK_CHECK(loaded.value()->journal->records().size() == 4);
            CK_CHECK(loaded.value()->journal->records().back().state == OperationJournalState::Cancelled);
#else
            CK_CHECK(loaded.value()->journal->records().size() == 3);
            CK_CHECK(loaded.value()->journal->records().back().state == OperationJournalState::Copied);
#endif
            auto inspected = inspect_volume_archive_recovery(*loaded.value()->journal, loaded.value()->tail);
            CK_CHECK(inspected.value());
            if (inspected.value())
#if defined(_WIN32)
                CK_CHECK(!inspected.value()->operation.requires_reconciliation);
#else
                CK_CHECK(inspected.value()->operation.requires_reconciliation);
#endif
        }
    }
}

CK_TEST(volume_archive_recovery_cleanup_refuses_changed_staging_bytes) {
    ck::testing::ScratchDirectory scratch("files-volume-archive-recovery-cleanup-mismatch", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto root = journal_root(scratch.path());
    const auto destination = address(scratch.path());
    constexpr std::uint64_t transaction = 42;
#if defined(_WIN32)
    const auto staging_name = NativeName::from_windows(u".mismatch.ckarchive.cktmp-42");
#else
    const auto staging_name = NativeName::from_posix(".mismatch.ckarchive.cktmp-42");
#endif
    const auto staging_address = destination.child_address(*staging_name);
    const auto staging_path = scratch.path() / ".mismatch.ckarchive.cktmp-42";
    const std::array<std::byte, 3> actual{std::byte{'b'}, std::byte{'a'}, std::byte{'d'}};
    const std::array<std::byte, 3> expected{std::byte{'g'}, std::byte{'o'}, std::byte{'o'}};
    {
        std::ofstream stream(staging_path, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(actual.data()), static_cast<std::streamsize>(actual.size()));
    }
    const auto name = archive_journal_name("2a000000000000000000000000000000");
    CK_CHECK(staging_address.value() && name.has_value());
    if (!staging_address.value() || !name)
        return;
    auto file = open_native_operation_journal_file(root, *name, true);
    auto journal = OperationJournal::create(FileOperation::Copy, journal_id(transaction));
    CK_CHECK(file.value() && journal.value());
    if (!file.value() || !journal.value())
        return;
    const auto before_digest = archive_recovery_evidence(transaction, destination, *staging_address.value(), "mismatch.ckarchive");
    const auto after_digest = archive_recovery_evidence(transaction, destination, *staging_address.value(),
                                                        "mismatch.ckarchive", digest_bytes(expected));
    auto journal_file = std::move(file).take_value();
    CK_CHECK(append_operation_journal_file(*journal_file, **journal.value(), {1, 0, OperationJournalState::Planned, before_digest}).value());
    CK_CHECK(append_operation_journal_file(*journal_file, **journal.value(), {2, 0, OperationJournalState::Prepared, before_digest}).value());
    CK_CHECK(append_operation_journal_file(*journal_file, **journal.value(), {3, 0, OperationJournalState::Copied, after_digest}).value());
    journal_file.reset();
    FilesVolumeArchiveRecoveryCleanupService service;
    CK_CHECK(service.request(root, *name, journal_id(transaction)) ==
             FilesVolumeArchiveRecoveryCleanupSubmission::Accepted);
    std::optional<FilesVolumeArchiveRecoveryCleanupReply> reply;
    for (int i = 0; i != 500 && !reply; ++i) {
        reply = service.take();
        if (!reply)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CK_CHECK(reply && reply->state == FilesVolumeArchiveRecoveryCleanupState::Failed);
    CK_CHECK(reply && reply->error && reply->error->state == ObservationState::Changed);
    CK_CHECK(std::filesystem::exists(staging_path));
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
