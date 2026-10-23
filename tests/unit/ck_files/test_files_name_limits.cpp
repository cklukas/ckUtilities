// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "name_limits.hpp"
#include "cvision/testing/cktest.hpp"
#include <limits>
#include <stdexcept>

namespace {
using namespace ck::files;
LocationInfo location(LocationQueryFamily family, std::uint64_t bound) {
    LocationInfo result; result.family = family;
    result.posix_name_max = result.windows_component_utf16_max = Observation<std::uint64_t>::available(bound);
    result.directory = Observation<ObjectIdentity>::unavailable(ObservationState::Denied, {ErrorDomain::Adapter, 42, "fake.parent"});
    return result;
}
template<class T> const T& required(const Observation<T>& result) {
    if (!result.value()) throw std::runtime_error("required length assessment unavailable");
    return *result.value();
}
}
CK_TEST(component_length_compares_exact_posix_bytes_including_invalid_unicode_without_conversion) {
    for (const auto family : {LocationQueryFamily::LinuxDescriptor, LocationQueryFamily::DarwinDescriptor}) {
        for (const std::uint64_t bound : {1ULL, 2ULL, 3ULL, 255ULL, std::numeric_limits<unsigned long long>::max()}) {
            const auto result = assess_component_length(*NativeName::from_posix("a\xff\n"), location(family, bound));
            CK_CHECK(required(result).unit == ComponentLengthUnit::PosixBytes && required(result).name_units == 3);
            CK_CHECK(required(result).reported_maximum == bound && required(result).exceeds_reported_maximum() == (3 > bound));
            CK_CHECK(required(result).directory.state() == ObservationState::Denied);
        }
    }
}
CK_TEST(component_length_counts_utf16_units_rather_than_scalars_bytes_or_display_columns) {
    for (const auto& text : {std::u16string(u"a\U0001f600"), std::u16string{u'a', char16_t(0xd800), u'b'}}) {
        const auto component = *NativeName::from_windows(text);
        const auto exact = assess_component_length(component, location(LocationQueryFamily::WindowsHandle, 3));
        const auto over = assess_component_length(component, location(LocationQueryFamily::WindowsHandle, 2));
        CK_CHECK(required(exact).unit == ComponentLengthUnit::WindowsUtf16Units && required(exact).name_units == 3);
        CK_CHECK(!required(exact).exceeds_reported_maximum() && required(over).exceeds_reported_maximum());
    }
}
CK_TEST(component_length_preserves_native_bound_failures_instead_of_inventing_defaults) {
    for (const auto family : {LocationQueryFamily::LinuxDescriptor, LocationQueryFamily::DarwinDescriptor, LocationQueryFamily::WindowsHandle}) {
        for (const auto state : {ObservationState::Unknown, ObservationState::Unsupported, ObservationState::Denied, ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
            auto info = location(family, 255); const NativeError error{ErrorDomain::Posix, 13, "fake.pathconf"};
            info.posix_name_max = info.windows_component_utf16_max = Observation<std::uint64_t>::unavailable(state, error);
            const auto component = family == LocationQueryFamily::WindowsHandle ? *NativeName::from_windows(u"a") : *NativeName::from_posix("a");
            const auto result = assess_component_length(component, info);
            CK_CHECK(result.state() == state && result.unavailable_detail()->error == error);
        }
    }
}
CK_TEST(component_length_rejects_zero_bounds_wrong_encodings_and_invalid_families) {
    const auto posix = *NativeName::from_posix("a"), windows = *NativeName::from_windows(u"a");
    CK_CHECK(assess_component_length(posix, location(LocationQueryFamily::Unknown, 255)).state() == ObservationState::Unknown);
    CK_CHECK(assess_component_length(posix, location(static_cast<LocationQueryFamily>(999), 255)).state() == ObservationState::Failed);
    CK_CHECK(assess_component_length(posix, location(LocationQueryFamily::WindowsHandle, 255)).state() == ObservationState::Failed);
    CK_CHECK(assess_component_length(windows, location(LocationQueryFamily::DarwinDescriptor, 255)).state() == ObservationState::Failed);
    CK_CHECK(assess_component_length(posix, location(LocationQueryFamily::LinuxDescriptor, 0)).state() == ObservationState::Failed);
}
