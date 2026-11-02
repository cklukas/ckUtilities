// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "native_operation_copy_job.hpp"
#include "native_operation_delete_tree.hpp"
#include "native_operation_move_job.hpp"
#include "native_operation_recovery_evidence.hpp"
#include "native_operation_recovery.hpp"
#include "scratch_directory.hpp"
#include "stream_copy_job.hpp"
#include "operation_journal_file.hpp"

#include <cvision/testing/cktest.hpp>

#include <array>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace ck::files;
namespace fs = std::filesystem;

template <class T> T take(Observation<T> result) {
    if (!result.value())
        throw std::runtime_error("native content fixture: " + result.unavailable_detail()->error.operation);
    return std::move(result).take_value();
}

NativeName name(const char* text) {
#if defined(_WIN32)
    std::u16string value;
    for (const auto* p = text; *p; ++p) value.push_back(static_cast<char16_t>(*p));
    return *NativeName::from_windows(std::move(value));
#else
    return *NativeName::from_posix(text);
#endif
}
}

CK_TEST(native_operation_staging_file_is_created_relative_to_held_directory_and_roundtrips_content) {
    const ck::testing::ScratchDirectory scratch("files-operation-content", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto staging = take(directory->create_child_file(name("staging.bin")));
    const std::string text = "held staging content";
    std::vector<std::byte> bytes;
    for (const auto character : text) bytes.push_back(std::byte{static_cast<unsigned char>(character)});
    CK_CHECK(take(staging->write_content(0, std::span<const std::byte>(bytes).first(5))) == 5);
    CK_CHECK(take(staging->write_content(5, std::span<const std::byte>(bytes).subspan(5))) == bytes.size() - 5);
    CK_CHECK(take(staging->flush_content()));
    const auto read = take(staging->read_content({bytes.size() + 1}, 0));
    CK_CHECK(read.complete && read.bytes == bytes);
    CK_CHECK(!directory->create_child_file(name("staging.bin")).value());
}

CK_TEST(native_operation_staging_applies_platform_portable_basic_metadata) {
    const ck::testing::ScratchDirectory scratch("files-operation-metadata-boundary",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto source_path = scratch.path() / "source.bin";
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "portable metadata";
    }
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto source = take(directory->open_child(name("source.bin"), EntryAccess::ContentRead));
    const auto source_metadata = take(source->inspect_basic());
    auto staging = take(directory->create_child_file(name(".ck-metadata-staging")));
    const auto applied = staging->apply_portable_basic_metadata(source_metadata);
    if (!applied.value())
        throw std::runtime_error("portable metadata application unavailable: " +
            (applied.unavailable_detail() ? applied.unavailable_detail()->error.operation : "false"));
    CK_CHECK(applied.value() && *applied.value());
    const auto staging_metadata = take(staging->inspect_basic());
#if defined(_WIN32)
    CK_CHECK(source_metadata.windows_attributes.value() && staging_metadata.windows_attributes.value() &&
             *source_metadata.windows_attributes.value() == *staging_metadata.windows_attributes.value() &&
             source_metadata.accessed.value() && staging_metadata.accessed.value() &&
             *source_metadata.accessed.value() == *staging_metadata.accessed.value() &&
             source_metadata.modified.value() && staging_metadata.modified.value() &&
             *source_metadata.modified.value() == *staging_metadata.modified.value() &&
             source_metadata.changed.value() && staging_metadata.changed.value() &&
             *source_metadata.changed.value() == *staging_metadata.changed.value() &&
             source_metadata.created.value() && staging_metadata.created.value() &&
             *source_metadata.created.value() == *staging_metadata.created.value());
#else
    CK_CHECK(source_metadata.posix_permissions.value() && staging_metadata.posix_permissions.value() &&
             *source_metadata.posix_permissions.value() == *staging_metadata.posix_permissions.value() &&
             source_metadata.accessed.value() && staging_metadata.accessed.value() &&
             *source_metadata.accessed.value() == *staging_metadata.accessed.value() &&
             source_metadata.modified.value() && staging_metadata.modified.value() &&
             *source_metadata.modified.value() == *staging_metadata.modified.value());
#endif
}

CK_TEST(native_operation_staging_applies_reviewed_nonsecurity_attribute_on_held_object) {
    const ck::testing::ScratchDirectory scratch("files-operation-attribute-boundary",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto source_path = scratch.path() / "source.bin";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "attribute source";
    }
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto source = take(directory->open_child(name("source.bin"), EntryAccess::ContentRead));
    const auto source_metadata = take(source->inspect_basic());
    auto staging = take(directory->create_child_file(name(".ck-attribute-staging")));
#if defined(_WIN32)
    const auto identifier = NativeFeatureIdentifier::from_windows_ea("ckutilities");
    const auto flags = Observation<std::uint32_t>::available(0);
#else
    const auto identifier = NativeFeatureIdentifier::from_posix("user.ckutilities.portable");
    const auto flags = Observation<std::uint32_t>::unavailable(
        ObservationState::Unsupported, {ErrorDomain::Adapter,
            static_cast<std::int64_t>(AdapterError::NotApplicable), "attributes.test_flags"});
#endif
    if (!identifier || !source_metadata.identity.value())
        throw std::runtime_error("attribute fixture could not construct its native identity");
    const NativeBytes bytes{std::byte{'a'}, std::byte{'t'}, std::byte{'t'}, std::byte{'r'}, std::byte{'\0'}, std::byte{'x'}};
    const AttributeValue value{*identifier, bytes, source_metadata.identity, flags};
    const auto applied = staging->apply_nonsecurity_attribute(value);
    if (!applied.value())
        throw std::runtime_error("nonsecurity attribute application unavailable: " +
            (applied.unavailable_detail() ? applied.unavailable_detail()->error.operation : "false"));
    CK_CHECK(applied.value() && *applied.value());
    const auto observed = take(staging->read_attribute(*identifier));
    CK_CHECK(observed.bytes == bytes && observed.owner.value());
    if (observed.owner.value() && source_metadata.identity.value())
        CK_CHECK(observed.bytes == bytes && observed.owner.value()->scope == source_metadata.identity.value()->scope);
}

CK_TEST(native_operation_staging_file_rejects_foreign_names_and_cancelled_writes) {
    const ck::testing::ScratchDirectory scratch("files-operation-content-invalid", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
#if defined(_WIN32)
    const auto foreign = NativeName::from_posix("staging.bin");
#else
    const auto foreign = NativeName::from_windows(u"staging.bin");
#endif
    CK_CHECK(!directory->create_child_file(*foreign).value());
    auto staging = take(directory->create_child_file(name("cancelled.bin")));
    std::stop_source stop;
    stop.request_stop();
    const std::array<std::byte, 1> byte{std::byte{1}};
    CK_CHECK(staging->write_content(0, byte, {}, stop.get_token()).state() == ObservationState::Unknown);
}

CK_TEST(native_operation_staging_cleanup_requires_handle_bound_disposition) {
    const ck::testing::ScratchDirectory scratch("files-operation-staging-cleanup",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto staging = take(directory->create_child_file(name("owned-staging.bin")));
    const std::array<std::byte, 4> bytes{std::byte{'o'}, std::byte{'w'}, std::byte{'n'}, std::byte{'d'}};
    CK_CHECK(take(staging->write_content(0, bytes)) == bytes.size());
    CK_CHECK(take(staging->flush_content()));
    const auto metadata = take(staging->inspect_basic());
    CK_CHECK(metadata.identity.value());
    if (!metadata.identity.value()) return;
    const auto discarded = directory->discard_child(name("owned-staging.bin"), *metadata.identity.value());
    staging.reset();
#if defined(_WIN32)
    if (!discarded.value())
        throw std::runtime_error("staging discard failed: " + discarded.unavailable_detail()->error.operation);
    if (fs::exists(scratch.path() / "owned-staging.bin"))
        throw std::runtime_error("staging discard remained present");
    CK_CHECK(discarded.value() && !fs::exists(scratch.path() / "owned-staging.bin"));
#else
    CK_CHECK(!discarded.value() && discarded.state() == ObservationState::Unsupported &&
             fs::exists(scratch.path() / "owned-staging.bin"));
#endif
}

CK_TEST(native_operation_stream_copy_composes_held_source_and_staging_handles) {
    const ck::testing::ScratchDirectory scratch("files-operation-native-copy", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto source_path = scratch.path() / "source.bin";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "native held stream copy with independent verification";
    }
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto source = take(directory->open_child(name("source.bin"), EntryAccess::ContentRead));
    auto destination = take(directory->create_child_file(name("staging.bin")));
    StreamCopyLimits limits;
    limits.chunk_bytes = 7;
    limits.max_bytes = 1024;
    auto created = create_stream_copy_job(*source, *destination, limits);
    CK_CHECK(created.value());
    if (!created.value())
        return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 100 && job->state() != StreamCopyState::Complete &&
         job->state() != StreamCopyState::Failed; ++step)
        job->advance();
    CK_CHECK(job->state() == StreamCopyState::Complete);
    const auto read = take(destination->read_content({1024}, 0));
    CK_CHECK(read.complete && read.bytes.size() == job->result().source_bytes);
}

CK_TEST(native_operation_copy_job_journals_verifies_and_publishes_without_replacement) {
    const ck::testing::ScratchDirectory scratch("files-native-operation-copy-job",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto source_directory_path = scratch.path() / "source";
    const auto destination_directory_path = scratch.path() / "destination";
    fs::create_directory(source_directory_path);
    fs::create_directory(destination_directory_path);
    {
        std::ofstream output(source_directory_path / "source.bin", std::ios::binary);
        output << "native operation copy job";
    }

    NativeFileSystem filesystem;
    auto source_parent = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(source_directory_path)));
    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(destination_directory_path)));
    const auto source_name = name("source.bin");
    const auto destination_name = name("copied.bin");
    const auto source = take(source_parent->open_child(source_name, EntryAccess::ContentRead));
    const auto source_metadata = take(source->inspect_basic());
    const auto source_parent_identity = take(source_parent->identity());
    const auto destination_identity = take(destination->identity());

    const auto journal_root = take(NativeAbsolutePath::from_host(scratch.path()));
    auto journal_file = take(open_native_operation_journal_file(journal_root, name("copy.journal"), true));
    OperationJournalId journal_id;
    journal_id.bytes[0] = std::byte{0x31};
    auto journal = take(OperationJournal::create(FileOperation::Copy, journal_id, {1, 32, 256}));
    CK_CHECK(append_operation_journal_file(*journal_file, *journal,
        {1, 0, OperationJournalState::Planned, {}}).value());

    OperationMetadataRequest metadata;
    metadata.apply_traversal_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    metadata.apply_security_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    metadata.apply_final_times = [](std::stop_token) { return Observation<bool>::available(true); };
    metadata.flush_destination = [](std::stop_token) { return Observation<bool>::available(true); };

    NativeOperationCopyRequest request{
        journal,
        std::shared_ptr<OperationJournalFile>(std::move(journal_file)),
        0,
        source_parent,
        source_parent_identity,
        source_name,
        take(source_metadata.identity),
        destination,
        destination_identity,
        std::nullopt,
        name(".ck-copy-staging"),
        destination_name,
        {},
        {}};
    request.copy_limits.chunk_bytes = 5;
    request.copy_limits.max_bytes = 1024;
    request.prepare_metadata = [metadata_request = std::move(metadata)](OpenedEntry&, std::stop_token) mutable {
        return OperationMetadataJob::create(std::move(metadata_request));
    };

    auto created = NativeOperationCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 200 && job->state() != OperationExecutionState::Complete &&
         job->state() != OperationExecutionState::Failed &&
         job->state() != OperationExecutionState::NeedsReconciliation; ++step)
        job->advance();

    if (job->state() != OperationExecutionState::Complete) {
        std::string detail = "native copy execution state " + std::to_string(static_cast<int>(job->state())) +
            " bytes " + std::to_string(job->progress().observed_bytes) + " records";
        for (const auto& record : journal->records()) detail += " " + std::to_string(static_cast<int>(record.state));
        if (job->result().failure) detail += " failure " + job->result().failure->error.operation;
        throw std::runtime_error(detail);
    }

    CK_CHECK(job->state() == OperationExecutionState::Complete);
    CK_CHECK(fs::exists(destination_directory_path / "copied.bin"));
    const auto published = take(destination->open_child(destination_name, EntryAccess::ContentRead));
    const auto published_metadata = take(published->inspect_basic());
    if (!published_metadata.logical_size.value() ||
        *published_metadata.logical_size.value() != std::string("native operation copy job").size())
        throw std::runtime_error("published native size=" +
                                 (published_metadata.logical_size.value() ?
                                      std::to_string(*published_metadata.logical_size.value()) : "unknown"));
    const auto published_content = take(published->read_content({1024}, 0));
    std::string copied;
    copied.reserve(published_content.bytes.size());
    for (const auto byte : published_content.bytes)
        copied.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    if (copied != "native operation copy job")
        throw std::runtime_error("copied bytes=" + std::to_string(copied.size()) + " value=" + copied +
                                 " progress=" + std::to_string(job->progress().observed_bytes));
    CK_CHECK(journal->state(0) == OperationJournalState::Complete);
    CK_CHECK(journal->records().size() >= 3 && !journal->records()[2].evidence.empty());
    if (journal->records().size() >= 3) {
        auto evidence = decode_native_operation_recovery_evidence(journal->records()[2].evidence);
        CK_CHECK(evidence.value() && evidence.value()->staging_name == name(".ck-copy-staging") &&
                 evidence.value()->destination_name == destination_name &&
                 evidence.value()->staging_identity.has_value() &&
                 evidence.value()->destination_identity.matches_live(destination_identity));
    }
    CK_CHECK(!fs::exists(destination_directory_path / "source.bin"));
}

