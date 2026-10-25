// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace ck::files {

enum class ObservationState { Available, Unsupported, Unknown, Denied, Gone, Changed, Failed };
enum class ErrorDomain { None, Posix, Win32, Adapter, NtStatus };
enum class AdapterError : std::int64_t {
    InvalidPath = 1, InvalidNativeResult = 2, NotApplicable = 3,
    UnsupportedQuery = 4, UnqualifiedIdentity = 5, InterruptedRead = 6,
    FinalLinkRejected = 7, NotDirectory = 8, InventoryLimit = 9, InvalidLimit = 10,
    InvalidIdentifier = 11, InvalidAccess = 12, AttributeAbsent = 13, ProviderReviewRequired = 14,
    MetadataChanged = 15, UnrepresentableName = 16, PlanInvalid = 17,
    PlanTopologyUnsafe = 18, PlanDecisionStale = 19
};

// Preserve native codes without translating them to a boolean or injecting a
// path into diagnostic text. operation is an adapter-owned operation name.
struct NativeError {
    ErrorDomain domain = ErrorDomain::None;
    std::int64_t code = 0;
    std::string operation;
    friend bool operator==(const NativeError&, const NativeError&) = default;
};

struct UnavailableObservation {
    ObservationState state = ObservationState::Unknown;
    NativeError error;
};

// Available(empty collection) is distinct from an unreadable collection.
// There is no implicit bool: callers must decide which states their operation
// accepts. The representation cannot hold Available without a T value.
template <class T>
class Observation final {
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>,
                  "observation payloads need noexcept moves to retain an active variant state");
public:
    Observation() : storage_(std::in_place_index<1>, UnavailableObservation{}) {}

    static Observation available(T value) {
        return Observation(std::move(value));
    }

    static Observation unavailable(ObservationState state, NativeError error = {}) {
        switch (state) {
        case ObservationState::Unsupported:
        case ObservationState::Unknown:
        case ObservationState::Denied:
        case ObservationState::Gone:
        case ObservationState::Changed:
        case ObservationState::Failed:
            return Observation(UnavailableObservation{state, std::move(error)});
        case ObservationState::Available:
            break;
        }
        throw std::invalid_argument("an unavailable observation needs a valid unavailable state");
    }

    ObservationState state() const noexcept {
        return value() ? ObservationState::Available : unavailable_detail()->state;
    }
    // Explicit transfer for owning payloads. A successfully consumed observation
    // becomes Unknown; extracting an unavailable value throws bad_variant_access.
    T take_value() && {
        T result = std::move(std::get<0>(storage_));
        storage_.template emplace<1>(UnavailableObservation{});
        return result;
    }
    const T* value() const noexcept { return std::get_if<0>(&storage_); }
    const UnavailableObservation* unavailable_detail() const noexcept { return std::get_if<1>(&storage_); }

private:
    explicit Observation(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    explicit Observation(UnavailableObservation detail) : storage_(std::in_place_index<1>, std::move(detail)) {}
    std::variant<T, UnavailableObservation> storage_;
};

} // namespace ck::files
