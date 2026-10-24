// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_pane_service.hpp"
#include "native_index_file.hpp"
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
namespace ck::vision {
static_assert(std::is_nothrow_move_constructible_v<FilesPaneReply> &&
              std::is_nothrow_move_assignable_v<FilesPaneReply>);
namespace {
using namespace ck::files;
struct Navigate {
    FilesPaneTicket ticket;
    NativeAbsolutePath path;
    PaneView view;
    std::size_t rows;
};
struct Read {
    FilesPaneTicket ticket, navigation;
    PaneStateStamp stamp;
    FilesPaneReadRange range;
};
bool valid(FilesPaneServiceLimits l) {
    return l.path_bytes && l.path_bytes <= NativePathLimits::hard_native_bytes && l.page_rows && l.page_rows <= 4096 &&
           l.page_text_bytes && l.page_text_bytes <= 16 * 1024 * 1024 && l.page_wire_bytes >= 16 &&
           l.page_wire_bytes <= DirectoryIndexScanLimits::hard_wire_bytes && l.diagnostic_bytes &&
           l.diagnostic_bytes <= 1024 * 1024 && l.directory_rows && l.directory_rows <= 1'000'000 && l.cache_bytes &&
           l.cache_bytes <= 8ULL * 1024 * 1024 * 1024 && l.cache_files && l.cache_files <= 256;
}
} // namespace
struct FilesPaneService::State {
    FilesPaneBackend backend;
    const FilesPaneServiceLimits limits;
    std::function<void()> wake;
    std::mutex mutex;
    std::condition_variable condition;
    std::optional<Navigate> navigation;
    std::optional<Read> read;
    std::optional<FilesPaneReply> reply;
    FilesPaneTicket latest_navigation, latest_request, cancelled;
    std::optional<std::stop_source> active_stop;
    bool navigation_busy = false, busy = false, stopping = false, signalled = false;
    std::uint64_t next = 1;
    std::atomic<bool> alive{true};
    std::atomic<FilesPaneWorkerFault> worker_fault{FilesPaneWorkerFault::None};
    std::jthread worker;
    State(FilesPaneBackend b, FilesPaneServiceLimits l, std::function<void()> w)
        : backend(std::move(b)), limits(l), wake(std::move(w)) {}
    void notify(bool needed) {
        if (needed && wake)
            wake();
    }
    bool signal() noexcept { return !std::exchange(signalled, true); }
    void emit(FilesPaneReply result, bool is_navigation) {
        bool needed = false;
        {
            std::lock_guard lock(mutex);
            busy = false;
            active_stop.reset();
            if (stopping || result.request != latest_request)
                return;
            if (is_navigation)
                navigation_busy = false;
            // Never truncate an adapter diagnostic into apparent native success.
            if (result.error && result.error->error.operation.size() > limits.diagnostic_bytes) {
                result.error.reset();
                result.page.reset();
                result.state = FilesPaneReplyState::LimitExceeded;
            }
            reply = std::move(result);
            needed = signal();
        }
        notify(needed);
    }
    void finish(FilesPaneWorkerFault reason) noexcept {
        bool needed = false;
        {
            std::lock_guard lock(mutex);
            worker_fault.store(reason, std::memory_order_release);
            busy = navigation_busy = false;
            navigation.reset();
            read.reset();
            active_stop.reset();
            if (!stopping && reason != FilesPaneWorkerFault::None) {
                // This path allocates nothing, even after std::bad_alloc.
                FilesPaneReply fatal;
                fatal.request = latest_request;
                fatal.navigation = latest_navigation;
                fatal.state = FilesPaneReplyState::WorkerFault;
                fatal.worker_fault = reason;
                reply = std::move(fatal);
            }
            alive.store(false, std::memory_order_release);
            needed = reason != FilesPaneWorkerFault::None ? (signalled = true) : signal();
        }
        // A notification failure cannot escape a thread entry or hide the fault.
        try {
            notify(needed);
        } catch (...) {
            if (reason == FilesPaneWorkerFault::None)
                worker_fault.store(FilesPaneWorkerFault::Exception);
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
    void loop() {
        // All native owners and all index/model retirement stay in this scope.
        auto pane = std::make_unique<IndexedPane>();
        std::unique_ptr<BasicFileSystem> filesystem;
        std::shared_ptr<IndexFileFactory> factory;
        auto budget = std::make_shared<IndexStorageBudget>(limits.cache_bytes, limits.cache_files);
        std::shared_ptr<const NativeAbsolutePath> active;
        for (;;) {
            std::optional<Navigate> nav;
            std::optional<Read> page;
            std::stop_source stop;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return stopping || navigation || read; });
                if (stopping)
                    return;
                if (navigation) {
                    nav = std::move(navigation);
                    navigation.reset();
                } else {
                    page = std::move(read);
                    read.reset();
                }
                busy = true;
                active_stop = stop;
                const auto &current = nav ? nav->ticket : page->ticket;
                if (current == cancelled)
                    stop.request_stop();
            }
            if (nav) {
                auto requested = std::make_shared<const NativeAbsolutePath>(std::move(nav->path));
                FilesPaneReply result;
                result.request = result.navigation = nav->ticket;
                result.active_location = active;
                result.requested_location = requested;
                auto candidate = std::make_unique<IndexedPane>();
                bool committed = false;
                auto request = candidate->begin_listing(nav->view);
                const auto fail = [&](const UnavailableObservation &error) {
                    candidate->fail_listing(request, error);
                    result.state = FilesPaneReplyState::Failed;
                    result.error = error;
                };
                if (!stop.stop_requested()) {
                    if (!filesystem) {
                        filesystem = backend.filesystem();
                        if (!filesystem)
                            throw std::runtime_error("null pane filesystem");
                    }
                    if (!factory) {
                        auto opened = backend.workspace();
                        if (!opened.value())
                            fail(*opened.unavailable_detail());
                        else {
                            factory = *opened.value();
                            if (!factory)
                                throw std::runtime_error("null pane workspace");
                        }
                    }
                    if (factory && !result.error && !stop.stop_requested()) {
                        auto opened = filesystem->open_native_directory(*requested);
                        if (!opened.value())
                            fail(*opened.unavailable_detail());
                        else if (!stop.stop_requested()) {
                            auto listing = std::make_unique<DirectoryListing>(std::move(opened).take_value(),
                                                                              DirectoryListingLimits{}, backend.clock);
                            std::array<std::byte, 16> generation{};
                            auto number = nav->ticket.number();
                            for (std::size_t i = 0; i < 8; ++i) {
                                generation[i] = std::byte(number & 255);
                                number >>= 8;
                            }
                            DirectoryIndexStoreLimits store;
                            store.rows = limits.directory_rows;
                            store.bytes = limits.cache_bytes;
                            DirectoryIndexListingJob collection(request, std::move(listing), backend.storage_model,
                                                                factory, budget, generation, store);
                            while (!stop.stop_requested()) {
                                const auto state = collection.advance(stop.get_token());
                                if (state == DirectoryIndexListingState::Complete ||
                                    state == DirectoryIndexListingState::Failed ||
                                    state == DirectoryIndexListingState::Cancelled)
                                    break;
                            }
                            auto collected = collection.completed();
                            if (collection.state() == DirectoryIndexListingState::Failed)
                                fail(*collected.unavailable_detail());
                            else if (collected.value() && !stop.stop_requested()) {
                                IndexedPaneDataJob preparation(*collected.value(), nav->view, {}, {},
                                                               limits.cache_bytes);
                                while (!stop.stop_requested()) {
                                    auto state = preparation.advance(stop.get_token());
                                    if (state == IndexedPaneDataJobState::Complete ||
                                        state == IndexedPaneDataJobState::Failed ||
                                        state == IndexedPaneDataJobState::Cancelled)
                                        break;
                                }
                                auto prepared = preparation.completed();
                                if (preparation.state() == IndexedPaneDataJobState::Failed)
                                    fail(*prepared.unavailable_detail());
                                else if (prepared.value() && !stop.stop_requested()) {
                                    const auto published = candidate->publish(*prepared.value(), stop.get_token());
                                    if (published == IndexedPanePublish::Applied) {
                                        capture(*candidate, candidate->state_stamp(), {0, nav->rows}, stop.get_token(),
                                                result);
                                        if (result.state == FilesPaneReplyState::Ready) {
                                            // No native I/O or native-owner destruction under this lock.
                                            std::lock_guard lock(mutex);
                                            if (!stopping && latest_request == nav->ticket &&
                                                cancelled != nav->ticket && !stop.stop_requested()) {
                                                pane.swap(candidate);
                                                active = requested;
                                                result.active_location = active;
                                                committed = true;
                                            } else {
                                                result.state = FilesPaneReplyState::Cancelled;
                                                result.page.reset();
                                            }
                                        }
                                    } else if (published == IndexedPanePublish::Failed && candidate->listing_error())
                                        fail(*candidate->listing_error());
                                    else if (published != IndexedPanePublish::Cancelled)
                                        throw std::runtime_error("pane publication invariant");
                                }
                            }
                        }
                    }
                }
                if (!committed && stop.stop_requested() && !result.error) {
                    candidate->cancel_listing(request);
                    result.state = FilesPaneReplyState::Cancelled;
                    result.page.reset();
                }
                result.stamp = pane->state_stamp();
                emit(std::move(result), true);
            } else {
                FilesPaneReply result;
                result.request = page->ticket;
                result.navigation = page->navigation;
                result.active_location = result.requested_location = active;
                capture(*pane, page->stamp, page->range, stop.get_token(), result);
                result.stamp = pane->state_stamp();
                emit(std::move(result), false);
            }
        }
    }
    void capture(IndexedPane &pane, const PaneStateStamp &stamp, FilesPaneReadRange range, std::stop_token stop,
                 FilesPaneReply &result) {
        if (!pane.data() || !pane.accepts(stamp)) {
            result.state = FilesPaneReplyState::Stale;
            return;
        }
        IndexedPanePageLimits page_limits;
        page_limits.display = {range.rows, limits.page_text_bytes};
        page_limits.wire_bytes = limits.page_wire_bytes;
        auto captured = pane.capture_page(stamp, range.first, page_limits, stop);
        switch (captured.state) {
        case IndexedPaneCaptureState::Ready:
            if (!captured.page)
                throw std::logic_error("missing completed pane page");
            if (captured.page->rows().size() !=
                std::min<std::uint64_t>(range.rows, captured.page->visible_rows() - range.first)) {
                result.state = FilesPaneReplyState::LimitExceeded;
                return;
            }
            result.state = FilesPaneReplyState::Ready;
            result.page = std::move(captured.page);
            return;
        case IndexedPaneCaptureState::Stale:
            result.state = FilesPaneReplyState::Stale;
            return;
        case IndexedPaneCaptureState::OutOfRange:
            result.state = FilesPaneReplyState::OutOfRange;
            return;
        case IndexedPaneCaptureState::LimitExceeded:
            result.state = FilesPaneReplyState::LimitExceeded;
            return;
        case IndexedPaneCaptureState::Cancelled:
            result.state = FilesPaneReplyState::Cancelled;
            return;
        case IndexedPaneCaptureState::Failed:
            result.state = FilesPaneReplyState::Failed;
            result.error = pane.listing_error();
            return;
        }
        throw std::runtime_error("pane capture invariant");
    }
};
FilesPaneBackend native_files_pane_backend(NativeAbsolutePath cache_root) {
    if (!cache_root.is_valid())
        throw std::invalid_argument("invalid pane cache address");
    FilesPaneBackend backend;
    backend.filesystem = [] { return std::make_unique<NativeFileSystem>(); };
    backend.workspace = [root = std::move(cache_root)] { return open_native_index_workspace(root); };
#if defined(_WIN32)
    backend.storage_model = StorageModel::WindowsHandleAttributes;
#elif defined(__APPLE__)
    backend.storage_model = StorageModel::DarwinStatFlags;
#elif defined(__linux__)
    backend.storage_model = StorageModel::LinuxStatx;
#else
    throw std::runtime_error("native file pane is unsupported on this platform");
#endif
    return backend;
}
FilesPaneService::FilesPaneService(FilesPaneBackend backend, FilesPaneServiceLimits limits,
                                   std::function<void()> wake) {
    if (!backend.filesystem || !backend.workspace || !backend.clock || !valid(limits) ||
        backend.storage_model < StorageModel::LinuxStatx ||
        backend.storage_model > StorageModel::WindowsHandleAttributes)
        throw std::invalid_argument("invalid pane service configuration");
    state_ = std::make_unique<State>(std::move(backend), limits, std::move(wake));
    state_->worker = std::jthread([state = state_.get()] { state->run(); });
}
FilesPaneService::~FilesPaneService() { join(); }
FilesPaneSubmitted FilesPaneService::navigate(NativeAbsolutePath location, PaneView view, std::size_t first_page_rows) {
    if (!location.is_valid() || !valid_pane_order({view.sort, view.descending, view.folders_first}))
        return {FilesPaneSubmission::Invalid, {}};
    const auto units = location.posix_bytes() ? location.posix_bytes()->size() : location.windows_units()->size();
    const auto width = location.posix_bytes() ? 1U : 2U;
    if (units > state_->limits.path_bytes / width || view.display_substring.size() > 4096 || !first_page_rows ||
        first_page_rows > state_->limits.page_rows)
        return {FilesPaneSubmission::LimitExceeded, {}};
    std::optional<std::stop_source> stop;
    FilesPaneTicket request;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || !state_->alive.load())
            return {FilesPaneSubmission::Stopped, {}};
        if (!state_->next)
            return {FilesPaneSubmission::Exhausted, {}};
        request = ticket(state_->next++);
        state_->navigation.emplace(Navigate{request, std::move(location), std::move(view), first_page_rows});
        state_->read.reset();
        state_->reply.reset();
        state_->cancelled = {};
        state_->latest_navigation = state_->latest_request = request;
        state_->navigation_busy = true;
        stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