CK_TEST(native_operation_move_batch_renames_only_after_identity_revalidation) {
    const ck::testing::ScratchDirectory scratch("files-native-operation-move",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto source_path = scratch.path() / "source";
    const auto destination_path = scratch.path() / "destination";
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    fs::create_directory(source_path);
    fs::create_directory(destination_path);
    {
        std::ofstream output(source_path / "move-me.bin", std::ios::binary);
        output << "same-filesystem move";
    }

    NativeFileSystem filesystem;
    auto source_parent = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(source_path)));
    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(destination_path)));
    auto source = take(HeldChild::observe_with_access(source_parent, name("move-me.bin"), EntryAccess::MetadataOnly));
    const auto journal_root = take(NativeAbsolutePath::from_host(scratch.path()));
    OperationJournalId journal_id;
    journal_id.bytes[0] = std::byte{0x52};

    NativeOperationMoveBatchRequest request;
    request.sources = {source};
    request.destination = destination;
    request.destination_names = {name("moved.bin")};
    request.journal_root = journal_root;
    request.journal_id = journal_id;
    request.journal_limits = {1, 32, 256};
    request.open_journal = [](const NativeAbsolutePath &root, const NativeName &basename, bool create_new) {
        return open_native_operation_journal_file(root, basename, create_new);
    };
    auto created = create_native_operation_move_batch(std::move(request));
    CK_CHECK(created.value());
    if (!created.value())
        throw std::runtime_error("native move factory failed: " +
            (created.unavailable_detail() ? created.unavailable_detail()->error.operation : "unknown"));
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 32 && job->state() != OperationBatchState::Complete &&
         job->state() != OperationBatchState::Failed &&
         job->state() != OperationBatchState::NeedsReconciliation; ++step)
        job->advance();
    if (job->state() != OperationBatchState::Complete) {
        const auto &item = job->result().items.front();
        std::string detail = "native move did not complete" +
            (item.failure ? ": " + item.failure->error.operation : "");
        for (const auto &entry : job->result().items)
            detail += " item=" + std::to_string(entry.item) + " outcome=" +
                std::to_string(static_cast<int>(entry.outcome));
        throw std::runtime_error(detail);
    }
    CK_CHECK(!fs::exists(source_path / "move-me.bin"));
    CK_CHECK(fs::exists(destination_path / "moved.bin"));
}

