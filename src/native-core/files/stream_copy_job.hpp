// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "observation.hpp"
#include "opened_entry.hpp"
#include "sha256.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>

namespace ck::files {

enum class StreamCopyState { Ready, Copying, Copied, Verifying, Complete, Failed, Cancelled };

struct StreamCopyLimits final {
    std::size_t chunk_bytes = 64 * 1024;
    std::uint64_t max_bytes = 4ULL * 1024 * 1024 * 1024;
    std::size_t max_write_attempts = 1024;
    static constexpr std::size_t hard_chunk_bytes = 4 * 1024 * 1024;
    static constexpr std::uint64_t hard_max_bytes = 64ULL * 1024 * 1024 * 1024;
    static constexpr std::size_t hard_write_attempts = 1'000'000;
    bool valid() const noexcept {
        return chunk_bytes > 0 && chunk_bytes <= hard_chunk_bytes && max_bytes > 0 &&
            max_bytes <= hard_max_bytes && max_write_attempts > 0 && max_write_attempts <= hard_write_attempts;
    }
};

struct StreamCopyRequest final {
    std::function<Observation<ContentRead>(std::uint64_t, std::size_t, std::stop_token)> read_source;
    std::function<Observation<std::size_t>(std::uint64_t, std::span<const std::byte>, std::stop_token)> write_destination;
    std::function<Observation<ContentRead>(std::uint64_t, std::size_t, std::stop_token)> read_destination;
    std::function<Observation<bool>(std::stop_token)> flush_destination;
    StreamCopyLimits limits;
};

struct StreamCopyResult final {
    StreamCopyState state = StreamCopyState::Ready;
    std::uint64_t source_bytes = 0;
    std::uint64_t destination_bytes = 0;
    std::array<std::byte, 32> source_digest{};
    std::array<std::byte, 32> destination_digest{};
    std::optional<UnavailableObservation> error;
};

class StreamCopyJob final {
public:
    using Pointer = std::unique_ptr<StreamCopyJob>;

    static Observation<Pointer> create(StreamCopyRequest request);

    StreamCopyState state() const noexcept { return result_.state; }
    const StreamCopyResult& result() const noexcept { return result_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return result_.error; }

    // One advance performs one bounded source or verification read. Native
    // callbacks remain worker-owned and may block; no callback is invoked after
    // the job reaches a terminal state.
    StreamCopyState advance(std::stop_token stop = {});

private:
    explicit StreamCopyJob(StreamCopyRequest request) noexcept : request_(std::move(request)) {}
    void fail(UnavailableObservation detail) noexcept;
    void cancel() noexcept;
    StreamCopyState copy_step(std::stop_token stop);
    StreamCopyState verify_step(std::stop_token stop);

    StreamCopyRequest request_;
    StreamCopyResult result_;
    Sha256 source_hash_;
    Sha256 destination_hash_;
    std::uint64_t source_offset_ = 0;
    std::uint64_t verify_offset_ = 0;
};

// Compose the bounded copy state machine with two worker-owned native object
// handles. Both entries must remain alive and dedicated to this job until it
// reaches a terminal state; this helper performs no path reopening or UI work.
Observation<StreamCopyJob::Pointer> create_stream_copy_job(OpenedEntry& source, OpenedEntry& destination,
                                                           StreamCopyLimits limits = {});

} // namespace ck::files
