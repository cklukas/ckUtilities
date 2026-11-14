// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery.hpp"
#include "directory_cursor.hpp"
#include "files_recovery_finalization.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>

namespace ck::vision {
namespace {

template <class T>
ck::files::Observation<T> error(ck::files::AdapterError code, const char *operation,
                                ck::files::ObservationState state = ck::files::ObservationState::Failed) {
    return ck::files::Observation<T>::unavailable(state,
        {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

std::optional<FilesOperationJournalKind> journal_kind(const ck::files::NativeName &name) {
    const auto starts_with = [&](std::string_view prefix) {
        if (const auto *bytes = name.posix_bytes())
            return bytes->starts_with(prefix);
        const auto *units = name.windows_units();
        return units && units->size() >= prefix.size() &&
            std::equal(prefix.begin(), prefix.end(), units->begin(), [](char byte, char16_t unit) {
                return static_cast<unsigned char>(byte) == unit;
            });
    };
    if (starts_with(".ck-retention-"))
        return FilesOperationJournalKind::RetainedTarget;
    constexpr std::string_view prefixes[]{".ck-copy-journal-", ".ck-tree-journal-", ".ck-move-journal-",
                                          ".ck-delete-tree-journal-", ".ck-reviewed-journal-"};
    if (std::any_of(std::begin(prefixes), std::end(prefixes), starts_with))
        return FilesOperationJournalKind::Transfer;
    return std::nullopt;
}

bool valid_limits(const FilesOperationRecoveryLimits &limits) {
    return limits.max_journals != 0 && limits.max_journals <= 16 * 1024 &&
           limits.journal.max_items != 0 && limits.journal.max_records != 0 &&
           limits.codec.max_bytes != 0 && limits.evidence.max_bytes != 0 &&
           limits.max_scan_batches != 0 && limits.max_scan_batches <= 4096;
}

} // namespace

ck::files::Observation<FilesOperationRecoveryScan> discover_operation_recovery(
    const ck::files::NativeAbsolutePath &private_root, FilesOperationRecoveryLimits limits) {
    if (!private_root.is_valid() || !valid_limits(limits))
        return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InvalidLimit,
                                                 "operation_recovery.scan_limits");
    // Acquire this exact native directory once. Every journal read is relative
    // to its retained handle; presentation text and navigation paths never
    // become reopening authority.
    auto opened = ck::files::open_native_operation_journal_directory(private_root);
    if (!opened.value())
        return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
            opened.state(), opened.unavailable_detail()->error);
    auto cursor = std::move(opened).take_value();
    return detail::finish_recovery_observation<FilesOperationRecoveryScan>(
        [&]() -> ck::files::Observation<FilesOperationRecoveryScan> {
            FilesOperationRecoveryScan result;
            auto remaining_batches = limits.max_scan_batches;
            for (;;) {
                if (!remaining_batches)
                    return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InventoryLimit,
                        "operation_recovery.scan_batches", ck::files::ObservationState::Unknown);
                // read() limits returned names but always caps native steps at
                // max_batch_size, including skipped records. Charge a whole batch.
                auto batch = cursor->read(std::min(limits.max_journals, ck::files::DirectoryCursor::max_batch_size));
                --remaining_batches;
                if (batch.error)
                    return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
                        batch.error->state, batch.error->error);
                for (const auto &name : batch.names) {
                    const auto kind = journal_kind(name);
                    if (!kind)
                        continue;
                    if (result.journals.size() >= limits.max_journals)
                        return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InventoryLimit,
                            "operation_recovery.count", ck::files::ObservationState::Unknown);

                    FilesOperationRecoveryItem item;
                    item.native_name = name;
                    item.display_name = name.display_name();
                    item.kind = *kind;
                    auto file = ck::files::open_held_operation_journal_file(*cursor, name);
                    if (!file.value()) {
                        item.error = *file.unavailable_detail();
                        result.requires_reconciliation = true;
                    } else {
                        auto inspection = detail::finish_recovery_observation<ck::files::NativeOperationRecoveryInspection>(
                            [&] {
                                if (item.kind == FilesOperationJournalKind::RetainedTarget)
                                    return ck::files::inspect_native_retention_recovery(
                                        **file.value(), limits.journal, limits.codec, limits.evidence);
                                return ck::files::inspect_native_operation_recovery_evidence(
                                    **file.value(), limits.journal, limits.codec, limits.evidence);
                            },
                            [&] { return (*file.value())->finish_io(); }, "operation_recovery.finish_io");
                        if (!inspection.value()) {
                            item.error = *inspection.unavailable_detail();
                            result.requires_reconciliation = true;
                        } else {
                            result.requires_reconciliation = result.requires_reconciliation ||
                                inspection.value()->report.requires_reconciliation;
                            item.report = FilesOperationRecoveryReport{
                                inspection.value()->journal_id, std::move(inspection).take_value()};
                        }
                    }
                    result.journals.push_back(std::move(item));
                }
                if (batch.state == ck::files::DirectoryReadState::End)
                    break;
                if (batch.state != ck::files::DirectoryReadState::More)
                    return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InterruptedRead,
                        "operation_recovery.enumerate", ck::files::ObservationState::Unknown);
            }
            return ck::files::Observation<FilesOperationRecoveryScan>::available(std::move(result));
        }, [&] { return ck::files::finish_operation_journal_directory(*cursor); },
        "operation_recovery.directory_finish_io");
}

