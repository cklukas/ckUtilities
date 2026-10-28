// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "observation.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ck::vision {
enum class FilesVolumeArchiveSupport {
    Unknown,
    LiveReadable,
    SnapshotPreferred,
    Denied,
    Unsupported,
};

struct FilesVolumeArchiveCapability {
    FilesVolumeArchiveSupport support = FilesVolumeArchiveSupport::Unknown;
    bool snapshot_available = false;
    std::string diagnostic;
};

struct FilesVolumeRecord {
    std::string name;
    std::string mount_point;
    std::string filesystem;
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::uint64_t> free_bytes;
    bool read_only = false;
    // Native volume/device identity, not a display label or a path. It is
    // opaque to the UI and must be revalidated before an archive worker uses it.
    std::string identity;
    FilesVolumeArchiveCapability archive;
    // Exact native mount units when the platform path is not safely carried by
    // the display string. POSIX records leave this empty; Windows records use
    // the volume/mount UTF-16 units without a UTF-8 round trip.
    std::u16string native_mount_units;
};

enum class FilesVolumeReplyState { Ready, Failed, Cancelled, WorkerFault };

struct FilesVolumeReply {
    FilesVolumeReplyState state = FilesVolumeReplyState::Failed;
    std::vector<FilesVolumeRecord> volumes;
    std::optional<ck::files::UnavailableObservation> error;
};

using FilesVolumeEnumerator = std::function<ck::files::Observation<std::vector<FilesVolumeRecord>>()>;

// Read-only worker boundary for the volume/mount information view. Native
// enumeration and space queries never run on the UI thread and return only
// bounded value data. No row is an operation target and no elevation API is
// used or requested.
class FilesVolumeService final {
  public:
    explicit FilesVolumeService(std::function<void()> wake = {}, FilesVolumeEnumerator enumerator = {});
    ~FilesVolumeService();
    FilesVolumeService(const FilesVolumeService &) = delete;
    FilesVolumeService &operator=(const FilesVolumeService &) = delete;
    bool request();
    std::optional<FilesVolumeReply> take();
    void request_stop() noexcept;
    void join();
    bool running() const noexcept;

  private:
    struct State;
    std::unique_ptr<State> state_;
};

ck::files::Observation<std::vector<FilesVolumeRecord>> enumerate_native_volumes();
} // namespace ck::vision
