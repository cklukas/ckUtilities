// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_name_rules.hpp"
#include "monotonic_clock.hpp"
#include <array>
#include <stop_token>

namespace ck::files {
inline constexpr std::array inventory_directory_access{DirectoryAccess::List, DirectoryAccess::Search,
    DirectoryAccess::AddFile, DirectoryAccess::AddDirectory};
struct DirectoryInventoryData {
    Observation<ObjectIdentity> anchor;
    Observation<BasicMetadata> before, after;
    Observation<LocationInfo> location_before, location_after;
    std::array<Observation<DirectoryAccessCheck>, inventory_directory_access.size()> access;
    Observation<DirectoryFscryptPolicy> fscrypt_policy;
    Observation<DirectoryFscryptKeyStatus> fscrypt_key;
    Observation<IdentityComparison> directory_continuity;
    Observation<QualifiedDirectoryNameRule> name_rule;
    std::size_t variable_bytes = 0;
};
struct DirectoryInventoryLimits {
    // Filesystem identifiers + every retained diagnostic operation string.
    // Fixed-size observations/arrays are bounded separately, not charged here.
    std::size_t variable_bytes = 1024 * 1024;
    static constexpr std::size_t hard_variable_bytes = 8 * 1024 * 1024;
    MonotonicClock::Duration time_slice = std::chrono::milliseconds(20);
};
class DirectoryInventoryJob;
class CompletedDirectoryInventory final {
public:
    const DirectoryInventoryData& data() const noexcept { return *data_; }
    // Requires a qualified unchanged anchor/before/after directory first, then
    // compares this individual observation's owner. Same is not a capability.
    Observation<IdentityComparison> compare_owner(const Observation<ObjectIdentity>& owner) const;
    // Worker-only lifetime/query access; retiring the last owner may close the
    // native enumeration cursor. Do not pass this result to UI publication.
    std::shared_ptr<const DirectoryCursor> pin_directory() const { return directory_; }
private:
    friend class DirectoryInventoryJob;
    CompletedDirectoryInventory(std::shared_ptr<const DirectoryCursor> directory,
        std::unique_ptr<DirectoryInventoryData> data) noexcept : directory_(std::move(directory)), data_(std::move(data)) {}
    std::shared_ptr<const DirectoryCursor> directory_;
    std::unique_ptr<const DirectoryInventoryData> data_;
};
enum class DirectoryInventoryState { Collecting, Paused, Complete, Failed, Abandoned };
// Fixed query families through one held directory, no enumeration/path reopen,
// content reads, elevation, probes, creation or mutation. Unsupported/denied
// queries remain per-field observations. Completion is not a snapshot, complete
// preservation assessment, reservation, effective-operation permission or consent.
class DirectoryInventoryJob final {
public:
    using Result = std::shared_ptr<const CompletedDirectoryInventory>;
    static constexpr std::size_t steps_per_advance = 32;
    DirectoryInventoryJob(std::shared_ptr<const DirectoryCursor> directory, DirectoryInventoryLimits limits = {},
        std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    DirectoryInventoryJob(const DirectoryInventoryJob&) = delete;
    DirectoryInventoryJob& operator=(const DirectoryInventoryJob&) = delete;
    DirectoryInventoryState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    DirectoryInventoryState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    enum class Phase { Anchor, Before, LocationBefore, Access, Policy, Key, LocationAfter, After, Derive, Seal };
    template<class T> bool retain(Observation<T> result, Observation<T>& target);
    void release_staging() noexcept;
    void fail(UnavailableObservation error) noexcept;
    void invalid_reply();
    void step();
    std::shared_ptr<const DirectoryCursor> directory_;
    std::unique_ptr<DirectoryInventoryData> data_;
    DirectoryInventoryLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    Phase phase_ = Phase::Anchor;
    std::size_t next_access_ = 0;
    DirectoryInventoryState state_ = DirectoryInventoryState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};
} // namespace ck::files
