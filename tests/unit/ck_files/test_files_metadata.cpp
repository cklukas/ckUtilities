// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_metadata.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "cvision/testing/cktest.hpp"

namespace {
using namespace ck::files;

struct ThrowingCopy {
    static inline bool should_throw = false;
    int number = 0;
    explicit ThrowingCopy(int value) : number(value) {}
    ThrowingCopy(const ThrowingCopy& other) : number(other.number) {
        if (should_throw) throw std::runtime_error("injected allocation/copy failure");
    }
    ThrowingCopy& operator=(const ThrowingCopy& other) {
        if (should_throw) throw std::runtime_error("injected copy failure");
        number = other.number;
        return *this;
    }
    ThrowingCopy(ThrowingCopy&&) noexcept = default;
    ThrowingCopy& operator=(ThrowingCopy&&) noexcept = default;
};

ObjectIdentity identity(const AdapterScope& scope) {
    return {scope, IdentityScheme::WindowsFileId128, NativeIdentifier::from_u64(7), NativeIdentifier::from_u64(9), std::nullopt};
}
}

CK_TEST(observations_distinguish_zero_empty_unknown_and_every_failure_state) {
    const Observation<std::uint64_t> unknown;
    CK_CHECK(unknown.state() == ObservationState::Unknown);
    CK_CHECK(unknown.value() == nullptr);
    CK_CHECK(unknown.unavailable_detail()->error.domain == ErrorDomain::None);
    const auto zero = Observation<std::uint64_t>::available(0);
    CK_CHECK(zero.state() == ObservationState::Available && zero.value() && *zero.value() == 0);
    CK_CHECK(zero.unavailable_detail() == nullptr);
    const auto empty = Observation<std::vector<int>>::available({});
    CK_CHECK(empty.value() && empty.value()->empty());
    for (auto state : {ObservationState::Unsupported, ObservationState::Unknown, ObservationState::Denied,
                       ObservationState::Gone, ObservationState::Changed, ObservationState::Failed}) {
        const NativeError error{ErrorDomain::Win32, 4'294'967'295LL, "native.operation"};
        const auto result = Observation<int>::unavailable(state, error);
        CK_CHECK(result.state() == state && result.value() == nullptr);
        CK_CHECK(result.unavailable_detail()->error == error);
    }
    for (auto invalid : {ObservationState::Available, static_cast<ObservationState>(255)}) {
        bool rejected = false;
        try { (void)Observation<int>::unavailable(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        CK_CHECK(rejected);
    }
}

CK_TEST(observation_copy_failure_cannot_turn_unavailable_into_valueless_success) {
    auto target = Observation<ThrowingCopy>::unavailable(ObservationState::Gone);
    const auto source = Observation<ThrowingCopy>::available(ThrowingCopy{42});
    ThrowingCopy::should_throw = true;
    bool rejected = false;
    try { target = source; }
    catch (const std::runtime_error&) { rejected = true; }
    ThrowingCopy::should_throw = false;
    CK_CHECK(rejected);
    CK_CHECK(target.state() == ObservationState::Gone && target.value() == nullptr);
    target = source;
    CK_CHECK(target.value() && target.value()->number == 42);
}

CK_TEST(object_identity_uses_the_full_identifier_and_scope) {
    AdapterScope scope;
    const auto original = identity(scope);
    auto changed = original;
    CK_CHECK(compare_identity(original, changed) == IdentityComparison::Same);
    changed.object.bytes[15] = std::byte{1};
    CK_CHECK(compare_identity(original, changed) == IdentityComparison::Different);
    changed = original;
    changed.volume.bytes[15] = std::byte{1};
    CK_CHECK(compare_identity(original, changed) == IdentityComparison::Different);
    changed = identity(AdapterScope{});
    CK_CHECK(compare_identity(original, changed) == IdentityComparison::Unknown);
    changed = original;
    changed.scheme = IdentityScheme::WindowsFileIndex64;
    CK_CHECK(compare_identity(original, changed) == IdentityComparison::Unknown);
    changed.scheme = static_cast<IdentityScheme>(255);
    CK_CHECK(compare_identity(changed, changed) == IdentityComparison::Unknown);
}

CK_TEST(identity_generation_absence_is_not_a_zero_generation) {
    AdapterScope scope;
    auto first = identity(scope);
    auto second = first;
    second.generation = 0;
    CK_CHECK(compare_identity(first, second) == IdentityComparison::Unknown);
    first.generation = 0;
    CK_CHECK(compare_identity(first, second) == IdentityComparison::Same);
    second.generation = 1;
    CK_CHECK(compare_identity(first, second) == IdentityComparison::Different);
    AdapterScope moved = std::move(scope);
    CK_CHECK(moved.is_valid() && !scope.is_valid());
    auto invalid = identity(scope);
    CK_CHECK(compare_identity(invalid, invalid) == IdentityComparison::Unknown);
}

CK_TEST(file_times_preserve_pre_epoch_zero_precision_and_extreme_values) {
    CK_CHECK(!FileTimestamp::from_unix(0, 1'000'000'000));
    CK_CHECK(FileTimestamp::from_unix(-1, 999'999'999).has_value());
    const auto origin = FileTimestamp::from_windows_ticks(0);
    CK_CHECK(origin.seconds() == -11'644'473'600 && origin.nanoseconds() == 0);
    const auto epoch = FileTimestamp::from_windows_ticks(116'444'736'000'000'000ULL);
    CK_CHECK(epoch.seconds() == 0 && epoch.nanoseconds() == 0);
    const auto before = FileTimestamp::from_windows_ticks(116'444'735'999'999'999ULL);
    CK_CHECK(before.seconds() == -1 && before.nanoseconds() == 999'999'900);
    const auto largest = FileTimestamp::from_windows_ticks(std::numeric_limits<std::uint64_t>::max());
    CK_CHECK(largest.seconds() == 1'833'029'933'770LL && largest.nanoseconds() == 955'161'500);
}

CK_TEST(uninspected_basic_fields_default_to_unknown_instead_of_zero) {
    const BasicMetadata metadata;
    CK_CHECK(metadata.identity.state() == ObservationState::Unknown);
    CK_CHECK(metadata.kind.state() == ObservationState::Unknown);
    CK_CHECK(metadata.logical_size.state() == ObservationState::Unknown);
    CK_CHECK(metadata.created.state() == ObservationState::Unknown);
    CK_CHECK(metadata.linux_attributes.state() == ObservationState::Unknown);
}