#if defined(__linux__)
CK_TEST(native_operation_cross_filesystem_move_retains_source_when_exclusion_is_unsupported_with_open_writer) {
    const ck::testing::ScratchDirectory scratch("files-native-cross-filesystem-move",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto source_path = scratch.path() / "source.bin";
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "cross-filesystem source remains on POSIX";
    }
    std::ofstream open_writer(source_path, std::ios::binary | std::ios::app);
    CK_CHECK(open_writer.is_open());
    open_writer << " while another writer remains open";
    open_writer.flush();
    const auto destination_path = fs::path("/dev/shm") /
        ("ck-files-crossmove-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup final {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove_all(path, error); }
    } cleanup{destination_path};
    fs::create_directory(destination_path);

    NativeFileSystem filesystem;
    auto source_parent = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(destination_path)));
    auto source = take(HeldChild::observe_with_access(source_parent, name("source.bin"), EntryAccess::MetadataOnly));
    const auto destination_identity = take(destination->identity());
    const auto journal_root = take(NativeAbsolutePath::from_host(scratch.path()));
    auto journal_file = take(open_native_operation_journal_file(journal_root, name("move.journal"), true));
    OperationJournalId journal_id;
    journal_id.bytes[0] = std::byte{0x73};
    auto journal = take(OperationJournal::create(FileOperation::Move, journal_id, {1, 32, 256}));
    CK_CHECK(append_operation_journal_file(*journal_file, *journal,
        {1, 0, OperationJournalState::Planned, {}}).value());

    NativeOperationMoveRequest request{
        journal,
        std::shared_ptr<OperationJournalFile>(std::move(journal_file)),
        0,
        source,
        destination,
        destination_identity,
        name("moved.bin"),
        {4, 1024},
        true};
    auto created = NativeOperationMoveJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 64 && job->state() != OperationExecutionState::Complete &&
         job->state() != OperationExecutionState::Failed &&
         job->state() != OperationExecutionState::RetainedSource &&
         job->state() != OperationExecutionState::NeedsReconciliation; ++step)
        job->advance();
    CK_CHECK(job->state() == OperationExecutionState::Failed ||
             job->state() == OperationExecutionState::RetainedSource);
    CK_CHECK(fs::exists(source_path));
    CK_CHECK(!fs::exists(destination_path / "moved.bin"));
    open_writer << " after the refused move";
    open_writer.flush();
    CK_CHECK(fs::file_size(source_path) > 0);
}
#endif

