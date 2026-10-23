// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "directory_cursor.hpp"
#include "name_limits.hpp"
#include "windows_name.hpp"

namespace ck::files {
struct DirectoryEntryProbeLimits {
    std::size_t component_bytes = 64 * 1024;
    static constexpr std::size_t hard_component_bytes = 1024 * 1024;
};
// Worker-owned, immutable observations of one native component through a held
// directory. Available(record) does not mean available name or permission.
// Fields may fail independently. No pathname, display conversion, case-folding,
// implicit follow, mutation, reservation or snapshot guarantee is involved.
// Creating/refreshing and destroying the native owners belong to the worker.
class DirectoryEntryProbe final {
public:
    using Pointer = std::unique_ptr<const DirectoryEntryProbe>;
    DirectoryEntryProbe(const DirectoryEntryProbe&) = delete;
    DirectoryEntryProbe& operator=(const DirectoryEntryProbe&) = delete;
    static Observation<Pointer> inspect(std::shared_ptr<const DirectoryCursor> directory, const NativeName& name,
        ChildLookupMode mode = ChildLookupMode::NativeDefault, DirectoryEntryProbeLimits limits = {}, std::stop_token stop = {});
    // Uses the same held directory, never an original pathname. Old observations
    // and any opened child remain independently owned until their record retires.
    // Cooperative cancellation returns Unknown/InterruptedRead with operation
    // entry_probe.cancelled and no partial record; native calls can still block.
    Observation<Pointer> refresh(std::stop_token stop = {}) const;
    const NativeName& requested() const noexcept { return requested_; }
    ChildLookupMode mode() const noexcept { return mode_; }
    const Observation<LocationInfo>& location() const noexcept { return location_; }
    const Observation<ComponentLengthAssessment>& length() const noexcept { return length_; }
    const std::optional<Win32NameCheck>& win32_conventions() const noexcept { return win32_; }
    const Observation<ChildLookup>& lookup() const noexcept { return lookup_; }
    const Observation<BasicMetadata>& existing_metadata() const noexcept { return metadata_; }
private:
    DirectoryEntryProbe(std::shared_ptr<const DirectoryCursor> directory, NativeName name,
        ChildLookupMode mode, DirectoryEntryProbeLimits limits);
    std::shared_ptr<const DirectoryCursor> directory_;
    NativeName requested_;
    ChildLookupMode mode_;
    DirectoryEntryProbeLimits limits_;
    Observation<LocationInfo> location_;
    Observation<ComponentLengthAssessment> length_;
    std::optional<Win32NameCheck> win32_;
    Observation<ChildLookup> lookup_;
    Observation<BasicMetadata> metadata_;
};
} // namespace ck::files
