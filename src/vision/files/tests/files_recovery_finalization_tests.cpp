// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_recovery_finalization.hpp"
#include <cvision/testing/cktest.hpp>
#include <memory>
#include <new>
#include <stdexcept>

namespace {
using ck::files::Observation;
using ck::files::ObservationState;
using ck::files::ErrorDomain;
using ck::vision::detail::finish_recovery_observation;
Observation<bool> close_failure() {
    return Observation<bool>::unavailable(ObservationState::Failed, {ErrorDomain::Win32, 29, "test.close"});
}
}

CK_TEST(files_recovery_completion_retains_success_and_move_only_payload) {
    unsigned closes = 0;
    auto result = finish_recovery_observation<std::unique_ptr<int>>(
        [] { return Observation<std::unique_ptr<int>>::available(std::make_unique<int>(17)); },
        [&] { ++closes; return Observation<bool>::available(true); }, "test.false");
    CK_CHECK(closes == 1 && result.value() && **result.value() == 17);
}

CK_TEST(files_recovery_completion_retains_work_error_after_successful_close) {
    unsigned closes = 0;
    auto result = finish_recovery_observation<int>(
        [] { return Observation<int>::unavailable(ObservationState::Denied, {ErrorDomain::Win32, 5, "test.read"}); },
        [&] { ++closes; return Observation<bool>::available(true); }, "test.false");
    CK_CHECK(closes == 1 && !result.value() && result.unavailable_detail());
    if (result.unavailable_detail()) CK_CHECK(result.unavailable_detail()->error.operation == "test.read");
}

CK_TEST(files_recovery_completion_surfaces_close_error_over_work_error_or_exception) {
    for (int work = 0; work != 3; ++work) {
        unsigned closes = 0;
        auto result = finish_recovery_observation<int>(
            [&]() -> Observation<int> {
                if (work == 2) throw std::runtime_error("inspection exception");
                if (work == 1) return Observation<int>::unavailable(ObservationState::Failed, {ErrorDomain::Adapter, 2, "test.decode"});
                return Observation<int>::available(17);
            }, [&] { ++closes; return close_failure(); }, "test.false");
        CK_CHECK(closes == 1 && !result.value() && result.unavailable_detail());
        if (result.unavailable_detail()) {
            CK_CHECK(result.unavailable_detail()->error.domain == ErrorDomain::Win32);
            CK_CHECK(result.unavailable_detail()->error.code == 29);
            CK_CHECK(result.unavailable_detail()->error.operation == "test.close");
        }
    }
}

CK_TEST(files_recovery_completion_rethrows_work_exception_only_after_close) {
    unsigned closes = 0;
    bool caught = false;
    try {
        (void)finish_recovery_observation<int>(
            []() -> Observation<int> { throw std::bad_alloc(); },
            [&] { ++closes; return Observation<bool>::available(true); }, "test.false");
    } catch (const std::bad_alloc &) { caught = true; }
    CK_CHECK(caught && closes == 1);
}

CK_TEST(files_recovery_completion_finishes_root_when_child_completion_throws) {
    unsigned child_closes = 0, root_closes = 0;
    bool caught = false;
    try {
        (void)finish_recovery_observation<int>([&] {
            return finish_recovery_observation<int>(
                [] { return Observation<int>::available(17); },
                [&]() -> Observation<bool> { ++child_closes; throw std::bad_alloc(); }, "test.child");
        }, [&] { ++root_closes; return Observation<bool>::available(true); }, "test.root");
    } catch (const std::bad_alloc &) { caught = true; }
    CK_CHECK(caught && child_closes == 1 && root_closes == 1);
}

CK_TEST(files_recovery_completion_refuses_false_completion) {
    auto result = finish_recovery_observation<int>([] { return Observation<int>::available(17); },
        [] { return Observation<bool>::available(false); }, "test.false");
    CK_CHECK(!result.value() && result.unavailable_detail());
    if (result.unavailable_detail()) {
        CK_CHECK(result.unavailable_detail()->error.domain == ErrorDomain::Adapter);
        CK_CHECK(result.unavailable_detail()->error.code == static_cast<std::int64_t>(ck::files::AdapterError::InvalidNativeResult));
        CK_CHECK(result.unavailable_detail()->error.operation == "test.false");
    }
}