CK_TEST(native_operation_recovery_cleanup_revalidates_staging_identity_and_disposition) {
    const ck::testing::ScratchDirectory scratch("files-native-recovery-cleanup",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    NativeFileSystem filesystem;
    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    auto staging = take(destination->create_child_file(name(".ck-crash-staging")));
    const std::array<std::byte, 3> bytes{std::byte{'r'}, std::byte{'e'}, std::byte{'c'}};
    CK_CHECK(take(staging->write_content(0, bytes)) == bytes.size());
    CK_CHECK(take(staging->flush_content()));
    const auto staging_metadata = take(staging->inspect_basic());
    CK_CHECK(staging_metadata.identity.value());
    if (!staging_metadata.identity.value()) return;
    const auto destination_identity = take(destination->identity());
    const auto staging_durable = NativeDurableIdentity::from_live(
        *staging_metadata.identity.value());
    const auto destination_durable = NativeDurableIdentity::from_live(destination_identity);
    CK_CHECK(staging_durable && destination_durable);
    if (!staging_durable || !destination_durable) return;
    NativeOperationRecoveryCleanupRequest request{
        {0, OperationJournalState::Copied,
         NativeOperationRecoveryEvidence{name(".ck-crash-staging"), name("published"),
          *destination_durable, *destination_durable, *destination_durable, staging_durable, {}, {}},
         std::nullopt}, destination, destination_identity};
    staging.reset();
    auto cleaned = cleanup_native_unpublished_staging(request);
#if defined(_WIN32)
    if (!cleaned.value() || !*cleaned.value())
        throw std::runtime_error("recovery cleanup unavailable: " +
            (cleaned.unavailable_detail() ? cleaned.unavailable_detail()->error.operation : "false"));
    if (fs::exists(scratch.path() / ".ck-crash-staging"))
        throw std::runtime_error("recovery cleanup left staging object");
    CK_CHECK(cleaned.value() && *cleaned.value() && !fs::exists(scratch.path() / ".ck-crash-staging"));
#else
    CK_CHECK(!cleaned.value() && cleaned.state() == ObservationState::Unsupported &&
             fs::exists(scratch.path() / ".ck-crash-staging"));
#endif
}

CK_TEST(native_operation_recovery_cleanup_rejects_substituted_staging_identity) {
    const ck::testing::ScratchDirectory scratch("files-native-recovery-substitution",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    NativeFileSystem filesystem;
    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    auto original = take(destination->create_child_file(name(".ck-substituted-staging")));
    const std::array<std::byte, 1> original_byte{std::byte{'o'}};
    CK_CHECK(take(original->write_content(0, original_byte)) == original_byte.size());
    CK_CHECK(take(original->flush_content()));
    const auto original_metadata = take(original->inspect_basic());
    CK_CHECK(original_metadata.identity.value());
    if (!original_metadata.identity.value()) return;
    const auto destination_identity = take(destination->identity());
    const auto durable = NativeDurableIdentity::from_live(destination_identity);
    const auto original_durable = NativeDurableIdentity::from_live(*original_metadata.identity.value());
    CK_CHECK(durable && original_durable);
    if (!durable || !original_durable) return;
    original.reset();
    fs::rename(scratch.path() / ".ck-substituted-staging", scratch.path() / ".ck-original");
    auto replacement = take(destination->create_child_file(name(".ck-substituted-staging")));
    const std::array<std::byte, 1> replacement_byte{std::byte{'r'}};
    CK_CHECK(take(replacement->write_content(0, replacement_byte)) == replacement_byte.size());
    CK_CHECK(take(replacement->flush_content()));
    replacement.reset();
    NativeOperationRecoveryCleanupRequest request{
        {0, OperationJournalState::Copied,
         NativeOperationRecoveryEvidence{name(".ck-substituted-staging"), name("published"),
          *durable, *durable, *durable, original_durable, {}, {}}, std::nullopt},
        destination, destination_identity};
    auto cleaned = cleanup_native_unpublished_staging(request);
    CK_CHECK(!cleaned.value() && cleaned.state() == ObservationState::Changed &&
             fs::exists(scratch.path() / ".ck-substituted-staging"));
}

CK_TEST(native_operation_copy_batch_keeps_worker_handles_and_item_receipts) {
    const ck::testing::ScratchDirectory scratch("files-native-operation-copy-batch",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto source_directory_path = scratch.path() / "source";
    const auto destination_directory_path = scratch.path() / "destination";
    fs::create_directory(source_directory_path);
    fs::create_directory(destination_directory_path);
    {
        std::ofstream first(source_directory_path / "first.bin", std::ios::binary);
        first << "first native batch payload";
        std::ofstream second(source_directory_path / "second.bin", std::ios::binary);
        second << "second native batch payload";
    }

    NativeFileSystem filesystem;
    auto source_parent = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(source_directory_path)));
    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(destination_directory_path)));
    const auto source_parent_identity = take(source_parent->identity());
    const auto destination_identity = take(destination->identity());

    auto make_job = [&](const char* source_text, const char* staging_text, const char* destination_text,
                        const char* journal_text, std::byte journal_byte) {
        const auto source_name = name(source_text);
        const auto staging_name = name(staging_text);
        const auto destination_name = name(destination_text);
        const auto source = take(source_parent->open_child(source_name, EntryAccess::ContentRead));
        const auto source_metadata = take(source->inspect_basic());
        const auto journal_root = take(NativeAbsolutePath::from_host(scratch.path()));
        auto journal_file = take(open_native_operation_journal_file(journal_root, name(journal_text), true));
        OperationJournalId journal_id;
        journal_id.bytes[0] = journal_byte;
        auto journal = take(OperationJournal::create(FileOperation::Copy, journal_id, {1, 32, 512}));
        CK_CHECK(append_operation_journal_file(*journal_file, *journal,
            {1, 0, OperationJournalState::Planned, {}}).value());

        NativeOperationCopyRequest request{
            journal,
            std::shared_ptr<OperationJournalFile>(std::move(journal_file)),
            0,
            source_parent,
            source_parent_identity,
            source_name,
            take(source_metadata.identity),
            destination,
            destination_identity,
            std::nullopt,
            staging_name,
            destination_name,
            {},
            {}};
        request.copy_limits.chunk_bytes = 4;
        request.copy_limits.max_bytes = 1024;
        request.prepare_metadata = [](OpenedEntry&, std::stop_token) {
            OperationMetadataRequest metadata;
            metadata.apply_traversal_metadata = [](std::stop_token) {
                return Observation<bool>::available(true);
            };
            metadata.apply_security_metadata = [](std::stop_token) {
                return Observation<bool>::available(true);
            };
            metadata.apply_final_times = [](std::stop_token) {
                return Observation<bool>::available(true);
            };
            metadata.flush_destination = [](std::stop_token) {
                return Observation<bool>::available(true);
            };
            return OperationMetadataJob::create(std::move(metadata));
        };
        auto created = NativeOperationCopyJob::create(std::move(request));
        CK_CHECK(created.value());
        return created.value() ? std::move(created).take_value() : NativeOperationCopyJob::Pointer{};
    };

    std::vector<NativeOperationCopyJob::Pointer> jobs;
    jobs.push_back(make_job("first.bin", ".ck-first-staging", "first-copy.bin", "first.journal", std::byte{0x41}));
    jobs.push_back(make_job("second.bin", ".ck-second-staging", "second-copy.bin", "second.journal", std::byte{0x42}));
    CK_CHECK(jobs[0] && jobs[1]);
    if (!jobs[0] || !jobs[1]) return;

    auto created = create_native_operation_copy_batch(std::move(jobs));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto batch = std::move(created).take_value();
    for (unsigned step = 0; step != 400 && batch->state() != OperationBatchState::Complete &&
         batch->state() != OperationBatchState::Failed && batch->state() != OperationBatchState::Cancelled; ++step)
        batch->advance();

    CK_CHECK(batch->state() == OperationBatchState::Complete);
    CK_CHECK(batch->result().items.size() == 2);
    CK_CHECK(batch->result().items[0].outcome == OperationItemOutcome::Complete);
    CK_CHECK(batch->result().items[1].outcome == OperationItemOutcome::Complete);
    for (const auto& destination_name : {name("first-copy.bin"), name("second-copy.bin")}) {
        const auto published = take(destination->open_child(destination_name, EntryAccess::ContentRead));
        const auto content = take(published->read_content({1024}, 0));
        CK_CHECK(content.complete && !content.bytes.empty());
    }
    batch.reset();
    for (const auto& journal_text : {"first.journal", "second.journal"}) {
        auto reopened = take(open_native_operation_journal_file(
            take(NativeAbsolutePath::from_host(scratch.path())), name(journal_text), false));
        auto loaded = take(load_operation_journal(*reopened));
        const auto verified = std::find_if(loaded.journal->records().rbegin(), loaded.journal->records().rend(),
            [](const OperationJournalRecord& record) {
                return record.state == OperationJournalState::Verified;
            });
        CK_CHECK(verified != loaded.journal->records().rend());
        if (verified != loaded.journal->records().rend()) {
            auto evidence = take(decode_native_operation_recovery_evidence(verified->evidence));
            CK_CHECK(evidence.content_digest.has_value());
        }
    }
}

