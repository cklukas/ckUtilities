// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_entry_probe.hpp"
#include "name_mapping.hpp"
#include "monotonic_clock.hpp"
#include <map>
#include <tuple>

namespace ck::files {
struct DestinationBatchLimits {
    std::size_t rows = 256;
    // Source + mapped strings; unique names additionally reserve an index key
    // and the probe/request context copies. Not a process-memory quota.
    std::size_t name_bytes = 4 * 1024 * 1024;
    std::size_t component_bytes = 64 * 1024;
    MonotonicClock::Duration time_slice = std::chrono::milliseconds(20);
    static constexpr std::size_t hard_rows = 4096, hard_name_bytes = 16 * 1024 * 1024;
};
struct DestinationEntryInspection {
    explicit DestinationEntryInspection(NativeName name) : source(std::move(name)) {}
    NativeName source;
    Observation<NativeName> mapped;
    // Every mapped entry points at the first identical mapped native component,
    // including itself. Duplicate rows share that representative's probe.
    std::optional<std::size_t> exact_name_leader;
    std::size_t exact_name_count = 0; // populated on leader only
    DirectoryEntryProbe::Pointer probe;
    std::optional<UnavailableObservation> probe_failure;
    // Set only for positive matches, including the representative itself once
    // another distinct mapped name reports the same scoped object ID.
    // No value is NOT proof of different objects or collision-free names.
    std::optional<std::size_t> reported_object_leader;
};
class DestinationBatchJob;
class CompletedDestinationBatch final {
public:
    const std::vector<DestinationEntryInspection>& entries() const noexcept { return entries_; }
    std::size_t exact_name_count(std::size_t row) const noexcept;
    const DirectoryEntryProbe* probe(std::size_t row) const noexcept;
    const UnavailableObservation* probe_failure(std::size_t row) const noexcept;
    std::optional<std::size_t> reported_object_leader(std::size_t row) const noexcept;
    // Retains the actual parent even when all lookups are Missing/unavailable.
    // Queries and final owner retirement belong on workers.
    std::shared_ptr<const DirectoryCursor> pin_directory() const { return root_directory_; }
    const std::vector<std::shared_ptr<const DirectoryCursor>>& parent_directories() const noexcept {
        return parent_directories_;
    }
private:
    friend class DestinationBatchJob;
    CompletedDestinationBatch(std::shared_ptr<const DirectoryCursor> root_directory,
        std::vector<std::shared_ptr<const DirectoryCursor>> parent_directories,
        std::vector<DestinationEntryInspection> entries) noexcept
        : root_directory_(std::move(root_directory)), parent_directories_(std::move(parent_directories)),
          entries_(std::move(entries)) {}
    const DestinationEntryInspection* leader(std::size_t row) const noexcept;
    std::shared_ptr<const DirectoryCursor> root_directory_;
    std::vector<std::shared_ptr<const DirectoryCursor>> parent_directories_;
    std::vector<DestinationEntryInspection> entries_;
};
enum class DestinationBatchState { Collecting, Paused, Complete, Failed, Abandoned };
// Worker-only bounded batch, not a frozen operation plan or UI publication job.
// Exact native-name duplicates and positive existing-object matches are separate
// evidence. Hard links are NOT proof of equivalent directory-entry names.
// Distinct missing names can still collide by native case/normalization/alias
// rules; this boundary does not infer equivalence or grant permission to create.
class DestinationBatchJob final {
public:
    using Result = std::shared_ptr<const CompletedDestinationBatch>;
    static constexpr std::size_t steps_per_advance = 256;
    DestinationBatchJob(std::shared_ptr<const DirectoryCursor> directory, std::vector<NativeName> names,
        NameEncoding destination_encoding, DestinationBatchLimits limits = {},
        std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    DestinationBatchJob(std::vector<std::shared_ptr<const DirectoryCursor>> parent_directories,
        std::vector<NativeName> names, NameEncoding destination_encoding, DestinationBatchLimits limits = {},
        std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    DestinationBatchJob(const DestinationBatchJob&) = delete;
    DestinationBatchJob& operator=(const DestinationBatchJob&) = delete;
    DestinationBatchState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    DestinationBatchState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    enum class Phase { Map, Probe, Group, CompareGroup, Seal };
    // Scope is deliberately not ordered or hashed. Exact key buckets are only
    // an index; compare_identity still qualifies every positive match.
    using IdentityKey = std::tuple<IdentityScheme, std::array<std::byte, 16>, std::array<std::byte, 16>, std::optional<std::uint64_t>>;
    void fail(UnavailableObservation error) noexcept;
    void release_staging() noexcept;
    bool retain_name(const NativeName& name);
    const ObjectIdentity* identity(std::size_t row) const noexcept;
    void step(std::stop_token stop);
    std::vector<std::shared_ptr<const DirectoryCursor>> parent_directories_;
    std::shared_ptr<const DirectoryCursor> root_directory_;
    std::vector<NativeName> names_;
    NameEncoding encoding_;
    DestinationBatchLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    std::vector<DestinationEntryInspection> entries_;
    std::map<NativeName, std::vector<std::size_t>, NativeNameLess> names_index_;
    std::map<IdentityKey, std::vector<std::size_t>> objects_;
    std::vector<std::size_t>* bucket_ = nullptr;
    std::size_t next_ = 0, candidate_ = 0, bytes_ = 0;
    Phase phase_ = Phase::Map;
    DestinationBatchState state_ = DestinationBatchState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};
} // namespace ck::files
