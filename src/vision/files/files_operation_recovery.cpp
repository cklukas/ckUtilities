// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_operation_recovery.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <thread>

namespace ck::vision {
namespace {
namespace fs = std::filesystem;

template <class T>
ck::files::Observation<T> error(ck::files::AdapterError code, const char *operation,
                                ck::files::ObservationState state = ck::files::ObservationState::Failed) {
    return ck::files::Observation<T>::unavailable(state,
        {ck::files::ErrorDomain::Adapter, static_cast<std::int64_t>(code), operation});
}

bool is_operation_journal_name(std::string_view value) {
    return value.starts_with(".ck-copy-journal-") || value.starts_with(".ck-tree-journal-") ||
           value.starts_with(".ck-move-journal-") || value.starts_with(".ck-delete-tree-journal-");
}

std::optional<ck::files::NativeName> native_name(const fs::path &path) {
#if defined(_WIN32)
    const auto units = path.filename().native();
    std::u16string value;
    value.reserve(units.size());
    for (const auto unit : units)
        value.push_back(static_cast<char16_t>(unit));
    return ck::files::NativeName::from_windows(std::move(value));
#else
    return ck::files::NativeName::from_posix(path.filename().native());
#endif
}

bool valid_limits(const FilesOperationRecoveryLimits &limits) {
    return limits.max_journals != 0 && limits.max_journals <= 16 * 1024 &&
           limits.journal.max_items != 0 && limits.journal.max_records != 0 &&
           limits.codec.max_bytes != 0 && limits.evidence.max_bytes != 0;
}

} // namespace

ck::files::Observation<FilesOperationRecoveryScan> discover_operation_recovery(
    const ck::files::NativeAbsolutePath &private_root, FilesOperationRecoveryLimits limits) {
    if (!private_root.is_valid() || !valid_limits(limits))
        return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InvalidLimit,
                                                 "operation_recovery.scan_limits");
    auto host = private_root.host_path();
    if (!host.value())
        return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
            host.state(), host.unavailable_detail()->error);

    std::error_code ec;
    const auto root_status = fs::symlink_status(*host.value(), ec);
    if (ec)
        return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
            ck::files::ObservationState::Denied,
            {ck::files::ErrorDomain::Posix, ec.value(), "operation_recovery.root_status"});
    if (!fs::is_directory(root_status) || fs::is_symlink(root_status))
        return error<FilesOperationRecoveryScan>(ck::files::AdapterError::NotDirectory,
                                                 "operation_recovery.root");

    FilesOperationRecoveryScan result;
    fs::directory_iterator iterator(*host.value(), ec);
    if (ec)
        return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
            ck::files::ObservationState::Denied,
            {ck::files::ErrorDomain::Posix, ec.value(), "operation_recovery.enumerate"});
    const fs::directory_iterator end;
    for (; iterator != end; iterator.increment(ec)) {
        if (ec)
            return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
                ck::files::ObservationState::Denied,
                {ck::files::ErrorDomain::Posix, ec.value(), "operation_recovery.enumerate"});
        const auto entry = *iterator;
        const auto basename = entry.path().filename().string();
        if (!is_operation_journal_name(basename))
            continue;
        if (result.journals.size() >= limits.max_journals)
            return error<FilesOperationRecoveryScan>(ck::files::AdapterError::InventoryLimit,
                                                     "operation_recovery.count",
                                                     ck::files::ObservationState::Unknown);

        FilesOperationRecoveryItem item;
        item.display_name = basename;
        item.native_name = native_name(entry.path());
        const auto status = entry.symlink_status(ec);
        if (ec) {
            item.error = {ck::files::ObservationState::Denied,
                          {ck::files::ErrorDomain::Posix, ec.value(), "operation_recovery.entry_status"}};
            result.requires_reconciliation = true;
            result.journals.push_back(std::move(item));
            ec.clear();
            continue;
        }
        if (!item.native_name || fs::is_symlink(status) || !fs::is_regular_file(status)) {
            item.error = {ck::files::ObservationState::Unsupported,
                          {ck::files::ErrorDomain::Adapter,
                           static_cast<std::int64_t>(ck::files::AdapterError::UnsupportedQuery),
                           "operation_recovery.entry_kind"}};
            result.requires_reconciliation = true;
            result.journals.push_back(std::move(item));
            continue;
        }

        auto file = ck::files::open_native_operation_journal_file(private_root, *item.native_name, false);
        if (!file.value()) {
            item.error = *file.unavailable_detail();
            result.requires_reconciliation = true;
        } else {
            auto inspection = ck::files::inspect_native_operation_recovery_evidence(
                **file.value(), limits.journal, limits.codec, limits.evidence);
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
    if (ec)
        return ck::files::Observation<FilesOperationRecoveryScan>::unavailable(
            ck::files::ObservationState::Denied,
            {ck::files::ErrorDomain::Posix, ec.value(), "operation_recovery.enumerate"});
    return ck::files::Observation<FilesOperationRecoveryScan>::available(std::move(result));
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
            result.error = {ck::files::ObservationState::Failed,
                             {ck::files::ErrorDomain::Adapter,
                              static_cast<std::int64_t>(ck::files::AdapterError::PlanInvalid),
                              "operation_recovery.worker_fault"}};
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
