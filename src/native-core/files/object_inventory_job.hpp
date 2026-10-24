// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "opened_entry.hpp"
#include "monotonic_clock.hpp"
#include <array>
#include <set>
#include <stop_token>

namespace ck::files {
inline constexpr std::array inventory_security_parts{SecurityPart::LinuxAccessAcl, SecurityPart::LinuxDefaultAcl,
    SecurityPart::DarwinExtendedAcl, SecurityPart::WindowsAccess, SecurityPart::WindowsIntegrity, SecurityPart::WindowsFullDescriptor};
struct InventoryAttributeRead {
    std::size_t catalog_index = 0; // requested identifier in attributes, never display text
    Observation<AttributeValue> value;
};
struct ObjectInventoryData {
    Observation<BasicMetadata> before, after;
    Observation<StorageInfo> storage;
    Observation<FileIntegrityInfo> integrity;
    Observation<FileDataLayout> layout;
    Observation<LinkMetadata> link;
    Observation<FileLinkLimits> link_limits;
    Observation<DataChannelCatalog> channels;
    Observation<AttributeCatalog> attributes;
    std::vector<InventoryAttributeRead> attribute_values;
    std::array<Observation<NativeSecurityValue>, inventory_security_parts.size()> security;
    // Only the before/after identities of the pinned object. Same does not mean
    // unchanged bytes, metadata, membership or agreement of every query owner.
    Observation<IdentityComparison> identity_continuity;
};
struct ObjectInventoryLimits {
    InventoryLimits query; // passed unchanged; each native query defines its wire budget
    // Counts retained query observations + catalog descriptors + attribute reads.
    // Bytes count identifiers, binary values, link payload/decoded names. These
    // are resource bounds, not native wire sizes or a total process-memory quota.
    InventoryLimits retained{InventoryLimits::hard_native_bytes, InventoryLimits::hard_records};
    MonotonicClock::Duration time_slice = std::chrono::milliseconds(20);
};
class ObjectInventoryJob;
class CompletedObjectInventory final {
public:
    const ObjectInventoryData& data() const noexcept { return *data_; }
    // Compare an individual available payload's owner with the first held-object
    // observation. Unknown/denied identities never become Same. Must be checked
    // separately from query coverage before using any feature observation.
    Observation<IdentityComparison> compare_owner(const Observation<ObjectIdentity>& owner) const;
    // Worker-only query access. The immutable result pins this exact object;
    // retiring its last owner can close native handles and belongs on a worker.
    const OpenedEntry& object_for_worker() const noexcept { return *object_; }
private:
    friend class ObjectInventoryJob;
    CompletedObjectInventory(std::shared_ptr<const OpenedEntry> object, std::unique_ptr<ObjectInventoryData> data) noexcept
        : object_(std::move(object)), data_(std::move(data)) {}
    std::shared_ptr<const OpenedEntry> object_;
    std::unique_ptr<const ObjectInventoryData> data_;
};
enum class ObjectInventoryJobState { Collecting, Paused, Complete, Failed, Abandoned };
// Bounded worker collection through one already-open object. No path reopening,
// link traversal, main-data/named-stream content API, elevation or mutation.
// Attribute values are read explicitly and may themselves carry additional data.
// Query failures remain per-field observations; malformed replies, resource and
// clock errors fail the collection. Completion means all query families were
// attempted, not a snapshot, full preservation inventory or operation capability.
class ObjectInventoryJob final {
public:
    using Result = std::shared_ptr<const CompletedObjectInventory>;
    static constexpr std::size_t steps_per_advance = 256;
    ObjectInventoryJob(std::unique_ptr<OpenedEntry> object, ObjectInventoryLimits limits = {},
        std::shared_ptr<const MonotonicClock> clock = system_monotonic_clock());
    ObjectInventoryJob(const ObjectInventoryJob&) = delete;
    ObjectInventoryJob& operator=(const ObjectInventoryJob&) = delete;
    ObjectInventoryJobState advance(std::stop_token stop = {});
    // Completed native queries commit privately before a time/cancel yield;
    // resume never repeats them. No result is exposed until all phases finish.
    bool abandon() noexcept;
    ObjectInventoryJobState state() const noexcept { return state_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return failure_; }
    Observation<Result> completed() const;
private:
    enum class Phase { Before, Storage, Integrity, Layout, CheckLayout, Link, LinkLimits, Channels, CheckChannels, Attributes, CheckAttributes, Values, Security, After, Seal };
    struct FeatureLess {
        bool operator()(const NativeFeatureIdentifier& a, const NativeFeatureIdentifier& b) const noexcept;
    };
    bool retain(std::size_t bytes, std::size_t records);
    bool retain_identifier(const NativeFeatureIdentifier& identifier, std::size_t records);
    bool invalid_reply();
    void fail(UnavailableObservation error) noexcept;
    void release_staging() noexcept;
    void step(std::stop_token stop);
    std::shared_ptr<const OpenedEntry> object_;
    std::unique_ptr<ObjectInventoryData> data_;
    ObjectInventoryLimits limits_;
    std::shared_ptr<const MonotonicClock> clock_;
    std::optional<MonotonicClock::TimePoint> last_reading_;
    std::set<NativeFeatureIdentifier, FeatureLess> identifiers_;
    Phase phase_ = Phase::Before;
    std::size_t next_ = 0, bytes_ = 0, records_ = 0;
    ObjectInventoryJobState state_ = ObjectInventoryJobState::Collecting;
    std::optional<UnavailableObservation> failure_;
    Result completed_;
};
} // namespace ck::files