CK_TEST(native_operation_copy_batch_factory_consumes_held_sources_and_requires_loss_consent) {
    const ck::testing::ScratchDirectory scratch("files-native-operation-copy-factory",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto source_directory_path = scratch.path() / "source";
    const auto destination_directory_path = scratch.path() / "destination";
    fs::create_directory(source_directory_path);
    fs::create_directory(destination_directory_path);
    {
        std::ofstream first(source_directory_path / "first.bin", std::ios::binary);
        first << "held factory payload";
    }

    NativeFileSystem filesystem;
    auto source_parent = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(source_directory_path)));
    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(destination_directory_path)));
    auto source = take(HeldChild::observe_with_access(source_parent, name("first.bin"), EntryAccess::ContentRead));
    auto journal_root = take(NativeAbsolutePath::from_host(scratch.path()));

    NativeOperationCopyBatchRequest refused;
    refused.sources = {source};
    refused.destination = destination;
    refused.destination_names = {name("first-copy.bin")};
    refused.journal_root = journal_root;
    refused.journal_id.bytes[0] = std::byte{0x70};
    refused.open_journal = [](const NativeAbsolutePath& root, const NativeName& basename, bool create_new) {
        return open_native_operation_journal_file(root, basename, create_new);
    };
    auto refused_created = create_native_operation_copy_batch(std::move(refused));
    CK_CHECK(refused_created.value());
    if (!refused_created.value()) return;
    auto refused_batch = std::move(refused_created).take_value();
    for (unsigned step = 0; step != 300 && refused_batch->state() != OperationBatchState::Complete &&
         refused_batch->state() != OperationBatchState::Failed &&
         refused_batch->state() != OperationBatchState::Cancelled; ++step)
        refused_batch->advance();
    CK_CHECK(refused_batch->state() == OperationBatchState::Failed);
    CK_CHECK(!fs::exists(destination_directory_path / "first-copy.bin"));

    NativeOperationCopyBatchRequest request;
    request.sources = {source};
    request.destination = destination;
    request.destination_names = {name("first-copy-consented.bin")};
    request.journal_root = journal_root;
    request.journal_id.bytes[0] = std::byte{0x71};
    request.open_journal = [](const NativeAbsolutePath& root, const NativeName& basename, bool create_new) {
        return open_native_operation_journal_file(root, basename, create_new);
    };
    request.policy.allow_unhandled_metadata_loss = true;
    request.policy.copy_limits.chunk_bytes = 5;
    request.policy.copy_limits.max_bytes = 1024;
    auto created = create_native_operation_copy_batch(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto batch = std::move(created).take_value();
    for (unsigned step = 0; step != 300 && batch->state() != OperationBatchState::Complete &&
         batch->state() != OperationBatchState::Failed && batch->state() != OperationBatchState::Cancelled; ++step)
        batch->advance();
    CK_CHECK(batch->state() == OperationBatchState::Complete);
    CK_CHECK(fs::exists(destination_directory_path / "first-copy-consented.bin"));
    const auto copied = take(destination->open_child(name("first-copy-consented.bin"), EntryAccess::ContentRead));
    const auto content = take(copied->read_content({1024}, 0));
    CK_CHECK(content.complete && content.bytes.size() == std::string("held factory payload").size());

    fs::create_directory(source_directory_path / "nested");
    auto directory_source = take(HeldChild::observe_with_access(source_parent, name("nested"), EntryAccess::MetadataOnly));
    NativeOperationCopyBatchRequest unsupported;
    unsupported.sources = {directory_source};
    unsupported.destination = destination;
    unsupported.destination_names = {name("nested-copy")};
    unsupported.journal_root = journal_root;
    unsupported.journal_id.bytes[0] = std::byte{0x72};
    unsupported.open_journal = [](const NativeAbsolutePath& root, const NativeName& basename, bool create_new) {
        return open_native_operation_journal_file(root, basename, create_new);
    };
    const auto rejected_directory = create_native_operation_copy_batch(std::move(unsupported));
    CK_CHECK(!rejected_directory.value() && rejected_directory.state() == ObservationState::Unsupported);
}

