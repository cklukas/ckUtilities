// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "destination_batch_job.hpp"
#include "directory_name_rules.hpp"
namespace ck::files {
enum class OtherNameGroups { None, DistinctUnderReportedRules };
struct DestinationNameSummary {
    std::size_t mapped_rows = 0, unmapped_rows = 0, groups = 0;
    std::size_t duplicate_groups = 0, rows_in_duplicate_groups = 0;
};
struct DestinationNameRow {
    std::optional<std::size_t> exact_leader;
    std::size_t exact_count = 0, other_groups = 0;
    Observation<OtherNameGroups> against_other_groups;
};
struct DestinationNamesLimits {
    std::size_t rows = DestinationBatchLimits::hard_rows;
    std::size_t component_bytes = 64 * 1024;
    MonotonicClock::Duration time_slice = std::chrono::milliseconds(20);
};
class DestinationNamesJob;
class CompletedDestinationNames final {
public:
    const DestinationBatchJob::Result& batch() const noexcept { return batch_; }
    const DestinationNameSummary& summary() const noexcept { return summary_; }
    const Observation<LocationInfo>& before() const noexcept { return before_; }
    const Observation<LocationInfo>& after() const noexcept { return after_; }
    const Observation<QualifiedDirectoryNameRule>& rule() const noexcept { return rule_; }
    const Observation<OtherNameGroups>& other_groups() const noexcept { return other_groups_; }
    // O(1), no native I/O or name scans. Mapping failures remain per row.
    Observation<DestinationNameRow> row(std::size_t index) const;
    Observation<ComponentNameComparison> compare(std::size_t left, std::size_t right) const;
private:
    friend class DestinationNamesJob;
    CompletedDestinationNames(DestinationBatchJob::Result batch, DestinationNameSummary summary,
        Observation<LocationInfo> before, Observation<LocationInfo> after,
        Observation<QualifiedDirectoryNameRule> rule, Observation<OtherNameGroups> other_groups) noexcept
        : batch_(std::move(batch)), summary_(summary), before_(std::move(before)), after_(std::move(after)),
          rule_(std::move(rule)), other_groups_(std::move(other_groups)) {}
    DestinationBatchJob::Result batch_;
    DestinationNameSummary summary_;
    Observation<LocationInfo> before_, after_;
    Observation<QualifiedDirectoryNameRule> rule_;
    Observation<OtherNameGroups> other_groups_;
};
enum class DestinationNamesState { Collecting, Paused, Complete, Failed, Abandoned };
// Worker-only assessment over a sealed destination batch. O(rows) work/storage
// bounds; no quadratic pair table, new child lookups, path reopen or mutation.
// Exact groups, native name rules and existing-object aliases remain separate.
// None means no OTHER mapped groups, not acceptance, no conflicts or permission.
// Completed queries commit before cancellation/time yields; native calls may
// block. Issued results retain the batch/parent and retire on workers.
class DestinationNamesJob final {
public:
    using Result = std::shared_ptr<const CompletedDestinationNames>;
    static constexpr std::size_t steps_per_advance = 256;
    DestinationNamesJob(DestinationBatchJob::Result batch, DestinationNamesLimits limits = {},
        std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    DestinationNamesJob(const DestinationNamesJob&) = delete;
    DestinationNamesJob& operator=(const DestinationNamesJob&) = delete;
    DestinationNamesState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    DestinationNamesState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    enum class Phase { Validate, Before, CheckBefore, After, CheckAfter, Seal };
    void fail(UnavailableObservation error) noexcept;
    void release_staging() noexcept;
    void set_coverage();
    void step();
    DestinationBatchJob::Result batch_;
    DestinationNamesLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    DestinationNameSummary summary_;
    std::optional<NameEncoding> encoding_;
    Observation<LocationInfo> before_, after_;
    Observation<QualifiedDirectoryNameRule> rule_;
    Observation<OtherNameGroups> other_groups_;
    std::size_t next_ = 0;
    Phase phase_ = Phase::Validate;
    DestinationNamesState state_ = DestinationNamesState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};
} // namespace ck::files
