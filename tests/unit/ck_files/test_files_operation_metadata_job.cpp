// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "operation_metadata_job.hpp"

#include <cvision/testing/cktest.hpp>

#include <string>
#include <vector>

using namespace ck::files;

namespace {
OperationMetadataRequest request(std::vector<std::string>& order) {
    return {
        [&order](std::stop_token) { order.push_back("traversal"); return Observation<bool>::available(true); },
        [&order](std::stop_token) { order.push_back("security"); return Observation<bool>::available(true); },
        [&order](std::stop_token) { order.push_back("times"); return Observation<bool>::available(true); },
        [&order](std::stop_token) { order.push_back("flush"); return Observation<bool>::available(true); },
    };
}
}

CK_TEST(operation_metadata_job_requires_all_explicit_phase_callbacks) {
    OperationMetadataRequest missing;
    missing.apply_traversal_metadata = [](std::stop_token) { return Observation<bool>::available(true); };
    CK_CHECK(!OperationMetadataJob::create(std::move(missing)).value());
}

CK_TEST(operation_metadata_job_applies_phases_in_order) {
    std::vector<std::string> order;
    auto created = OperationMetadataJob::create(request(order));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 8 && job->state() != OperationMetadataState::Complete; ++step) job->advance();
    CK_CHECK(job->state() == OperationMetadataState::Complete);
    const std::vector<std::string> expected{"traversal", "security", "times", "flush"};
    CK_CHECK(order == expected);
}

CK_TEST(operation_metadata_job_cancellation_and_failure_stop_later_phases) {
    std::vector<std::string> order;
    auto created = OperationMetadataJob::create(request(order));
    CK_CHECK(created.value());
    if (!created.value()) return;
    auto job = std::move(created).take_value();
    std::stop_source stop;
    CK_CHECK(job->advance() == OperationMetadataState::ApplyingSecurity);
    stop.request_stop();
    CK_CHECK(job->advance(stop.get_token()) == OperationMetadataState::Cancelled);
    CK_CHECK(order == std::vector<std::string>{"traversal"});

    order.clear();
    auto failing = request(order);
    failing.apply_security_metadata = [&order](std::stop_token) {
        order.push_back("security");
        return Observation<bool>::unavailable(ObservationState::Denied,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidAccess), "test.security"});
    };
    auto failed = OperationMetadataJob::create(std::move(failing));
    CK_CHECK(failed.value());
    if (failed.value()) {
        auto failing_job = std::move(failed).take_value();
        failing_job->advance();
        CK_CHECK(failing_job->advance() == OperationMetadataState::Failed && failing_job->failure());
        const std::vector<std::string> expected{"traversal", "security"};
        CK_CHECK(order == expected);
    }
}
