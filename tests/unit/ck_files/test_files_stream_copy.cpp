// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sha256.hpp"
#include "stream_copy_job.hpp"

#include <cvision/testing/cktest.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace ck::files;

namespace {
std::vector<std::byte> bytes(const std::string &text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const auto value : text)
        result.push_back(std::byte{static_cast<unsigned char>(value)});
    return result;
}
std::array<std::byte, 32> digest(const char *hex) {
    std::array<std::byte, 32> result{};
    for (std::size_t index = 0; index != result.size(); ++index) {
        const auto digit = [](char value) {
            if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
            return static_cast<unsigned>(value - 'a' + 10);
        };
        result[index] = std::byte{static_cast<unsigned char>((digit(hex[index * 2]) << 4) | digit(hex[index * 2 + 1]))};
    }
    return result;
}

struct MemoryCopy final {
    std::vector<std::byte> source;
    std::vector<std::byte> destination;
    bool corrupt = false;
    unsigned flushes = 0;

    StreamCopyRequest request(std::size_t chunk = 4) {
        StreamCopyRequest result;
        result.limits.chunk_bytes = chunk;
        result.limits.max_bytes = 1024;
        result.read_source = [this](std::uint64_t offset, std::size_t limit, std::stop_token) {
            if (offset > source.size())
                return Observation<ContentRead>::unavailable(ObservationState::Gone,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "test.source_offset"});
            const auto count = std::min(limit, source.size() - static_cast<std::size_t>(offset));
            ContentRead value;
            value.bytes.assign(source.begin() + static_cast<std::ptrdiff_t>(offset),
                               source.begin() + static_cast<std::ptrdiff_t>(offset + count));
            value.complete = offset + count == source.size();
            return Observation<ContentRead>::available(std::move(value));
        };
        result.write_destination = [this](std::uint64_t offset, std::span<const std::byte> input, std::stop_token) {
            if (offset > destination.size())
                destination.resize(static_cast<std::size_t>(offset));
            const auto count = std::min<std::size_t>(2, input.size());
            if (offset + count > destination.size())
                destination.resize(static_cast<std::size_t>(offset + count));
            std::copy_n(input.begin(), count, destination.begin() + static_cast<std::ptrdiff_t>(offset));
            return Observation<std::size_t>::available(count);
        };
        result.read_destination = [this](std::uint64_t offset, std::size_t limit, std::stop_token) {
            if (offset > destination.size())
                return Observation<ContentRead>::unavailable(ObservationState::Gone,
                    {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InvalidIdentifier), "test.destination_offset"});
            const auto count = std::min(limit, destination.size() - static_cast<std::size_t>(offset));
            ContentRead value;
            value.bytes.assign(destination.begin() + static_cast<std::ptrdiff_t>(offset),
                               destination.begin() + static_cast<std::ptrdiff_t>(offset + count));
            if (corrupt && !value.bytes.empty())
                value.bytes.front() ^= std::byte{1};
            value.complete = offset + count == destination.size();
            return Observation<ContentRead>::available(std::move(value));
        };
        result.flush_destination = [this](std::stop_token) {
            ++flushes;
            return Observation<bool>::available(true);
        };
        return result;
    }
};
} // namespace

CK_TEST(files_sha256_matches_standard_vectors) {
    Sha256 empty;
    CK_CHECK(empty.digest() == digest("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    Sha256 value;
    const auto input = bytes("abc");
    value.update(input);
    CK_CHECK(value.digest() == digest("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

CK_TEST(stream_copy_job_handles_short_writes_and_independent_destination_verification) {
    MemoryCopy memory;
    memory.source = bytes("streamed file data with short writes");
    auto created = StreamCopyJob::create(memory.request());
    CK_CHECK(created.value());
    if (!created.value())
        return;
    auto job = std::move(created).take_value();
    for (unsigned step = 0; step != 100 && job->state() != StreamCopyState::Complete &&
         job->state() != StreamCopyState::Failed; ++step)
        job->advance();
    CK_CHECK(job->state() == StreamCopyState::Complete && memory.destination == memory.source && memory.flushes == 1);
    CK_CHECK(job->result().source_bytes == memory.source.size() &&
             job->result().source_digest == job->result().destination_digest);
}

CK_TEST(stream_copy_job_rejects_digest_mismatch_and_source_no_progress) {
    MemoryCopy corrupted;
    corrupted.source = bytes("copy");
    corrupted.corrupt = true;
    auto created = StreamCopyJob::create(corrupted.request(8));
    CK_CHECK(created.value());
    if (created.value()) {
        auto job = std::move(created).take_value();
        for (unsigned step = 0; step != 20 && job->state() != StreamCopyState::Complete &&
             job->state() != StreamCopyState::Failed; ++step)
            job->advance();
        CK_CHECK(job->state() == StreamCopyState::Failed && job->failure() &&
                 job->failure()->error.operation == "stream_copy.verify_digest");
    }
    StreamCopyRequest stalled = corrupted.request();
    stalled.read_source = [](std::uint64_t, std::size_t, std::stop_token) {
        return Observation<ContentRead>::available(ContentRead{});
    };
    auto stalled_job = StreamCopyJob::create(std::move(stalled));
    CK_CHECK(stalled_job.value());
    if (stalled_job.value()) {
        auto job = std::move(stalled_job).take_value();
        CK_CHECK(job->advance() == StreamCopyState::Failed && job->failure() &&
                 job->failure()->error.operation == "stream_copy.source_no_progress");
    }
}

CK_TEST(stream_copy_job_cancellation_and_flush_failure_are_terminal_without_success) {
    MemoryCopy memory;
    memory.source = bytes("cancel me");
    auto request = memory.request();
    request.flush_destination = [](std::stop_token) {
        return Observation<bool>::unavailable(ObservationState::Unknown,
            {ErrorDomain::Adapter, static_cast<std::int64_t>(AdapterError::InterruptedRead), "test.flush"});
    };
    auto created = StreamCopyJob::create(std::move(request));
    CK_CHECK(created.value());
    if (created.value()) {
        auto job = std::move(created).take_value();
        for (unsigned step = 0; step != 20 && job->state() != StreamCopyState::Failed; ++step)
            job->advance();
        CK_CHECK(job->state() == StreamCopyState::Failed && !job->result().error.value().error.operation.empty());
    }
}
