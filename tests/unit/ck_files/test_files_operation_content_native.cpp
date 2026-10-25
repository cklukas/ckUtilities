// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "native_filesystem.hpp"
#include "native_operation_copy_job.hpp"
#include "scratch_directory.hpp"
#include "stream_copy_job.hpp"
#include "operation_journal_file.hpp"

#include <cvision/testing/cktest.hpp>

#include <array>
#include <fstream>
#include <filesystem>
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
    CK_CHECK(!fs::exists(destination_directory_path / "source.bin"));
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

CK_TEST(native_operation_rename_boundary_never_emulates_unqualified_posix_rename) {
    const ck::testing::ScratchDirectory scratch("files-operation-rename-boundary", CKTOOLS_FILES_PRIVATE_INDEX_TEST_ROOT);
    const auto source_path = scratch.path() / "source.bin";
    {
        std::ofstream output(source_path, std::ios::binary);
        output << "source remains until rename identity protocol is qualified";
    }
    NativeFileSystem filesystem;
    auto directory = take(filesystem.open_directory(scratch.path()));
    auto source = take(directory->open_child(name("source.bin"), EntryAccess::ContentRead));
    const auto metadata = take(source->inspect_basic());
    CK_CHECK(metadata.identity.value());
    if (!metadata.identity.value()) return;
    const auto renamed = directory->rename_child_noreplace(name("source.bin"), *metadata.identity.value(),
                                                            *directory, name("renamed.bin"));
#if defined(_WIN32)
    CK_CHECK(renamed.value() || renamed.state() != ObservationState::Unsupported);
#else
    CK_CHECK(!renamed.value() && renamed.state() == ObservationState::Unsupported);
    CK_CHECK(fs::exists(source_path) && !fs::exists(scratch.path() / "renamed.bin"));
#endif
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
