// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "name_limits.hpp"

namespace ck::files {
Observation<ComponentLengthAssessment> assess_component_length(const NativeName& name, const LocationInfo& location) {
    using Result = Observation<ComponentLengthAssessment>;
    const auto invalid = [](const char* operation) {
        return Result::unavailable(ObservationState::Failed,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), operation});
    };
    if (!name.is_valid()) return invalid("component_length.name");
    const Observation<std::uint64_t>* bound = nullptr;
    ComponentLengthUnit unit;
    std::size_t count = 0;
    switch (location.family) {
    case LocationQueryFamily::LinuxDescriptor:
    case LocationQueryFamily::DarwinDescriptor:
        if (!name.posix_bytes()) return invalid("component_length.encoding");
        count = name.posix_bytes()->size(); unit = ComponentLengthUnit::PosixBytes;
        bound = &location.posix_name_max;
        break;
    case LocationQueryFamily::WindowsHandle:
        if (!name.windows_units()) return invalid("component_length.encoding");
        count = name.windows_units()->size(); unit = ComponentLengthUnit::WindowsUtf16Units;
        bound = &location.windows_component_utf16_max;
        break;
    case LocationQueryFamily::Unknown:
        return Result::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::UnsupportedQuery), "component_length.family"});
    default: return invalid("component_length.family");
    }
    if (!bound->value()) {
        const auto& error = *bound->unavailable_detail();
        return Result::unavailable(error.state, error.error);
    }
    if (*bound->value() == 0) return Result::unavailable(ObservationState::Failed,
        {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidNativeResult), "component_length.zero_bound"});
    static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
    return Result::available({location.directory, unit, static_cast<std::uint64_t>(count), *bound->value()});
}
} // namespace ck::files
