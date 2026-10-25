// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "operation_journal.hpp"
#include "operation_metadata_job.hpp"
#include "stream_copy_job.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace ck::files {

enum class OperationCopyState { Ready, Preparing, Copying, PreparingMetadata, ApplyingMetadata,
                                Verifying, Verified, Failed, Cancelled, NeedsReconciliation };

struct OperationCopyRequest final {
    std::size_t item = 0;
    OperationJournal::Pointer journal;
    std::function<Observation<bool>(std::stop_token)> revalidate_source;
    std::function<Observation<bool>(std::stop_token)> revalidate_destination;
    // The callback creates and retains the private destination object. An
    // unavailable result is conservative: the caller must reconcile ownership
    // before deciding whether any staging object exists.
    std::function<Observation<StreamCopyJob::Pointer>(std::stop_token)> prepare_copy;
    // Creates the ordered metadata job against the still-private destination.
    // It runs only after the stream copy has independently verified its bytes.
    // An unavailable result requires reconciliation; no publication follows.
    std::function<Observation<OperationMetadataJob::Pointer>(std::stop_token)> prepare_metadata;
    std::function<Observation<bool>(std::stop_token)> verify_copy;
    std::function<Observation<bool>(OperationJournalRecord)> append_journal;
};

struct OperationCopyResult final {
    OperationCopyState state = OperationCopyState::Ready;
    std::uint64_t observed_bytes = 0;
    std::optional<UnavailableObservation> failure;
};

// Worker-only journal/copy composition. It never publishes a destination,
// replaces a target, removes a source, or exposes a UI mutation command.
class OperationCopyJob final {
public:
    using Pointer = std::unique_ptr<OperationCopyJob>;
    static Observation<Pointer> create(OperationCopyRequest request);
    OperationCopyState state() const noexcept { return result_.state; }
    const OperationCopyResult& result() const noexcept { return result_; }
    const std::optional<UnavailableObservation>& failure() const noexcept { return result_.failure; }
    // Cancels a verified-but-not-yet-committed copy without invoking any
    // native publication callback. The journal transition remains durable;
    // a journal failure becomes reconciliation rather than being hidden.
    OperationCopyState cancel_after_verification();
    OperationCopyState advance(std::stop_token stop = {});

private:
    explicit OperationCopyJob(OperationCopyRequest request) noexcept : request_(std::move(request)) {}
    bool append(OperationJournalState state);
    void fail(UnavailableObservation detail, bool reconciliation);
    bool validate(const std::function<Observation<bool>(std::stop_token)>& callback,
                  const char* operation, std::stop_token stop);
    OperationCopyRequest request_;
    OperationCopyResult result_;
    StreamCopyJob::Pointer copy_;
    OperationMetadataJob::Pointer metadata_;
};

} // namespace ck::files
