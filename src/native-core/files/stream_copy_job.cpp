// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "stream_copy_job.hpp"

#include <limits>
#include <stdexcept>

namespace ck::files {
namespace {

UnavailableObservation error(AdapterError code, const char *operation,
                              ObservationState state = ObservationState::Failed) {
    return {state, {ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation}};
}

template <class T>
Observation<T> rejected(const UnavailableObservation &detail) {
    return Observation<T>::unavailable(detail.state, detail.error);
}

} // namespace

Observation<StreamCopyJob::Pointer> StreamCopyJob::create(StreamCopyRequest request) {
    using Result = Observation<Pointer>;
    if (!request.limits.valid())
        return rejected<Pointer>(error(AdapterError::InvalidLimit, "stream_copy.limits"));
    if (!request.read_source || !request.write_destination || !request.read_destination || !request.flush_destination)
        return rejected<Pointer>(error(AdapterError::InvalidAccess, "stream_copy.adapters"));
    return Result::available(Pointer(new StreamCopyJob(std::move(request))));
}

void StreamCopyJob::fail(UnavailableObservation detail) noexcept {
    result_.state = StreamCopyState::Failed;
    result_.error = std::move(detail);
}

void StreamCopyJob::cancel() noexcept {
    result_.state = StreamCopyState::Cancelled;
    result_.error = error(AdapterError::InterruptedRead, "stream_copy.cancelled", ObservationState::Unknown);
}

StreamCopyState StreamCopyJob::copy_step(std::stop_token stop) {
    ContentRead content;
    try {
        auto read = request_.read_source(source_offset_, request_.limits.chunk_bytes, stop);
        if (!read.value()) {
            fail(*read.unavailable_detail());
            return state();
        }
        content = std::move(read).take_value();
    } catch (...) {
        fail(error(AdapterError::InvalidNativeResult, "stream_copy.source_exception"));
        return state();
    }
    if (content.bytes.size() > request_.limits.chunk_bytes || content.bytes.size() > request_.limits.max_bytes - source_offset_) {
        fail(error(AdapterError::InventoryLimit, "stream_copy.source_limit", ObservationState::Unknown));
        return state();
    }
    if (content.bytes.empty() && !content.complete) {
        fail(error(AdapterError::InvalidNativeResult, "stream_copy.source_no_progress"));
        return state();
    }
    std::size_t written = 0;
    std::size_t attempts = 0;
    while (written != content.bytes.size()) {
        if (stop.stop_requested()) {
            cancel();
            return state();
        }
        if (++attempts > request_.limits.max_write_attempts) {
            fail(error(AdapterError::InvalidNativeResult, "stream_copy.write_attempts"));
            return state();
        }
        try {
            auto write = request_.write_destination(source_offset_ + written,
                std::span<const std::byte>(content.bytes).subspan(written), stop);
            if (!write.value()) {
                fail(*write.unavailable_detail());
                return state();
            }
            if (!*write.value() || *write.value() > content.bytes.size() - written) {
                fail(error(AdapterError::InvalidNativeResult, "stream_copy.write_count"));
                return state();
            }
            written += *write.value();
        } catch (...) {
            fail(error(AdapterError::InvalidNativeResult, "stream_copy.destination_exception"));
            return state();
        }
    }
    source_hash_.update(content.bytes);
    source_offset_ += content.bytes.size();
    result_.source_bytes = source_offset_;
    result_.destination_bytes = source_offset_;
    if (content.complete) {
        result_.state = StreamCopyState::Copied;
        try {
            auto flushed = request_.flush_destination(stop);
            if (!flushed.value()) {
                fail(*flushed.unavailable_detail());
                return state();
            }
            if (!*flushed.value()) {
                fail(error(AdapterError::InvalidNativeResult, "stream_copy.flush_result"));
                return state();
            }
        } catch (...) {
            fail(error(AdapterError::InvalidNativeResult, "stream_copy.flush_exception"));
            return state();
        }
        result_.state = StreamCopyState::Verifying;
    } else {
        result_.state = StreamCopyState::Copying;
    }
    return state();
}

StreamCopyState StreamCopyJob::verify_step(std::stop_token stop) {
    if (stop.stop_requested()) {
        cancel();
        return state();
    }
    ContentRead content;
    try {
        auto read = request_.read_destination(verify_offset_, request_.limits.chunk_bytes, stop);
        if (!read.value()) {
            fail(*read.unavailable_detail());
            return state();
        }
        content = std::move(read).take_value();
    } catch (...) {
        fail(error(AdapterError::InvalidNativeResult, "stream_copy.verify_exception"));
        return state();
    }
    if (content.bytes.size() > request_.limits.chunk_bytes ||
        content.bytes.size() > source_offset_ - verify_offset_) {
        fail(error(AdapterError::InvalidNativeResult, "stream_copy.verify_size"));
        return state();
    }
    if (content.bytes.empty() && !content.complete) {
        fail(error(AdapterError::InvalidNativeResult, "stream_copy.verify_no_progress"));
        return state();
    }
    destination_hash_.update(content.bytes);
    verify_offset_ += content.bytes.size();
    if (!content.complete) {
        result_.state = StreamCopyState::Verifying;
        return state();
    }
    if (verify_offset_ != source_offset_) {
        fail(error(AdapterError::InvalidNativeResult, "stream_copy.verify_length"));
        return state();
    }
    result_.source_digest = source_hash_.digest();
    result_.destination_digest = destination_hash_.digest();
    if (result_.source_digest != result_.destination_digest) {
        fail(error(AdapterError::InvalidNativeResult, "stream_copy.verify_digest"));
        return state();
    }
    result_.state = StreamCopyState::Complete;
    return state();
}

StreamCopyState StreamCopyJob::advance(std::stop_token stop) {
    if (state() == StreamCopyState::Complete || state() == StreamCopyState::Failed ||
        state() == StreamCopyState::Cancelled)
        return state();
    if (stop.stop_requested()) {
        cancel();
        return state();
    }
    if (state() == StreamCopyState::Ready || state() == StreamCopyState::Copying)
        return copy_step(stop);
    return verify_step(stop);
}

} // namespace ck::files