CK_TEST(native_operation_copy_tree_stages_exact_inventory_and_publishes_root_no_replace) {
    const ck::testing::ScratchDirectory scratch("files-native-operation-copy-tree",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto source_path = scratch.path() / "source";
    const auto destination_path = scratch.path() / "destination";
    fs::create_directory(source_path);
    fs::create_directory(destination_path);
    fs::create_directory(source_path / "nested");
    {
        std::ofstream first(source_path / "root.bin", std::ios::binary);
        first << "recursive root payload";
        std::ofstream second(source_path / "nested" / "child.bin", std::ios::binary);
        second << "recursive child payload";
    }

    NativeFileSystem filesystem;
    auto fixture_root = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    auto root_source = take(HeldChild::observe_with_access(fixture_root, name("source"), EntryAccess::MetadataOnly));
    auto source_root = std::shared_ptr<DirectoryCursor>(take(filesystem.open_directory(source_path)));
    auto source_native_path = take(NativeAbsolutePath::from_host(source_path));
    RecursiveInventoryJob inventory(source_root, source_native_path, InventoryMountPolicy::StayOnRootMount,
                                    InventoryLinkPolicy::PreserveLinks, {}, system_monotonic_clock(), root_source);
    for (unsigned step = 0; step != 200 && inventory.state() != RecursiveInventoryState::Complete &&
         inventory.state() != RecursiveInventoryState::Failed; ++step)
        inventory.advance();
    if (inventory.state() != RecursiveInventoryState::Complete) {
        const auto completed_probe = inventory.completed();
        throw std::runtime_error("recursive inventory state=" + std::to_string(static_cast<int>(inventory.state())) +
                                 (inventory.failure() ? " failure=" + inventory.failure()->error.operation : "") +
                                 (completed_probe.unavailable_detail() ?
                                      " completed=" + completed_probe.unavailable_detail()->error.operation : ""));
    }
    const auto completed = take(inventory.completed_sources());
    CK_CHECK(completed->inventory()->entries().size() == 4);

    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(destination_path)));
    NativeOperationCopyTreeRequest request(name("source-copy"));
    request.inventory = completed;
    request.destination = destination;
    request.journal_root = take(NativeAbsolutePath::from_host(scratch.path()));
    request.journal_id.bytes[0] = std::byte{0x73};
    request.policy.allow_unhandled_metadata_loss = true;
    request.policy.copy_limits.chunk_bytes = 4;
    request.policy.copy_limits.max_bytes = 1024;
    request.open_journal = [](const NativeAbsolutePath& root, const NativeName& basename, bool create_new) {
        return open_native_operation_journal_file(root, basename, create_new);
    };
    auto created = create_native_operation_copy_tree(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 1000 && job->state() != OperationExecutionState::Complete &&
         job->state() != OperationExecutionState::Failed &&
         job->state() != OperationExecutionState::Cancelled &&
         job->state() != OperationExecutionState::NeedsReconciliation; ++step)
        job->advance();
    if (job->state() != OperationExecutionState::Complete) {
        throw std::runtime_error("tree copy state=" + std::to_string(static_cast<int>(job->state())) +
                                 (job->result().failure ? " failure=" + job->result().failure->error.operation +
                                      " code=" + std::to_string(job->result().failure->error.code) : ""));
    }
    CK_CHECK(fs::is_directory(destination_path / "source-copy"));
    CK_CHECK(fs::is_regular_file(destination_path / "source-copy" / "root.bin"));
    CK_CHECK(fs::is_regular_file(destination_path / "source-copy" / "nested" / "child.bin"));
    std::ifstream root_input(destination_path / "source-copy" / "root.bin", std::ios::binary);
    std::string root_content((std::istreambuf_iterator<char>(root_input)), std::istreambuf_iterator<char>());
    CK_CHECK(root_content == "recursive root payload");
    std::ifstream child_input(destination_path / "source-copy" / "nested" / "child.bin", std::ios::binary);
    std::string child_content((std::istreambuf_iterator<char>(child_input)), std::istreambuf_iterator<char>());
    CK_CHECK(child_content == "recursive child payload");
    CK_CHECK(job->cleanup_unpublished_staging().state() == ObservationState::Unsupported);

    NativeOperationCopyTreeRequest collision_request(name("source-copy"));
    collision_request.inventory = completed;
    collision_request.destination = destination;
    collision_request.journal_root = take(NativeAbsolutePath::from_host(scratch.path()));
    collision_request.journal_id.bytes[0] = std::byte{0x74};
    collision_request.policy.allow_unhandled_metadata_loss = true;
    collision_request.policy.copy_limits.chunk_bytes = 4;
    collision_request.policy.copy_limits.max_bytes = 1024;
    collision_request.open_journal = [](const NativeAbsolutePath& root, const NativeName& basename, bool create_new) {
        return open_native_operation_journal_file(root, basename, create_new);
    };
    auto collision_created = create_native_operation_copy_tree(std::move(collision_request));
    CK_CHECK(collision_created.value());
    if (!collision_created.value()) return;
    auto collision_job = std::move(collision_created).take_value();
    for (unsigned step = 0; step != 1000 && collision_job->state() != OperationExecutionState::Complete &&
         collision_job->state() != OperationExecutionState::Failed &&
         collision_job->state() != OperationExecutionState::Cancelled &&
         collision_job->state() != OperationExecutionState::NeedsReconciliation; ++step)
        collision_job->advance();
    CK_CHECK(collision_job->state() == OperationExecutionState::Failed);
    CK_CHECK(fs::is_regular_file(destination_path / "source-copy" / "root.bin"));
    CK_CHECK(fs::is_directory(destination_path / ".ck-tree-staging-74000000000000000000000000000000-0"));
}

