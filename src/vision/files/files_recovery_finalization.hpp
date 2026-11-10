// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "observation.hpp"
#include <exception>
#include <optional>
#include <utility>

namespace ck::vision::detail {
// Worker-only completion for a read-only owner. Finish runs on every exit,
// including work exceptions, and must consume native ownership before any
// fallible error formatting. A completion failure takes precedence over the
// observation/exception: it must never disappear behind a decoder failure.
// No failed result grants mutation authority or publishes a successful scan.
template<class T, class Work, class Finish>
ck::files::Observation<T> finish_recovery_observation(Work &&work, Finish &&finish,
                                                     const char *false_operation) {
    std::optional<ck::files::Observation<T>> result;
    std::exception_ptr exception;
    try {
        result.emplace(std::forward<Work>(work)());
    } catch (...) {
        exception = std::current_exception();
    }
    auto completed = std::forward<Finish>(finish)();
    if (!completed.value())
        return ck::files::Observation<T>::unavailable(completed.state(), completed.unavailable_detail()->error);
    if (!*completed.value())
        return ck::files::Observation<T>::unavailable(ck::files::ObservationState::Failed,
            {ck::files::ErrorDomain::Adapter,
             static_cast<std::int64_t>(ck::files::AdapterError::InvalidNativeResult), false_operation});
    if (exception)
        std::rethrow_exception(exception);
    return std::move(*result);
}
} // namespace ck::vision::detail
