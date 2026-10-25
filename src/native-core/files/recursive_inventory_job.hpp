// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "directory_cursor.hpp"
#include "frozen_inventory.hpp"
#include "monotonic_clock.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace ck::files {

struct RecursiveInventoryLimits final {
    FrozenInventoryLimits inventory{};
    std::size_t max_depth = 1024;
    static constexpr std::size_t hard_max_depth = 4096;
    MonotonicClock::Duration time_slice = std::chrono::milliseconds(20);
};

enum class RecursiveInventoryState { Collecting, Paused, Complete, Failed, Abandoned };

// Worker-owned, read-only recursive traversal. The root cursor and every
// child cursor remain held by this job; no path is reopened and no display
// string is parsed. It seals a FrozenInventory only after every enumerated
// entry has an exact path, qualified identity, kind, mount and logical size.
// Links are recorded without traversal by default. FollowLinks is deliberately
// refused unless a future adapter supplies explicit target evidence; this job
// never follows a link as an implicit workaround.
class RecursiveInventoryJob final {
public:
    using Result = FrozenInventory::Pointer;

    RecursiveInventoryJob(std::shared_ptr<DirectoryCursor> root,
        NativeAbsolutePath root_path, InventoryMountPolicy mount_policy,
        InventoryLinkPolicy link_policy, RecursiveInventoryLimits limits = {},
        std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    RecursiveInventoryJob(const RecursiveInventoryJob&) = delete;
    RecursiveInventoryJob& operator=(const RecursiveInventoryJob&) = delete;

    RecursiveInventoryState advance(std::stop_token stop = {});
    bool abandon() noexcept;
    RecursiveInventoryState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;

private:
    struct Frame final {
        std::shared_ptr<DirectoryCursor> directory;
        NativeAbsolutePath path;
        ObjectIdentity identity;
        std::optional<NativeName> pending;
        std::size_t depth = 0;
    };

    enum class Phase { Root, Enumerate, Seal };

    static bool link_kind(ObjectKind kind) noexcept;
    bool add_metadata(const NativeAbsolutePath& path, const BasicMetadata& metadata,
        LinkTraversalEvidence link);
    bool fail(UnavailableObservation detail) noexcept;
    bool fail(AdapterError code, const char* operation,
        ObservationState state = ObservationState::Failed) noexcept;
    bool stop_or_yield(std::stop_token stop, CooperativeTimeSlice& slice);
    void release_staging() noexcept;

    std::shared_ptr<DirectoryCursor> root_;
    NativeAbsolutePath root_path_;
    InventoryLinkPolicy link_policy_;
    RecursiveInventoryLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    std::unique_ptr<FrozenInventoryBuilder> builder_;
    std::vector<Frame> frames_;
    std::vector<ObjectIdentity> directory_identities_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    Phase phase_ = Phase::Root;
    RecursiveInventoryState state_ = RecursiveInventoryState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};

} // namespace ck::files
