// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_filesystem.hpp"
#include "monotonic_clock.hpp"

namespace ck::files {
struct DirectoryListingLimits {
    std::size_t rows = 256;
    std::size_t name_bytes = 256 * 1024;
    std::size_t component_bytes = 64 * 1024;
    MonotonicClock::Duration time_slice = std::chrono::milliseconds(20);
};
struct ListedEntry {
    NativeName name;
    Observation<BasicMetadata> metadata;
};
struct DirectoryListingBatch {
    std::vector<ListedEntry> entries;
    DirectoryReadState state = DirectoryReadState::More;
    std::optional<UnavailableObservation> error; // exactly when Error
    bool time_slice_exhausted = false; // More after cooperative time yield
};
// Single-worker, read-only streaming discovery. Owns its cursor, not a path to
// reopen. Results are momentary observations, never frozen object references or
// write authority. Callers must bound retained batches/indexes separately.
class DirectoryListing final {
public:
    static Observation<std::unique_ptr<DirectoryListing>> open(const BasicFileSystem& filesystem,
        const std::filesystem::path& absolute_path, DirectoryListingLimits limits = {},
        std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    explicit DirectoryListing(std::unique_ptr<DirectoryCursor> cursor, DirectoryListingLimits limits = {},
        std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    DirectoryListing(const DirectoryListing&) = delete;
    DirectoryListing& operator=(const DirectoryListing&) = delete;
    // Bounded rows and native name storage per batch, plus at most one pending
    // component. At most limits.rows cursor reads (each itself bounded) per call.
    // One prepared child may survive a time yield, so slow opens make progress.
    // Cancellation closes it; completed inspections close before the next entry.
    // Metadata failures retain the
    // entry's name; enumeration/resource failures are terminal and never End.
    // Cancellation is checked between calls, not inside blocking OS operations.
    // Exceptions after consumption poison the stream before being rethrown.
    DirectoryListingBatch read(std::stop_token stop = {});
    const Observation<ObjectIdentity>& identity() const noexcept { return cursor_->identity(); }
    Observation<AdapterScope> source_scope() const { return cursor_->source_scope(); }
    // Worker-owned lifetime pin, not a frozen namespace or pathname attestation.
    // Enumeration remains owned here; const pins expose read-only native queries.
    std::shared_ptr<const DirectoryCursor> pin_directory() const { return cursor_; }
private:
    static void validate(DirectoryListingLimits limits);
    std::shared_ptr<DirectoryCursor> cursor_;
    DirectoryListingLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    std::unique_ptr<OpenedEntry> prepared_;
    std::optional<MonotonicClock::TimePoint> last_clock_reading_;
    std::optional<NativeName> pending_;
    bool ended_ = false;
    std::optional<UnavailableObservation> failure_;
};
} // namespace ck::files