FilesPaneSubmitted FilesPaneService::read_page(FilesPaneTicket navigation, PaneStateStamp stamp,
                                               FilesPaneReadRange range) {
    if (!navigation.valid())
        return {FilesPaneSubmission::Invalid, {}};
    if (!range.rows || range.rows > state_->limits.page_rows)
        return {FilesPaneSubmission::LimitExceeded, {}};
    std::optional<std::stop_source> stop;
    FilesPaneTicket request;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || !state_->alive.load())
            return {FilesPaneSubmission::Stopped, {}};
        if (navigation != state_->latest_navigation)
            return {FilesPaneSubmission::Invalid, {}};
        if (state_->navigation_busy || state_->reply)
            return {FilesPaneSubmission::Busy, {}};
        if (!state_->next)
            return {FilesPaneSubmission::Exhausted, {}};
        request = ticket(state_->next++);
        state_->read = Read{request, navigation, std::move(stamp), range};
        state_->latest_request = request;
        state_->cancelled = {};
        stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
    return {FilesPaneSubmission::Accepted, request};
}
bool FilesPaneService::cancel(const FilesPaneTicket &request) {
    std::optional<std::stop_source> stop;
    {
        std::lock_guard lock(state_->mutex);
        if (!request.valid() || request != state_->latest_request || state_->stopping ||
            (!state_->navigation && !state_->read && !state_->busy))
            return false;
        state_->cancelled = request;
        stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
    return true;
}
std::optional<FilesPaneReply> FilesPaneService::take() {
    std::lock_guard lock(state_->mutex);
    state_->signalled = false;
    auto result = std::move(state_->reply);
    state_->reply.reset();
    return result;
}
void FilesPaneService::request_stop() noexcept {
    std::optional<std::stop_source> stop;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->navigation.reset();
        state_->read.reset();
        state_->reply.reset();
        stop = state_->active_stop;
    }
    if (stop)
        stop->request_stop();
    state_->condition.notify_one();
}
void FilesPaneService::join() {
    request_stop();
    if (state_->worker.joinable()) {
        if (state_->worker.get_id() == std::this_thread::get_id())
            throw std::logic_error("pane service cannot join itself");
        state_->worker.join();
    }
}
bool FilesPaneService::running() const noexcept { return state_->alive.load(std::memory_order_acquire); }
bool FilesPaneService::working() const {
    std::lock_guard lock(state_->mutex);
    return state_->busy || state_->navigation || state_->read;
}
FilesPaneWorkerFault FilesPaneService::fault() const noexcept {
    return state_->worker_fault.load(std::memory_order_acquire);
}
} // namespace ck::vision