CK_TEST(native_operation_move_tree_publishes_then_qualifies_reverse_source_retirement) {
    const ck::testing::ScratchDirectory scratch("files-native-operation-move-tree",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto source_path = scratch.path() / "source";
    const auto destination_path = scratch.path() / "destination";
    fs::create_directory(source_path);
    fs::create_directory(destination_path);
    fs::create_directory(source_path / "nested");
    {
        std::ofstream first(source_path / "root.bin", std::ios::binary);
        first << "recursive move root payload";
        std::ofstream second(source_path / "nested" / "child.bin", std::ios::binary);
        second << "recursive move child payload";
    }

    NativeFileSystem filesystem;
    auto fixture_root = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    auto root_source = take(HeldChild::observe_with_access(fixture_root, name("source"), EntryAccess::MetadataOnly));
    auto source_root = std::shared_ptr<DirectoryCursor>(take(filesystem.open_directory(source_path)));
    auto source_native_path = take(NativeAbsolutePath::from_host(source_path));
    RecursiveInventoryJob inventory(source_root, source_native_path, InventoryMountPolicy::StayOnRootMount,
                                    InventoryLinkPolicy::PreserveLinks, {}, system_monotonic_clock(), root_source);
    for (unsigned step = 0; step != 200 && inventory.state() != RecursiveInventoryState::Complete &&
         inventory.state() != RecursiveInventoryState::Failed; ++step)
        inventory.advance();
    CK_CHECK(inventory.state() == RecursiveInventoryState::Complete);
    if (inventory.state() != RecursiveInventoryState::Complete) return;
    auto completed = take(inventory.completed_sources());
    auto destination = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(destination_path)));

    NativeOperationCopyTreeRequest request(name("source-move"));
    request.inventory = std::move(completed);
    request.destination = destination;
    request.journal_root = take(NativeAbsolutePath::from_host(scratch.path()));
    request.journal_id.bytes[0] = std::byte{0x75};
    request.policy.allow_unhandled_metadata_loss = true;
    request.policy.copy_limits.chunk_bytes = 4;
    request.policy.copy_limits.max_bytes = 1024;
    request.operation = FileOperation::Move;
    request.open_journal = [](const NativeAbsolutePath& root, const NativeName& basename, bool create_new) {
        return open_native_operation_journal_file(root, basename, create_new);
    };
    auto created = create_native_operation_copy_tree(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 1200 && job->state() != OperationExecutionState::Complete &&
         job->state() != OperationExecutionState::Failed &&
         job->state() != OperationExecutionState::Cancelled &&
         job->state() != OperationExecutionState::RetainedSource &&
         job->state() != OperationExecutionState::NeedsReconciliation; ++step)
        job->advance();

    CK_CHECK(fs::is_regular_file(destination_path / "source-move" / "root.bin"));
    CK_CHECK(fs::is_regular_file(destination_path / "source-move" / "nested" / "child.bin"));
#if defined(_WIN32)
    CK_CHECK(job->state() == OperationExecutionState::Complete);
    job.reset();
    const auto source_status = filesystem.inspect_basic(source_path);
    CK_CHECK(!source_status.value());
#else
    CK_CHECK(job->state() == OperationExecutionState::RetainedSource);
    CK_CHECK(fs::is_regular_file(source_path / "root.bin"));
    CK_CHECK(fs::is_regular_file(source_path / "nested" / "child.bin"));
#endif
}

