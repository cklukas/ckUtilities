// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_completion_service.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace ck::vision {
namespace {
using namespace ck::files;
struct Request {
    FilesCompletionTicket ticket;
    std::uint64_t revision;
    FilesLocationCompletionQuery query;
    bool show_hidden, refresh;
    std::optional<FilesCompletionCursor> after;
};
bool limited(const UnavailableObservation &e) {
    return e.error.domain == ErrorDomain::Adapter &&
           e.error.code == static_cast<std::int64_t>(AdapterError::InventoryLimit);
}
bool valid(const FilesCompletionServiceLimits &l) {
    const auto &q = l.query;
    const auto &m = l.matches;
    return q.path.max_native_bytes && q.path.max_native_bytes <= NativePathLimits::hard_native_bytes && q.components &&
           q.components <= FilesPaneLocation::hard_components && q.text_bytes && q.text_bytes <= 32768 && m.scan.rows &&
           m.scan.rows <= DirectoryIndexScanLimits::hard_rows && m.scan.wire_bytes >= 16 &&
           m.scan.wire_bytes <= DirectoryIndexScanLimits::hard_wire_bytes && m.candidates && m.candidates <= 64 &&
           m.text_bytes && m.text_bytes <= 1024 * 1024 && m.native_name_bytes && m.native_name_bytes <= 1024 * 1024 &&
           m.candidate_text_bytes && m.candidate_text_bytes <= 32768 && l.directory_rows &&
           l.directory_rows <= 1'000'000 && l.cache_bytes && l.cache_bytes <= 8ULL * 1024 * 1024 * 1024 &&
           l.cache_files && l.cache_files <= 256 && l.diagnostic_bytes && l.diagnostic_bytes <= 1024 * 1024;
}
} // namespace
struct FilesCompletionService::State {
    FilesPaneBackend backend;
    const FilesCompletionServiceLimits limits;
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Request> pending;
    std::optional<FilesCompletionReply> reply;
    FilesCompletionTicket latest, cancelled;
    std::uint64_t revision = 0, next = 1;
    std::shared_ptr<const FilesPaneLocation> active_parent;
    std::optional<std::stop_source> active_stop;
    bool discovery = false, busy = false, stopping = false, signalled = false;
    std::atomic<bool> alive{true};
    std::atomic<FilesPaneWorkerFault> worker_fault{FilesPaneWorkerFault::None};
    std::jthread worker;
    State(FilesPaneBackend b, FilesCompletionServiceLimits l, std::function<void()> w)
        : backend(std::move(b)), limits(l), wake(std::move(w)) {}
    void notify(bool needed) {
        if (needed && wake)
            wake();
    }
    void emit(FilesCompletionReply result) {
        bool needed = false;
        {
            std::lock_guard lock(mutex);
            busy = discovery = false;
            active_stop.reset();
            active_parent.reset();
            if (stopping || result.request != latest)
                return;
            if (result.request == cancelled && !result.error && result.state == FilesCompletionReplyState::Ready) {
                result.state = FilesCompletionReplyState::Cancelled;
                result.page.reset();
            }
            if ((result.error && result.error->error.operation.size() > limits.diagnostic_bytes) ||
                (result.page && result.page->first_proposal_error &&
                 result.page->first_proposal_error->error.operation.size() > limits.diagnostic_bytes)) {
                result.error.reset();
                result.page.reset();
                result.state = FilesCompletionReplyState::LimitExceeded;
            }
            reply = std::move(result);
            needed = !std::exchange(signalled, true);
        }
        notify(needed);
    }
    void finish(FilesPaneWorkerFault reason) noexcept {
        {
            std::lock_guard lock(mutex);
            worker_fault.store(reason, std::memory_order_release);
            busy = discovery = false;
            pending.reset();
            active_stop.reset();
            active_parent.reset();
            if (!stopping && reason != FilesPaneWorkerFault::None) {
                FilesCompletionReply fatal;
                fatal.request = latest;
                fatal.revision = revision;
                fatal.state = FilesCompletionReplyState::WorkerFault;
                fatal.fault = reason;
                reply = std::move(fatal);
                signalled = true;
            }
            alive.store(false, std::memory_order_release);
        }
        // Make a final attempt even if an earlier wake threw or was coalesced.
        try {
            notify(reason != FilesPaneWorkerFault::None);
        } catch (...) {
        }
    }
    void run() noexcept {
        try {
            loop();
            finish(FilesPaneWorkerFault::None);
        } catch (const std::bad_alloc &) {
            finish(FilesPaneWorkerFault::Allocation);
        } catch (...) {
            finish(FilesPaneWorkerFault::Exception);
        }
    }
    // Called only at worker discovery boundaries. The directory observation is
    // reusable, but no already-matched payload is ever relabeled with a new ID.
    void boundary(Request &current, std::stop_source &stop, bool leave_discovery = false) {
        bool halt = false;
        {
            std::lock_guard lock(mutex);
            if (!stop.stop_requested() && pending && pending->ticket != cancelled && !pending->refresh &&
                !pending->after && pending->query.parent() == current.query.parent()) {
                current = std::move(*pending);
                pending.reset();
            }
            if (leave_discovery)
                discovery = false;
            halt = stopping || current.ticket == cancelled || current.ticket != latest;
        }
        if (halt)
            stop.request_stop();
    }
    void loop() {
        // Every native/index owner, including cache replacement and shutdown,
        // is created and retired within this worker scope.
        std::unique_ptr<BasicFileSystem> filesystem;
        std::shared_ptr<IndexFileFactory> factory;
        auto budget = std::make_shared<IndexStorageBudget>(limits.cache_bytes, limits.cache_files);
        std::shared_ptr<const CompletedDirectoryIndex> cache;
        std::optional<FilesPaneLocation> cached_parent;
        for (;;) {
            std::optional<Request> request;
            std::stop_source stop;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping || pending.has_value(); });
                if (stopping)
                    return;
                request = std::move(pending);
                pending.reset();
                busy = discovery = true;
                active_parent = std::make_shared<const FilesPaneLocation>(request->query.parent());
                active_stop = stop;
                if (request->ticket == cancelled)
                    stop.request_stop();
            }
            auto &current = *request;
            FilesCompletionReply result;
            const auto fail = [&](const UnavailableObservation &e) {
                result.error = e;
                result.state =
                    limited(e) ? FilesCompletionReplyState::LimitExceeded : FilesCompletionReplyState::Failed;
            };
            const auto same_parent = [&] { return cache && cached_parent && *cached_parent == current.query.parent(); };
            if (current.after && (!same_parent() || current.refresh))
                result.state = FilesCompletionReplyState::Stale;
            else if (!stop.stop_requested()) {
                if (current.refresh && same_parent()) {
                    cache.reset();
                    cached_parent.reset();
                }
                if (!same_parent()) {
                    if (!filesystem) {
                        filesystem = backend.filesystem();
                        if (!filesystem)
                            throw std::runtime_error("null completion filesystem");
                    }
                    if (!factory && !stop.stop_requested()) {
                        auto opened = backend.workspace();
                        if (!opened.value())
                            fail(*opened.unavailable_detail());
                        else {
                            factory = *opened.value();
                            if (!factory)
                                throw std::runtime_error("null completion workspace");
                        }
                    }
                    if (!result.error)
                        boundary(current, stop);
                    if (!result.error && !stop.stop_requested()) {
                        const auto parent = current.query.parent();
                        auto opened = filesystem->open_native_directory(parent.root_address());
                        std::unique_ptr<DirectoryCursor> directory;
                        if (!opened.value())
                            fail(*opened.unavailable_detail());
                        else {
                            directory = std::move(opened).take_value();
                            if (!directory)
                                throw std::runtime_error("null completion directory");
                        }
                        for (const auto &name : parent.components()) {
                            if (!directory || stop.stop_requested())
                                break;
                            boundary(current, stop);
                            if (stop.stop_requested())
                                break;
                            auto child = directory->open_directory_child(name);
                            if (!child.value()) {
                                fail(*child.unavailable_detail());
                                directory.reset();
                                break;
                            }
                            directory = std::move(child).take_value();
                            if (!directory)
                                throw std::runtime_error("null completion child directory");
                        }
                        if (directory && !stop.stop_requested()) {
                            std::array<std::byte, 16> generation{};
                            auto serial = current.ticket.number();
                            for (std::size_t i = 0; i < 8; ++i) {
                                generation[i] = std::byte(serial & 255);
                                serial >>= 8;
                            }
                            DirectoryIndexStoreLimits store;
                            store.rows = limits.directory_rows;
                            store.bytes = limits.cache_bytes;
                            DirectoryIndexListingJob collection(
                                {AdapterScope{}, current.ticket.number()},
                                std::make_unique<DirectoryListing>(std::move(directory), DirectoryListingLimits{},
                                                                   backend.clock),
                                backend.storage_model, factory, budget, generation, store);
                            for (;;) {
                                if (collection.state() == DirectoryIndexListingState::Failed) {
                                    fail(*collection.failure());
                                    break;
                                }
                                boundary(current, stop);
                                const auto state = collection.advance(stop.get_token());
                                if (state == DirectoryIndexListingState::Failed) {
                                    fail(*collection.failure());
                                    break;
                                }
                                if (state == DirectoryIndexListingState::Cancelled)
                                    stop.request_stop();
                                if (stop.stop_requested())
                                    break;
                                if (state == DirectoryIndexListingState::Complete) {
                                    cache = *collection.completed().value();
                                    cached_parent = parent;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (!result.error) {
                    // Adoption and the transition to query-specific matching
                    // are atomic with submission. A concurrent edit either
                    // joins discovery here or stops this match through its token.
                    boundary(current, stop, true);
                    if (same_parent() && !stop.stop_requested()) {
                        auto matching = limits.matches;
                        matching.show_hidden = current.show_hidden;
                        FilesCompletionIndexJob match(cache, current.query, matching, current.after);
                        const bool stale = current.after && match.state() == FilesCompletionIndexState::Failed;
                        while (match.state() == FilesCompletionIndexState::Running)
                            match.advance(stop.get_token());
                        if (match.state() == FilesCompletionIndexState::Failed) {
                            if (match.failure())
                                fail(*match.failure());
                            else
                                throw std::runtime_error("completion match failed without diagnostic");
                            if (stale)
                                result.state = FilesCompletionReplyState::Stale;
                            if (cache->store()->state() != DirectoryIndexStoreState::Sealed) {
                                cache.reset();
                                cached_parent.reset();
                            }
                        } else if (match.state() == FilesCompletionIndexState::Complete && !stop.stop_requested()) {
                            result.state = FilesCompletionReplyState::Ready;
                            result.page = *match.completed().value();
                        }
                    }
                }
            }
            // Native failures retain precedence over a late cancellation. Old
            // requests are still discarded by ticket, never attached to a new edit.
            if (!result.error && result.state != FilesCompletionReplyState::Stale && stop.stop_requested()) {
                result.state = FilesCompletionReplyState::Cancelled;
                result.page.reset();
            }
            result.request = current.ticket;
            result.revision = current.revision;
            emit(std::move(result));
        }
    }
};
FilesCompletionService::FilesCompletionService(FilesPaneBackend backend, FilesCompletionServiceLimits limits,
                                               std::function<void()> wake) {
    if (!valid(limits) || !backend.filesystem || !backend.workspace || !backend.clock ||
        (backend.storage_model != StorageModel::Unknown && backend.storage_model != StorageModel::LinuxStatx &&
         backend.storage_model != StorageModel::DarwinStatFlags &&
         backend.storage_model != StorageModel::WindowsHandleAttributes))
        throw std::invalid_argument("invalid completion service configuration");
    state_ = std::make_unique<State>(std::move(backend), limits, std::move(wake));
    state_->worker = std::jthread([state = state_.get()] { state->run(); });
}
FilesCompletionService::~FilesCompletionService() { join(); }
FilesCompletionSubmitted FilesCompletionService::request(FilesLocationCompletionQuery query, std::uint64_t revision,
                                                         std::optional<bool> show_hidden,
                                                         std::optional<FilesCompletionCursor> after, bool refresh) {
    if (!revision || !query.parent().root_address().is_valid() || (after && refresh))
        return {FilesCompletionSubmission::Invalid, {}};
    const auto &l = state_->limits.query;
    query.limits_.path.max_native_bytes = std::min(query.limits_.path.max_native_bytes, l.path.max_native_bytes);
    query.limits_.components = std::min(query.limits_.components, l.components);
    query.limits_.text_bytes = std::min(query.limits_.text_bytes, l.text_bytes);
    const auto &p = query.parent();
    if (query.leading_.size() > query.limits_.text_bytes || p.native_bytes() > query.limits_.path.max_native_bytes ||
        p.components().size() >= query.limits_.components)
        return {FilesCompletionSubmission::LimitExceeded, {}};
    const auto &root = p.root_address();
    const auto width = root.posix_bytes() ? std::size_t{1} : sizeof(char16_t);
    const bool separator = !p.components().empty() || (root.posix_bytes() ? root.posix_bytes()->back() != '/'
                                                                          : root.windows_units()->back() != u'/' &&
                                                                                root.windows_units()->back() != u'\\');
    const auto prefix_bytes =
        query.posix_prefix() ? query.posix_prefix()->size() : query.windows_prefix()->size() * width;
    const auto available = query.limits_.path.max_native_bytes - p.native_bytes();
    if ((separator && width > available) || prefix_bytes > available - (separator ? width : 0))
        return {FilesCompletionSubmission::LimitExceeded, {}};
    std::optional<std::stop_source> stop;
    FilesCompletionTicket issued;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || !state_->alive.load())
            return {FilesCompletionSubmission::Stopped, {}};
        if (!state_->next)
            return {FilesCompletionSubmission::Exhausted, {}};
        issued = ticket(state_->next++);
        const bool reuse = state_->discovery && state_->active_parent && *state_->active_parent == query.parent() &&
                           !refresh && !after;
        state_->pending = Request{issued,           revision,
                                  std::move(query), show_hidden.value_or(state_->limits.matches.show_hidden),
                                  refresh,          std::move(after)};
        state_->reply.reset();
        state_->latest = issued;
        state_->revision = revision;
        state_->cancelled = {};
        if (!reuse)
            stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
    return {FilesCompletionSubmission::Accepted, issued};
}
bool FilesCompletionService::cancel(const FilesCompletionTicket &request) {
    std::optional<std::stop_source> stop;
    bool notify = false;
    {
        std::lock_guard lock(state_->mutex);
        if (!request.valid() || request != state_->latest || state_->stopping || !state_->alive.load() ||
            (!state_->busy && !state_->pending && !state_->reply))
            return false;
        if (state_->reply && state_->reply->state != FilesCompletionReplyState::Ready)
            return false;
        state_->cancelled = request;
        stop = state_->active_stop;
        if (state_->reply) {
            state_->reply->state = FilesCompletionReplyState::Cancelled;
            state_->reply->page.reset();
            state_->reply->error.reset();
            notify = !std::exchange(state_->signalled, true);
        }
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
    state_->notify(notify);
    return true;
}
std::optional<FilesCompletionReply> FilesCompletionService::take() {
    std::lock_guard lock(state_->mutex);
    state_->signalled = false;
    auto value = std::move(state_->reply);
    state_->reply.reset();
    return value;
}
void FilesCompletionService::request_stop() noexcept {
    std::optional<std::stop_source> stop;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->pending.reset();
        state_->reply.reset();
        stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
}
void FilesCompletionService::join() {
    request_stop();
    if (state_->worker.joinable()) {
        if (state_->worker.get_id() == std::this_thread::get_id())
            throw std::logic_error("completion service cannot join itself");
        state_->worker.join();
    }
}
bool FilesCompletionService::running() const noexcept { return state_->alive.load(std::memory_order_acquire); }
bool FilesCompletionService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->pending.has_value();
}
FilesPaneWorkerFault FilesCompletionService::fault() const noexcept {
    return state_->worker_fault.load(std::memory_order_acquire);
}
} // namespace ck::vision
