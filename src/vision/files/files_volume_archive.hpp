// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "files_volume_service.hpp"
#include "native_path.hpp"
#include "observation.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

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
    std::vector<FilesVolumeArchiveEntry> entries;
};

struct FilesVolumeArchiveWriteRequest final {
    FilesVolumeArchiveDocument document;
    std::filesystem::path destination_directory;
    std::string archive_name;
    std::uint64_t transaction_id = 1;
};

struct FilesVolumeArchiveWriteResult final {
    std::filesystem::path published_path;
    std::uint64_t logical_size = 0;
    std::uint64_t data_bytes = 0;
    std::size_t entry_count = 0;
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
};

enum class FilesVolumeArchiveSubmission {
    Accepted,
    Busy,
    Stopped,
    Invalid,
};

enum class FilesVolumeArchiveState {
    Complete,
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