struct FilesOperationRecoveryService::State final {
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<std::pair<ck::files::NativeAbsolutePath, FilesOperationRecoveryLimits>> pending;
    std::optional<FilesOperationRecoveryReply> reply;
    bool stopping = false, busy = false, signalled = false;
    std::thread worker;

    explicit State(std::function<void()> callback) : wake(std::move(callback)), worker([this] { run(); }) {}
    ~State() {
        request_stop();
        join();
    }
    void request_stop() noexcept {
        std::lock_guard lock(mutex);
        stopping = true;
        condition.notify_all();
    }
    void join() {
        if (worker.joinable()) worker.join();
    }
    void signal() {
        if (signalled || !wake) return;
        signalled = true;
        try { wake(); } catch (...) {}
    }
    void emit(FilesOperationRecoveryReply value) {
        std::lock_guard lock(mutex);
        if (stopping) return;
        reply = std::move(value);
        busy = false;
        signal();
    }
    void run_one(ck::files::NativeAbsolutePath root, FilesOperationRecoveryLimits limits) {
        try {
            auto scan = discover_operation_recovery(root, limits);
            FilesOperationRecoveryReply result;
            if (scan.value()) {
                result.state = FilesOperationRecoveryState::Ready;
                result.scan = std::move(scan).take_value();
            } else {
                result.state = FilesOperationRecoveryState::Failed;
                result.error = *scan.unavailable_detail();
            }
            emit(std::move(result));
        } catch (...) {
            FilesOperationRecoveryReply result;
            result.state = FilesOperationRecoveryState::WorkerFault;
            // Keep the fallback allocation-free: formatting a NativeError
            // here can throw again under persistent worker allocation failure.
            // WorkerFault with no error already has an explicit UI diagnostic.
            emit(std::move(result));
        }
    }
    void run() {
        for (;;) {
            std::optional<std::pair<ck::files::NativeAbsolutePath, FilesOperationRecoveryLimits>> job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stopping || pending.has_value(); });
                if (stopping) { pending.reset(); return; }
                job = std::move(*pending);
                pending.reset();
                busy = true;
                signalled = false;
            }
            run_one(std::move(job->first), std::move(job->second));
        }
    }
};

FilesOperationRecoveryService::FilesOperationRecoveryService(std::function<void()> wake)
    : state_(std::make_unique<State>(std::move(wake))) {}
FilesOperationRecoveryService::~FilesOperationRecoveryService() = default;
FilesOperationRecoverySubmission FilesOperationRecoveryService::request(
    ck::files::NativeAbsolutePath private_root, FilesOperationRecoveryLimits limits) {
    std::lock_guard lock(state_->mutex);
    if (state_->stopping) return FilesOperationRecoverySubmission::Stopped;
    if (!private_root.is_valid() || !valid_limits(limits)) return FilesOperationRecoverySubmission::Invalid;
    if (state_->busy || state_->pending) return FilesOperationRecoverySubmission::Busy;
    state_->pending.emplace(std::move(private_root), limits);
    state_->condition.notify_one();
    return FilesOperationRecoverySubmission::Accepted;
}
std::optional<FilesOperationRecoveryReply> FilesOperationRecoveryService::take() {
    std::lock_guard lock(state_->mutex);
    auto result = std::move(state_->reply);
    state_->reply.reset();
    state_->signalled = false;
    return result;
}
void FilesOperationRecoveryService::request_stop() noexcept { state_->request_stop(); }
void FilesOperationRecoveryService::join() { state_->join(); }
bool FilesOperationRecoveryService::running() const noexcept {
    std::lock_guard lock(state_->mutex);
    return !state_->stopping;
}
bool FilesOperationRecoveryService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}

} // namespace ck::vision