CK_TEST(native_operation_staging_publication_is_atomic_no_replace_and_identity_checked) {
    const ck::testing::ScratchDirectory scratch("files-operation-publication", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto staging = take(directory->create_child_file(name("staging.bin")));
    const std::array<std::byte, 4> bytes{std::byte{'s'}, std::byte{'a'}, std::byte{'f'}, std::byte{'e'}};
    CK_CHECK(take(staging->write_content(0, bytes)) == bytes.size());
    CK_CHECK(take(staging->flush_content()));
    CK_CHECK(take(staging->publish_noreplace(*directory, name("published.bin"))));
    CK_CHECK(!fs::exists(scratch.path() / "staging.bin") && fs::exists(scratch.path() / "published.bin"));
    const auto published = take(directory->open_child(name("published.bin"), EntryAccess::ContentRead));
    const auto read = take(published->read_content({16}, 0));
    CK_CHECK(read.complete && read.bytes == std::vector<std::byte>(bytes.begin(), bytes.end()));

    auto second = take(directory->create_child_file(name("second.bin")));
    CK_CHECK(take(second->write_content(0, bytes)) == bytes.size());
    CK_CHECK(take(second->flush_content()));
    const auto occupied = second->publish_noreplace(*directory, name("published.bin"));
    CK_CHECK(!occupied.value() && occupied.state() == ObservationState::Changed && fs::exists(scratch.path() / "second.bin"));

    auto substituted = take(directory->create_child_file(name("substituted.bin")));
    CK_CHECK(take(substituted->write_content(0, bytes)) == bytes.size());
    CK_CHECK(take(substituted->flush_content()));
    fs::rename(scratch.path() / "substituted.bin", scratch.path() / "substituted.retired");
    {
        std::ofstream replacement(scratch.path() / "substituted.bin", std::ios::binary);
        replacement << "replacement";
    }
    const auto changed = substituted->publish_noreplace(*directory, name("substituted.published"));
    CK_CHECK(!changed.value() && changed.state() == ObservationState::Changed &&
             fs::exists(scratch.path() / "substituted.retired") && fs::exists(scratch.path() / "substituted.bin"));
}

CK_TEST(native_operation_source_retirement_refuses_unqualified_posix_compare_delete) {
    const ck::testing::ScratchDirectory scratch("files-operation-retirement-boundary", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto source_path = scratch.path() / "source.bin";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "source remains until a qualified retirement adapter exists";
    }
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto source = take(directory->open_child(name("source.bin"), EntryAccess::ContentRead));
    const auto metadata = take(source->inspect_basic());
    CK_CHECK(metadata.identity.value());
    if (!metadata.identity.value()) return;
    const auto retired = directory->retire_child(name("source.bin"), *metadata.identity.value());
#if defined(_WIN32)
    // The Windows handle-disposition adapter is qualified separately on the
    // mandatory workstation; this host-side source test is POSIX-only.
    CK_CHECK(retired.value() || retired.state() != ObservationState::Unsupported);
#else
    CK_CHECK(!retired.value() && retired.state() == ObservationState::Unsupported);
    CK_CHECK(fs::exists(source_path));
#endif
}

CK_TEST(native_operation_rename_boundary_uses_qualified_native_no_replace) {
    const ck::testing::ScratchDirectory scratch("files-operation-rename-boundary", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto source_path = scratch.path() / "source.bin";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "source is renamed only through the qualified native identity protocol";
    }
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto source = take(directory->open_child(name("source.bin"), EntryAccess::ContentRead));
    const auto metadata = take(source->inspect_basic());
    CK_CHECK(metadata.identity.value());
    if (!metadata.identity.value()) return;
    const auto renamed = directory->rename_child_noreplace(name("source.bin"), *metadata.identity.value(),
                                                            *directory, name("renamed.bin"));
    CK_CHECK(renamed.value());
    CK_CHECK(!fs::exists(source_path) && fs::exists(scratch.path() / "renamed.bin"));

    const auto second_path = scratch.path() / "second.bin";
    {
        std::ofstream output(second_path, std::ios::binary);
        output << "collision remains untouched";
    }
    auto second = take(directory->open_child(name("second.bin"), EntryAccess::ContentRead));
    const auto second_metadata = take(second->inspect_basic());
    CK_CHECK(second_metadata.identity.value());
    if (!second_metadata.identity.value()) return;
    const auto collision = directory->rename_child_noreplace(name("second.bin"), *second_metadata.identity.value(),
                                                              *directory, name("renamed.bin"));
    CK_CHECK(!collision.value() && collision.state() == ObservationState::Changed);
    CK_CHECK(fs::exists(second_path) && fs::exists(scratch.path() / "renamed.bin"));
}

CK_TEST(native_operation_case_only_rename_requires_native_identity_proof) {
    const ck::testing::ScratchDirectory scratch("files-operation-case-rename-boundary", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto source_path = scratch.path() / "Source.bin";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "case-only rename remains gated until native proof exists";
    }
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto source = take(directory->open_child(name("Source.bin"), EntryAccess::ContentRead));
    const auto metadata = take(source->inspect_basic());
    CK_CHECK(metadata.identity.value());
    if (!metadata.identity.value()) return;
    const auto renamed = directory->rename_child_case_only(name("Source.bin"), *metadata.identity.value(),
                                                            name("source.bin"));
#if defined(_WIN32)
    CK_CHECK(renamed.value() || renamed.state() != ObservationState::Unsupported);
#elif defined(__APPLE__)
    CK_CHECK(renamed.value() || renamed.state() == ObservationState::Unsupported);
#else
    CK_CHECK(!renamed.value() && renamed.state() == ObservationState::Unsupported);
    CK_CHECK(fs::exists(source_path) && !fs::exists(scratch.path() / "source.bin"));
#endif
}

CK_TEST(native_operation_folder_creation_is_exclusive_and_returns_a_held_directory) {
    const ck::testing::ScratchDirectory scratch("files-operation-folder-creation", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto child = take(directory->create_child_directory(name("created")));
    const auto metadata = take(child->inspect_basic());
    CK_CHECK(metadata.kind.value() && *metadata.kind.value() == ObjectKind::Directory);
    CK_CHECK(fs::is_directory(scratch.path() / "created"));
    const auto existing = directory->create_child_directory(name("created"));
    CK_CHECK(!existing.value() && existing.state() == ObservationState::Changed);

    fs::rename(scratch.path() / "created", scratch.path() / "retired");
    fs::create_directory(scratch.path() / "created");
    const auto held_metadata = take(child->inspect_basic());
    CK_CHECK(held_metadata.kind.value() && *held_metadata.kind.value() == ObjectKind::Directory);
    CK_CHECK(fs::is_directory(scratch.path() / "retired") && fs::is_directory(scratch.path() / "created"));

    {
        std::ofstream output(scratch.path() / "occupied");
        output << "preserve this file";
    }
    const auto occupied = directory->create_child_directory(name("occupied"));
    CK_CHECK(!occupied.value() && occupied.state() == ObservationState::Changed);
    std::ifstream input(scratch.path() / "occupied");
    std::string content;
    std::getline(input, content);
    CK_CHECK(content == "preserve this file");
}

CK_TEST(native_operation_delete_tree_retires_only_the_frozen_tree) {
    const ck::testing::ScratchDirectory scratch("files-native-operation-delete-tree",
                                                CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
#if !defined(_WIN32)
    fs::permissions(scratch.path(), fs::perms::owner_all, fs::perm_options::replace);
#endif
    const auto source_path = scratch.path() / "source";
    fs::create_directory(source_path);
    fs::create_directory(source_path / "nested");
    {
        std::ofstream root_file(source_path / "root.bin", std::ios::binary);
        root_file << "delete root";
        std::ofstream nested_file(source_path / "nested" / "child.bin", std::ios::binary);
        nested_file << "delete child";
    }

    NativeFileSystem filesystem;
    auto fixture_root = std::shared_ptr<const DirectoryCursor>(take(filesystem.open_directory(scratch.path())));
    auto root_source = take(HeldChild::observe_with_access(fixture_root, name("source"), EntryAccess::MetadataOnly));
    auto source_root = std::shared_ptr<DirectoryCursor>(take(filesystem.open_directory(source_path)));
    auto source_native_path = take(NativeAbsolutePath::from_host(source_path));
    RecursiveInventoryJob inventory(source_root, source_native_path, InventoryMountPolicy::StayOnRootMount,
                                    InventoryLinkPolicy::PreserveLinks, {}, system_monotonic_clock(), root_source);
    for (unsigned step = 0; step != 200 && inventory.state() != RecursiveInventoryState::Complete &&
         inventory.state() != RecursiveInventoryState::Failed; ++step)
        inventory.advance();
    CK_CHECK(inventory.state() == RecursiveInventoryState::Complete);
    if (inventory.state() != RecursiveInventoryState::Complete) return;
    auto completed = take(inventory.completed_sources());

    auto journal_file = take(open_native_operation_journal_file(
        take(NativeAbsolutePath::from_host(scratch.path())), name(".ck-delete-journal"), true));
    NativeOperationDeleteTreeRequest request;
    request.inventory = std::move(completed);
    request.journal_file = std::shared_ptr<OperationJournalFile>(std::move(journal_file));
    request.journal_id.bytes[0] = std::byte{0x76};
    auto created = NativeOperationDeleteTreeJob::create(std::move(request));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 200 && job->state() != OperationExecutionState::Complete &&
         job->state() != OperationExecutionState::Failed &&
         job->state() != OperationExecutionState::Cancelled &&
         job->state() != OperationExecutionState::NeedsReconciliation; ++step)
        job->advance();
#if defined(_WIN32)
    const auto final_state = job->state();
    job.reset();
    root_source.reset();
    source_root.reset();
    fixture_root.reset();
    CK_CHECK(final_state == OperationExecutionState::Complete);
    CK_CHECK(!fs::exists(source_path));
#else
    CK_CHECK(job->state() == OperationExecutionState::Failed);
    CK_CHECK(fs::exists(source_path) && fs::exists(source_path / "nested" / "child.bin"));
#endif
}
