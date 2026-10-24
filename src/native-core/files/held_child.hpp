// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_cursor.hpp"

namespace ck::files {
// An owning observation of one basename through one held directory. The child
// handle outlives renames/unlink, preventing an ID from being recycled between
// successive qualified observations. Does not freeze data or directory membership.
// All factories/refreshes do native I/O and belong on the cursor's owning worker.
class HeldChild final {
public:
    using Pointer = std::shared_ptr<const HeldChild>;
    HeldChild(const HeldChild&) = delete;
    HeldChild& operator=(const HeldChild&) = delete;
    static Observation<Pointer> observe(std::shared_ptr<const DirectoryCursor> parent, NativeName name, std::stop_token stop = {});
    Observation<Pointer> refresh(std::stop_token stop = {}) const;
    const Observation<ObjectIdentity>& parent_identity() const noexcept { return parent_->identity(); }
    const NativeName& name() const noexcept { return name_; }
    const BasicMetadata& metadata() const noexcept { return metadata_; }
    // Fresh metadata from the retained child, never a name reopen. Worker only;
    // this observes the held object, not current membership of parent/name.
    Observation<BasicMetadata> inspect_basic() const;
    const Observation<IdentityComparison>& previous_continuity() const noexcept { return previous_continuity_; }
    // A later live observation in the same unbroken held-object family, with
    // the same parent/basename. Reusing a cached snapshot is never a refresh.
    bool continues(const HeldChild& previous) const noexcept;
private:
    struct Family {};
    HeldChild(std::shared_ptr<const DirectoryCursor> parent, NativeName name,
        std::shared_ptr<const OpenedEntry> child, BasicMetadata metadata,
        std::shared_ptr<const Family> family, std::uint64_t revision, Observation<IdentityComparison> continuity = {});
    std::shared_ptr<const DirectoryCursor> parent_;
    NativeName name_;
    std::shared_ptr<const OpenedEntry> child_;
    BasicMetadata metadata_;
    std::shared_ptr<const Family> family_;
    std::uint64_t revision_;
    Observation<IdentityComparison> previous_continuity_;
};
} // namespace ck::files
