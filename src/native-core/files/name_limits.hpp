// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "native_name.hpp"
#include "location_info.hpp"

namespace ck::files {
enum class ComponentLengthUnit { Unknown, PosixBytes, WindowsUtf16Units };
// Comparison with one reported native bound, not name validity or permission.
// Directory identity is observation context, not a retained parent handle.
struct ComponentLengthAssessment {
    Observation<ObjectIdentity> directory;
    ComponentLengthUnit unit = ComponentLengthUnit::Unknown;
    std::uint64_t name_units = 0;
    std::uint64_t reported_maximum = 0;
    bool exceeds_reported_maximum() const noexcept { return name_units > reported_maximum; }
};
// No truncation, Unicode conversion/normalization or allocation by native limit.
// Does not assess complete paths, character restrictions, character-count limits,
// native request-buffer limits, collisions, quotas or publication capability.
// A within-bound result never establishes that the component can be created.
Observation<ComponentLengthAssessment> assess_component_length(const NativeName& name, const LocationInfo& location);
} // namespace ck::files
